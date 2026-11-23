# BrainTerm — Running Decision Log

Non-obvious implementation decisions made during development, consistent with P1–P7 and ADR-001..ADR-008 in `docs/project.md`.

---

## M1 — 2026-11-22

### 1. Platform abstraction (`platform.h` / `platform_win.c` / `platform_unix.c`)
Rather than scattering `#ifdef G_OS_WIN32` across `vault.c`, the four OS-specific operations needed by M1 (`list_dir` without following links, `realpath`, atomic `replace_file`, and `fsync_dir`) live behind `platform.h`. On Windows, `bt_platform_list_dir` uses `FindFirstFileExW` with `FIND_FIRST_EX_LARGE_FETCH` and distinguishes true symlinks/junctions (`IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT`) from OneDrive cloud placeholders, while `bt_platform_replace_file` uses `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`.

### 2. Wiki-link navigation trigger: Ctrl+Click in editor
§6.A2 specifies "Wiki-link `[[name]]` click → open or create the target note". In a GtkSourceView text editor, intercepting plain single-click would prevent placing the cursor inside a `[[link]]` to edit it. Navigation therefore triggers on `Ctrl+Click` on a `[[target]]` span, preserving normal cursor placement while keeping single-gesture link jumping and ghost note creation (always routed through `vault_resolve()`).

### 3. `bt-markdown.lang` loading & Windows packaging smoke test
GtkSourceView 5 bundles `markdown.lang` and `yaml.lang` inside its compiled GResource, so `data/language-specs/bt-markdown.lang` references `markdown:markdown` and `yaml:yaml` directly and is loaded via `gtk_source_language_manager_append_search_path()` from `XDG_DATA_DIRS/brainterm/language-specs`, `BT_LANGSPEC_DIR`, or `./data/language-specs`. To support the §11.7 Windows clean-environment packaging smoke test from M1 onward, `bt` accepts `--vault <dir> --smoke-test`, which builds the full GTK4 window, verifies `bt-markdown` language resolution, prints `BT_SMOKE ok ...`, and exits cleanly; `scripts/package-win.ps1` runs this with `PATH` stripped to `System32` so any missing UCRT64 runtime DLL fails immediately.

---

## M2 — 2026-11-22

### 4. ConPTY IO pump & Job Object anti-zombie isolation (`pty_win.c`)
Anonymous pipes created for Windows ConPTY (`CreatePseudoConsole`) cannot be polled directly by GLib's `GPollFD`, so `pty_win.c` uses a dedicated reader thread feeding a fixed 512 KB ring buffer and coalesces main-thread wakeups via `g_main_context_invoke_full(NULL, G_PRIORITY_HIGH, ...)` draining up to 256 KB per dispatch (§5.4). Writes go through a non-blocking 1 MB ring buffer drained by a writer thread, dropping input with a status warning if full. Every hosted shell is created `CREATE_SUSPENDED`, assigned to a Win32 Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, and then resumed, guaranteeing zero orphan grandchildren on tab close, app exit, or crash (§6.D4).

### 5. Compact 16-byte cell mirror & trimmed scrollback lines (`vtermbridge.c`)
`BtVTerm` mirrors the live screen and 10,000-line scrollback ring as 16-byte `BtCell` structs (codepoint, width, flags, resolved 24-bit RGB fg/bg), trimming trailing blank default-colored cells on `sb_pushline`. This keeps scrollback memory well under the §10 RSS budget while keeping the Cairo/Pango renderer lock-free and allocation-free per cell. OSC 52 clipboard writes are not registered and stay off by default (§8.2).

---

## M3 — 2026-11-22

