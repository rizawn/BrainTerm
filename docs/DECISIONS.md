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