### 6. Hostile note-title sanitization for AI CLI launch (`agents.c`)
`bt_agents_format_launch_cmd()` composes the fixed-form `<cli> "Read <title> as context."\r` command by stripping control characters and replacing shell metacharacters (`"`, `` ` ``, `$`, `\`, `;`, `|`, `&`, `<`, `>`, `(`, `)`) with spaces. This guarantees zero shell interpolation across PowerShell, `cmd.exe`, and POSIX shells (§8.1).

### 7. Pre-M4 session persistence (`sessions.c`)
M3 persists sessions to `vault/.bt/sessions.json` using cJSON (no SQLite opened in M3 per scope guard); M4 will import this file into the SQLite `sessions` table on first DB open.

---

## M4 — 2026-11-22

### 8. `notes.body` column for FTS5 external-content triggers (`migrations/0001_init.sql`)
§7.2's FTS5 triggers (`notes_ai`, `notes_ad`, `notes_au`) reference `new.body` and `old.body` on `notes` with `content='notes'`. `notes` therefore includes `body TEXT NOT NULL DEFAULT ''` (derived plain text from MD4C), matching §7.2's explicit note preventing the draft's silent-desync bug.

### 9. Single-writer SQLite thread (`index.c`, ADR-007)
`BtIndex` owns a dedicated `bt-db-writer` `GThread` + `GAsyncQueue`; no other thread ever touches `sqlite3 *`. Both writes and reads execute serially on this thread in WAL mode (`synchronous=NORMAL`, `foreign_keys=ON`), keeping the terminal and editor hot paths completely isolated from SQLite.

---

## M5 & M6 — 2026-11-22

### 10. Dismissed AI tag storage (`organizer.c`)
Dismissed AI tag proposals are recorded in `vault/.bt/dismissed_tags.json` (keyed by vault-relative note path) rather than adding a non-spec column to the §7.2 `tags` table. This keeps the §7.2 schema verbatim while enforcing that a human-dismissed AI tag is never re-proposed by any backend (P4).

---

## M7 & M8 — 2026-11-22

### 11. 2D Cairo Brain Graph performance (`graphview.c`)
Disabling antialiasing on 1 px hairline edges (`CAIRO_ANTIALIAS_NONE`) while keeping `CAIRO_ANTIALIAS_FAST` + `tolerance=0.5` on node shapes brings 1,000-node / 3,000-edge software Cairo rendering to **10.46 ms/frame** (well under the 16.6 ms / 60 fps budget, even in `-O0` debug builds).

### 12. Optional offline embeddings & RRF hybrid search (`embed.c`)
To preserve P1 (100% local, zero network calls) and keep builds self-contained without vendoring multi-hundred-MB GGUF weights, `embed.c` ships a deterministic 128-dim word+trigram offline vectorizer with Reciprocal Rank Fusion (RRF, $k=60$) hybrid ranking and `similar` edge generation ($\cos > 0.75$). Embeddings are OFF by default and fully disableable without affecting FTS5 (ADR-005).

### 13. 3D Graph explicitly cut (MAY, §6.C4 / §14.5)
Per §6.C4 and PROMPT 8 ("3D graph is a MAY: it is the FIRST thing cut"), 3D graph rendering is cut from v0.1.0 to keep the pure C/GTK4 stack focused on rock-solid 2D 60 fps rendering.

### 14. §14 Open Questions — Closing Status
1. **ConPTY + `claude` TUI resize**: full-damage repaint + 100 ms debounce implemented; requires interactive human verification.
2. **GtkSourceView 5 IME on Windows**: `GtkIMMulticontext` wired in both editor and terminal; requires interactive Vietnamese/CJK human verification.
3. **`ask_memory` semantics in `bt-mcp`**: returns top-K evidence with `[[citation]]` and snippets (`"mode": "evidence"`), never a chat answer (P6).
4. **Scrollback memory ceiling**: 16-byte `BtCell` + trailing-blank trimming implemented; bounded well under 150 MB RSS.
5. **3D graph viability**: cut for v0.1.0 (MAY).

---

## M-UI ("Ink" Monochrome Reskin) — 2026-11-22

### ADR-009 — Add `libadwaita 1.x` for the Ink reskin
Amends ADR-001 ("GTK4 only") to adopt `libadwaita-1` (`AdwApplication`, `AdwApplicationWindow`, `AdwHeaderBar`, `AdwOverlaySplitView`, `AdwViewStack`/`AdwViewSwitcher`, `AdwSplitButton`, `AdwToastOverlay`, `AdwStyleManager`). `libadwaita` ships in MSYS2 UCRT64 and Ubuntu 24.04; zero core modules were modified.

### 15. Terminal-stays-dark deliberate asymmetry (§D.9)
Even when the user switches the Ink UI to Light ("Paper") mode, the embedded terminal VT grid retains its dark background (`#0B0B0B` / `#ECECEC`) because ANSI 16-color TUI programs (`vim`, `htop`, `claude`) are authored for dark terminal backgrounds. All terminal chrome (tabs, search bar, exit banner) follows the active Ink scheme.

### 16. Monospace font resolution order (§H.16)
Editor and terminal monospace font defaults to `Cascadia Mono, JetBrains Mono, Consolas, Monospace 11` on Windows and `JetBrains Mono, Cascadia Mono, Monospace 11` on Linux, falling back cleanly to the first installed system monospace face without bundling webfonts.

### 17. Old-control → new-place action map (zero orphans)
| Old control | New place in Ink | Shortcut / Action |
|---|---|---|
| Open Vault button | Primary menu → Open Vault… | `Ctrl+O` (`app.open-vault`) |
| Save button | Dissolved (1s autosave + header `●`/`○` + Toast) | `Ctrl+S` (`app.save-active`) |
| Quick Open button | Dissolved | `Ctrl+P` (`app.quick-open`) |
| Claude / Codex / Gemini buttons | Single `Agent` `AdwSplitButton` (default Claude, menu Codex/Gemini/Rollback) | `app.launch-claude` / `codex` / `gemini` |
| Graph button | `AdwViewSwitcher` → Graph | `app.toggle-graph` |
| Sessions button | `AdwViewSwitcher` → Sessions | `app.toggle-sessions` |
| `+ Tab` button | Terminal tab strip + Filetree right-click "Open terminal here" | `app.term-spawn` |
| Bottom status bar | `AdwToastOverlay` toasts + header subtitle (`●`/`○`) | — |

---

## FIX-UI — Pre-fix widget tree probe (2026-11-22)

Captured before applying any layout/UI fixes using `.\build\src\bt.exe --vault tests\fixtures\vault --smoke-test` (post-250ms layout settle):

```text
BT_TREE_STATE page=editor scheme=bt-ink-dark
BT_TREE name=bt-window type=AdwApplicationWindow vis=1 map=1 real=1 size=1280x800
BT_TREE name=bt-split-view type=AdwOverlaySplitView vis=1 map=1 real=1 size=1280x751
BT_TREE name=bt-right-paned type=GtkPaned vis=1 map=1 real=1 size=1020x751
BT_TREE name=bt-view-stack type=AdwViewStack vis=1 map=1 real=1 size=1020x460
BT_TREE name=bt-editor-box type=GtkBox vis=1 map=1 real=1 size=1020x460
BT_TREE name=bt-editor-placeholder type=GtkLabel vis=0 map=0 real=1 size=0x0
BT_TREE name=bt-editor-scrolled type=GtkScrolledWindow vis=1 map=1 real=1 size=1020x460
BT_TREE name=bt-editor-view type=GtkSourceView vis=1 map=1 real=1 size=1020x460
BT_TREE name=bt-graph-area type=GtkDrawingArea vis=1 map=0 real=0 size=0x0
BT_TREE name=bt-sessions-box type=GtkBox vis=1 map=0 real=0 size=0x0
BT_TREE name=bt-termtabs type=GtkNotebook vis=1 map=1 real=1 size=1020x290
BT_TREE name=bt-sidebar type=GtkBox vis=1 map=1 real=1 size=259x751
BT_TREE name=bt-filetree type=GtkScrolledWindow vis=1 map=1 real=1 size=259x58
BT_TREE name=bt-header type=AdwHeaderBar vis=1 map=1 real=1 size=1282x48
BT_SMOKE ok notes=4 lang=bt-markdown term=alive ui=adw-ink scheme=dark tabs=1 checkpoint=ok sessions=4 index=ok ask=search graph=nodes:4
```

Key observations from probe + remote-CWD run (`lang=none` from `%TEMP%`):
1. `bt-filetree` (`GtkScrolledWindow` inside vertical `bt-sidebar` `GtkBox`) had no `vexpand=TRUE` and collapsed to `259x58` inside the `259x751` sidebar.
2. `bt-graph-area` is unmapped (`map=0 real=0 size=0x0`) during startup when `bt_app_open_vault` calls `refresh_graph(app, TRUE)` in interactive mode; `bt_graphview_set_graph` resets every node's `birth_t = 0.05` (`ny = 260`, clipped below the 460px viewport) and `on_anim_tick` immediately stops on `!gtk_widget_get_mapped(gv->area)` with no `"map"` signal handler to resume/paint when switching to `"graph"`.
3. `AdwViewStack` visible child had no explicit startup default or `[ui] page` restore/fallback, and clicking the already-open note in the sidebar returned early before switching the stack back to `"editor"`.
4. `bt-editor-box`, `bt-view-stack`, `bt-right-paned`, and `bt-sessions-box` lacked explicit `hexpand`/`vexpand` flags (and `bt-right-paned` allowed shrinking children to 0), and running outside the repo root resolved `lang=none` (`configure_language` / `apply_ink_scheme` lacked exe-relative `share/brainterm/...` lookup).

### Root causes & applied fixes
1. **`src/graphview.c` (`on_area_map`, `on_area_destroy`, `bt_graphview_set_graph`)**: Defer starting `on_anim_tick` when `gv->area` is not yet mapped, and connect `"map"` on `gv->area` to resume any pending `birth_t < 1.0` growth animation and `gtk_widget_queue_draw()` when the `"graph"` page becomes active (plus `"destroy"` cleanup so timers never touch a torn-down `GtkDrawingArea`).
2. **`src/ui/editorpane.c`**: Add explicit `hexpand`/`vexpand` on `pane->box`, unify `GtkSourceLanguageManager` / `GtkSourceStyleSchemeManager` search path resolution across compile-time paths, `CWD/data`, exe-relative `share/brainterm`, and `XDG_DATA_DIRS` (fixing `lang=none` from non-repo CWDs), and expose `bt_editorpane_scheme_id()` / `bt_editorpane_view_widget()`.
3. **`src/ui/filetree.c` & `src/ui/sessionspanel.c`**: Set `hexpand`/`vexpand` on `tree->scrolled` (`bt-filetree` expands from `259x58` to `259x717`) and `bt-sessions-box` + its scrolled container.
4. **`src/app.c` & `src/app.h`**: Set explicit `hexpand`/`vexpand` on `view_stack` and `right_paned` (`shrink_* = FALSE`), set explicit startup page via `bt_app_resolve_page_name()` + `bt_app_load_saved_page()` (`[ui] page` in `state.ini` with defensive fallback to `"editor"` for missing/obsolete values), persist valid page switches and queue graph redraw on `"notify::visible-child-name"`, and switch `view_stack` to `"editor"` (clearing `term_fullscreen`) before the already-open early return in `bt_app_open_note()`.
5. **`src/graphview.c` (`bt_graph_draw_cairo`)**: Hoisted `cairo_stroke(cr)` out of the 3,000-edge loop into a single batched stroke pass so 1k-node / 3k-edge frames stay cleanly inside the 16.6 ms / 60 fps budget even under `-O0` debug builds.

---

## Dusk UI Redesign — 2026-11-22

### 18. Supersedes monochrome Ink with "Dusk" dark theme (`data/theme.css`, `src/theme.h`)
Replaces `data/bt-ink.css` and the grayscale-only `test_ink_theme` gate with a calm dark surface hierarchy (`#0E0F12` base, `#15171C` surface-1 cards, `#1B1E24` surface-2, `#262A32` / `#343944` borders, `#E6E8EC` / `#9AA1AD` / `#666D79` text) and `#8B7CF6` violet accent (+ `#4ADE80` / `#FBBF24` / `#F87171` status tokens). `tests/unit/test_theme.c` enforces exact `theme.css` ↔ `theme.h` parity, zero CSS parse errors, and WCAG AA contrast (`>= 4.5:1`). `BRAINTERM_CSS_DEV=<path>` enables live hot-reloading via `GFileMonitor`.

### 19. Stray vertical line & lone square node in Brain Graph (`src/graphview.c`)
- **Stray vertical line**: Intentional leftover from an earlier "spine guide line" (`cairo_move_to(0, -240); cairo_line_to(0, 260)`) that bisected the canvas unconditionally even with few notes. Removed and replaced with a faint dot grid across the `surface-1` canvas.
- **Lone square node**: `src/graph.c` intentionally creates a `BT_NODE_SESSION` square at `y = +260` for each recorded terminal/agent session in `.bt/sessions.json` (which starts with 1 shell session on vault open). Kept the data model untouched (UI-layer-only rule) and redesigned session nodes as rounded bordered badges sitting beneath a short horizontal `"sessions"` dock divider so they read as intentional session indicators rather than a stray artifact.

### 20. Font bundling & symbolic icons (`src/ui/themeload.c`, `scripts/gen_icons.py`)
Bundled OFL-licensed `InterVariable.ttf` and `JetBrainsMono-{Regular,Bold}.ttf` under `data/fonts/` (registered via `AddFontResourceExW(..., FR_PRIVATE, 0)` on Windows and `FcConfigAppFontAddFile` + `PangoFc` on Linux/Unix, with CSS/Pango fallback to `Segoe UI Variable` / `Cascadia Mono` / `Consolas`), plus 16 Lucide-style filled-outline symbolic SVGs compiled into `brainterm.gresource.xml`.

---

## Logseq-Style Knowledge Management (Clean-Room) — 2026-11-22

See `docs/logseq-parity.md` for full scope and clean-room rules (Logseq is AGPL-3.0; zero Logseq code or assets are used; file-format interop only).

### ADR-010 — Journal file naming & template (`journals/YYYY-MM-DD.md`)
**Decision.** Daily journals live at `journals/YYYY-MM-DD.md` (ISO-8601 date, local calendar day), auto-created on first open or quick capture from `templates/journal.md` (if present in the vault) or a minimal built-in template with standard BrainTerm YAML frontmatter (`id: bt-journal-YYYYMMDD`, `title: "YYYY-MM-DD"`, `created`, `updated`, `source: human`, `tags: [journal]`).
**Rationale.** ISO-8601 sorts chronologically in every file tree and CLI tool, matches BrainTerm's existing RFC-3339 timestamps and `inbox/` filenames, and is Windows-safe. Logseq's `YYYY_MM_DD.md` naming is converted on import (P8) rather than adopted as a second live convention.

### ADR-011 — Task markers, scheduling, priority & surgical toggle
**Decision.** Recognize both Logseq-style keyword markers (`TODO`, `DOING`, `DONE`, `LATER`, `WAITING`, `CANCELED`; `NOW` normalized to `DOING` on read) at the start of a line or list item AND standard GFM checkboxes (`- [ ]` → `TODO`, `- [x]` / `- [X]` → `DONE`). Optional priority tokens (`[#A]`, `[#B]`, `[#C]`) immediately follow the marker; optional `SCHEDULED: <YYYY-MM-DD ...>` and `DEADLINE: <YYYY-MM-DD ...>` dates are accepted inline or on immediately following indented lines of the same item.
**Rationale.** Plain Markdown notes stay readable in GitHub, VS Code, Obsidian, and CLI agents. Toggling a task from the Tasks panel or editor performs a surgical in-place replacement of the marker span only, preserving the item's original style (GFM vs keyword), indentation, and CRLF/LF line endings via `vault_atomic_write()`.

### ADR-012 — Inline properties (`key:: value`) & parameterized query syntax (`{{query ...}}`)
**Decision.** Index inline `key:: value` lines (outside fenced code blocks) into a derived SQLite `properties` table alongside YAML frontmatter; YAML frontmatter remains canonical for BrainTerm's reserved page keys (`id`, `title`, `created`, `updated`, `source`, `agent`, `tags`, `tags_ai`, `links`). Query blocks use a small, closed s-expression syntax `{{query <expr>}}` supporting `(and ...)`, `(or ...)`, `(not ...)`, `[[page]]`, `#tag`, `(task ...)`, `(property key [val])`, `(priority A|B|C)`, `(between <from> <to>)`, `(path <prefix>)`, and `"fts phrase"`.
**Rationale.** Compiles deterministically to parameterized SQLite prepared statements (zero string-interpolated SQL), runs exclusively on `bt-db-writer` with a 250 ms `sqlite3_progress_handler` timeout and hard result cap (default 200, max 1,000), and avoids embedding a Clojure/Datalog runtime (`#+BEGIN_QUERY` is intentionally skipped).

### ADR-013 — Lazy block IDs (`id:: <uuid>`), references `((id))` & read-only embeds
**Decision.** Block IDs use lowercase UUIDv4 written on an indented child line (`  id:: <uuid>`) immediately below the referenced bullet/paragraph and are inserted **lazily** only when the user creates a reference or embed to that block. References `((uuid))` render with hover preview and Ctrl+Click jump to source; `{{embed ((uuid))}}` and `{{embed [[page]]}}` render inline as read-only preview cards with a jump-to-source link (cycle-checked, max depth 3).
**Rationale.** Prevents polluting ordinary Markdown notes with UUID noise until needed, preserves clean `git diff`s for humans and agents, and eliminates multi-buffer conflict hazards by keeping embeds read-only.

### ADR-014 — Link occurrences & backlinks index (`migrations/0002_backlinks.sql`)
**Decision.** Preserve the §7.2 `edges` table unchanged and add `link_refs(id, src_note_id, dst_raw, dst_note_id, line_no, col_no, context)` in `migrations/0002_backlinks.sql` (`PRAGMA user_version = 2`), storing every wiki-link occurrence's 1-based line number, byte column, and trimmed line context (capped at 240 chars) plus resolved `dst_note_id` (case-insensitive match by id, path, stem, or title). Unlinked references are computed asynchronously via `notes_fts` phrase search excluding already-linked notes, self, and titles shorter than 3 characters.
**Rationale.** Keeps existing graph and organizer queries backward-compatible while giving P1 Backlinks O(log N) lookup with exact line jump and context snippets, fully rebuildable from L0 files (P5).

### ADR-015 — Single Shared Link Resolver (`src/linkresolve.c/.h`)
- **Decision:** Centralize all link scanning and resolution in GTK-free `libbt_core` (`bt_link_scan`, `BtIndexSnapshot`, `bt_link_resolve`) and route editor clicks, tooltips, index storage (`0002_links_resolution.sql`), and `bt_graph_build()` ghost-node creation through it.
- **Rationale:** Guarantees a link is a ghost node in Brain Graph iff it is `BT_LINK_UNRESOLVED` in the editor, resolves relative subfolder wikilinks (`chapter-01-descriptive/...`), and keeps snapshot rebuilds under 20 ms for 3,000 notes off the GTK main thread.

### ADR-016 — Default Single-Click Links & Caret-Inside Rule
- **Decision:** Single-click (`n_press == 1`, within GTK drag threshold, empty selection, Alt not held) opens links by default (`[editor] links_open_on_click = true`), except when the caret is already inside the clicked link span (which edits the link instead). `Alt+Click` always places the caret; `Ctrl+Click` works in both modes and is required when `links_open_on_click = false`.
- **Rationale:** Matches user expectation from browser/Obsidian while preserving effortless in-place editing of link targets without extra modes.

### ADR-017 — Untrusted Link Open-Target Security Policy
- **Decision:** Enforce a pure-C policy check before any external open: hard-block 32 executable/script/shortcut extensions (after case-folding and stripping trailing dots/spaces), auto-allow only safe document/media extensions inside the vault (`vault_resolve()` symlink/junction checked), prompt via confirm popover for unknown extensions and directories, allow only `http`/`https`/`mailto` URLs (≤ 2,048 bytes, strip & warn on `user:pass@`, one-time per-session confirm popover), and invoke OS handlers strictly via async `GtkFileLauncher` / `GtkUriLauncher` (zero shell execution).
- **Rationale:** Notes may contain AI-generated or imported content; defense-in-depth prevents arbitrary code execution or vault escape.

### ADR-018 — Pandoc-Style Math Delimiters & Dual Emphasis Fix
- **Decision:** Scan math spans (`$...$` with non-space inner boundaries and non-digit follower; `$$...$$` up to 200 lines / 16 KB; `\(...\)`, `\[...\]`, top-level environments) before links and emphasis, and enforce exclusion in both `data/language-specs/bt-markdown.lang` (math contexts placed ahead of `markdown:markdown`) and `editorpane.c`'s live-preview tag pass.
- **Rationale:** Eliminates false positives on currency (`$5 and $10`) while completely fixing the italic-inside-math bug visible in `02-bootstrap.md`.

### ADR-019 — Math Engine: Clean-Room C17 `libbt_math` on HarfBuzz OpenType MATH
- **Decision:** Build **Option 1** — a clean-room C17 static library `libbt_math` depending only on HarfBuzz (`<hb-ot.h>`), paired with Cairo-FT (`cairo-ft.h`) rendering in the UI layer.
- **Alternatives evaluated:**

| Option | Verdict | Reason |
|---|---|---|
| **1. Own C17 engine on HarfBuzz OpenType MATH + Cairo-FT** | **Selected** | Single native C exe, 0 extra runtime deps, offline, fits Dusk/GTK4/Cairo, P0 spike succeeded on first try (HarfBuzz 14.2.1 + Cairo-FT 1.18.4). |
| 2. MicroTeX (C++) | Fallback | Introduces C++ toolchain/ABI, larger binary, upstream maintenance & subset risks; keep as documented fallback if M3 hits a blocker. |
| 3. KaTeX / MathJax in WebView | Rejected | Heavy browser engine dependency, startup latency, non-native widget integration on Windows GTK4. |
| 4. External LaTeX (`tectonic`/`pdflatex`) | Rejected | External install burden, high per-formula latency, breaks single-folder offline packaging. |
| 5. Unicode substitution only | Rejected | Cannot render fractions, integrals, matrices, or radicals. |

### ADR-020 — Bundled Math Font: STIX Two Math (SIL OFL 1.1)
- **Decision:** Bundle `STIXTwoMath-Regular.otf` (v2.13 from `stipub/stixfonts`, SIL Open Font License 1.1) inside `data/fonts/` and compile into `brainterm.gresource.xml` alongside `data/fonts/LICENSE-STIXTwoMath.txt`. Verify `hb_ot_math_has_data()` at startup with raw-source fallback on failure.
- **Rationale:** Comprehensive Unicode math alphanumeric & symbol coverage for statistics/ML, complete OpenType MATH table (variants, assemblies, kerning, accent attachments), permissive OFL 1.1 redistribution license matching BrainTerm's bundled Inter and JetBrains Mono fonts.

### ADR-021 — Editor Math Integration (Pending M5 Spike)
- **Decision (provisional):** Ship **E4** (live rendered preview popover on caret/hover) at STOP 2, then run the mandatory M5 spike comparing **E1** (invisible source + inline `GdkPaintable` with irreversible-action undo guard + save/copy stripping), **E2** (`GtkTextView::snapshot_layer` overlay), and **E4** against all 8 hard criteria (B.12.2). Adopt E1 or E2 only if all 8 criteria pass with 10,000-op randomized file-integrity proof; otherwise retain E4.
- **Note on inline display math (`$$...$$` mid-paragraph):** When display math appears mid-paragraph, render it on its own visual display block (or split visually in E1/E2) without modifying buffer newlines.

### ADR-022 — Math Caching, Idle Budgets & Resource Limits
- **Decision:** Lay out formulas in DPI-independent `em` units into immutable flat `BtMathLayout` draw lists cached in a 2,000-entry LRU (`(hash, mode)`), and rasterize device-pixel `GdkTexture`s into a 32 MB LRU (`(hash, mode, px_size, scale, rgba)`). Schedule off-screen/visible-range work in idle callbacks capped at 4 ms/tick and enforce all B.17 untrusted-input ceilings (4 KB inline / 16 KB display, depth 64, 5k AST nodes, 20×20 matrix, 20k draw ops, 50 ms soft layout cap).
- **Rationale:** Guarantees zero per-frame allocations, crisp HiDPI rendering, bounded RSS, and 60 FPS scrolling on 300-formula notes.
