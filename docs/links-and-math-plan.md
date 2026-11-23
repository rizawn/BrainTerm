# BrainTerm — Clickable Links & LaTeX Math Implementation Plan (Phase P0)

> Date: 2026-11-23  
> Status: **Phase P0 complete (read-only recon + HarfBuzz MATH spike + ADR drafts) — awaiting STOP 0 approval.**  
> Scope: Part A (Clickable Links, Phases L1–L3) and Part B (LaTeX Math Rendering, Phases M1–M6). Zero production code written in P0.

---

## 1. Environment & Toolchain Recon (Measured)

| Component | Measured Value |
|---|---|
| OS / Host | Windows 11 x86_64, MSYS2 UCRT64 (`C:\msys64\ucrt64\bin`) |
| Compiler / Build | GCC 16.1.0, Meson 1.11.1, Ninja 1.13.2 (`-std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Dwerror=true`) |
| GTK4 / libadwaita | `gtk4` **4.22.4**, `libadwaita-1` **1.9.1**, `gtksourceview-5` **5.18.0**, `glib-2.0` **2.88.2** |
| HarfBuzz / FreeType / Cairo | `harfbuzz` **14.2.1** (`hb-ot-math.h` present via `<hb-ot.h>`), `freetype2` **26.6.20**, `cairo` **1.18.4** (`CAIRO_HAS_FT_FONT = 1`, `cairo-ft.h` present) |
| GTK4 Launchers & TextView APIs | `GtkFileLauncher`, `GtkUriLauncher`, `gtk_text_buffer_insert_paintable`, `gtk_text_buffer_begin_irreversible_action`, `GtkTextView::snapshot_layer` all present |
| ASan / UBSan on MSYS2 UCRT64 GCC | **Not available** (`ld: cannot find -lasan / -lubsan`). Mitigated via strict `-Werror` flags, bounded arena allocators, fuzz harnesses, and Linux/WSL CI ASan/UBSan. |
| GPU / Driver (baseline) | NVIDIA GeForce RTX 5060 Laptop GPU/PCIe/SSE2, OpenGL 4.6.0 NVIDIA 616.64 |

### 1.1 Read-only `life2` Vault Measurements

Scanned `C:\Users\Riza Wahyu Nugraha\Documents\coolyeah\persemester\sem 1-2\life2` read-only (zero writes):

| Metric | Count |
|---|---:|
| Total files | 348 |
| Markdown notes (`.md`) | 310 |
| Notes containing links | 170 |
| Raw `[[wikilinks]]` | 887 |
| Raw Markdown `[text](dest)` links | 264 |
| Notes containing math | 77 |
| Raw inline `$...$` spans | 915 |
| Raw display `$$...$$` blocks | 192 |
| Total math formulas | **1,107** |

Confirmed both screenshot notes verbatim:
- `statistical-statistics-notes/MathStats.md`: Markdown tables with relative folder wikilinks (`| 01 | [[chapter-01-descriptive/01-data-types-collection.md]] | ... |`).
- `statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md`: inline `$...$` inside numbered/bullet lists, display `$$(\hat{\theta}^*_{(\alpha/2)}, \hat{\theta}^*_{(1-\alpha/2)})$$`, and conditional-probability `|` pipes inside math.

### 1.2 P0 HarfBuzz OpenType MATH + Cairo-FT Spike Result

Executed throwaway C17 spike (`spike_hb_math.c`, outside repo) loading an OpenType MATH font from memory (`hb_blob_create` + `FT_New_Memory_Face` + `cairo_ft_font_face_create_for_ft_face`):

- `HarfBuzz compile=14.2.1 runtime=14.2.1`, `Cairo runtime=1.18.4`
- `hb_ot_math_has_data(face) == 1` (`upem = 2048`)
- MATH constants read directly via `hb_ot_math_get_constant()`:
  - `AxisHeight = 585 (0.286 em)`
  - `FractionRuleThickness = 133 (0.065 em)`
  - `FractionNumeratorDisplayStyleShiftUp = 1550 (0.757 em)`
  - `FractionDenominatorDisplayStyleShiftDown = 1370 (0.669 em)`
  - `FractionNumDisplayStyleGapMin = 260`, `FractionDenomDisplayStyleGapMin = 260`
  - `ScriptPercentScaleDown = 73%`, `ScriptScriptPercentScaleDown = 60%`
- Rendered `\frac{a+b}{c}` cleanly to a 165×160 PNG on Dusk `#0E0F12` with `#E6E8EC` glyphs and exact fraction bar positioning (`status = 0`).
- **Gotcha discovered:** HarfBuzz 14.2.1 requires `#include <hb-ot.h>` (direct `#include <harfbuzz/hb-ot-math.h>` triggers `#error`).

*(Note: P0 spike used Windows' local Cambria Math face 1 strictly as an unredistributed local test font. Production will bundle **STIX Two Math** `STIXTwoMath-Regular.otf` v2.13 under SIL OFL 1.1.)*

---

## 2. Repo vs. Brief Discrepancies (Repo Wins)

| # | Brief Assumption | Actual Repo State | Plan Adjustment |
|---|---|---|---|
| 1 | Plain clicks did nothing; reveal-on-caret hides `[[ ]]` | `editorpane.c:612` explicitly requires `GDK_CONTROL_MASK` (`Ctrl+Click`). `[[ ]]` brackets are dimmed via `t_syntax`, never hidden. | Implement single-click default + caret-inside rule (A.6) while keeping bracket dimming non-destructive. |
| 2 | Existing `bt_app_open_or_create_wikilink()` creates notes safely | `src/app.c:594-622` silently writes a bare `# target\n\n` stub without confirmation or BrainTerm YAML frontmatter. | Replace with A.11 confirm popover + standard YAML frontmatter template. |
| 3 | Emphasis bug comes from highlighter or live-preview tags | **Both** cause it: `data/language-specs/bt-markdown.lang` has no math contexts AND `editorpane.c:249` applies `t_italic` to `*...*` without skipping math. | Fix both in M1 (`bt:math-inline` / `bt:math-display` before `markdown:markdown`, plus `editorpane.c` math-span exclusion). |
| 4 | Migration `000N_links_resolution.sql` | `migrations/` currently has only `0001_init.sql` (`BT_SCHEMA_VERSION = 1`). Draft ADR-014 (`0002_backlinks.sql`) in `DECISIONS.md` was never implemented in `index.c`. | Add `migrations/0002_links_resolution.sql` bumping `BT_SCHEMA_VERSION` to `2`. |
| 5 | Frontmatter `aliases` field available | `src/frontmatter.h` parses `id/title/created/updated/source/agent/tags/tags_ai/links`; `aliases` currently lands in `unknown`. | Add `GPtrArray *aliases` to `BtFrontmatter` in L1 while preserving existing serialization order. |
| 6 | Graph ghost nodes share resolver without touching `graph.c` layout | `src/graph.c:148-184` resolves link edges using ad-hoc `path_map`/`stem_map` (ignores relative-to-source paths & aliases). | Update **only** edge resolution inside `bt_graph_build()` to call `bt_link_resolve()` (zero changes to `bt_graph_layout()`). |
| 7 | Status-bar one-line messages | `bt_app_set_status()` currently displays `AdwToast`; bottom status bar has `status_graph_notice`. | Add a dedicated muted status-bar info label for link-open/blocked/math-error counts (no toast spam). |
| 8 | ASan/UBSan available locally | MSYS2 UCRT64 GCC 16.1.0 lacks `libasan` / `libubsan`. | Enforce `-Dwerror=true`, arena bounds checks, deterministic fuzzing on Windows; run ASan/UBSan on Linux CI. |

---

## 3. Phased Execution Plan

### Part A — Clickable Links

- **Phase L1 — Core Resolver, Link Parser & Index Migration**
  - Add `src/linkresolve.h/.c` (GTK-free, `libbt_core`):
    - `bt_link_scan()` & `bt_link_scan_range()` (wikilinks with `\|` table-safe alias handling, Markdown links with balanced parens & `%`-decoding, bare `http`/`https` URLs, `mailto:`, frontmatter `links`, skipping fenced/inline code & HTML comments; sorted `GArray` of `BtLinkSpan`).
    - `BtIndexSnapshot` immutable hash-indexed view (path, basename, title, aliases; NFC + platform case-folding; < 20 ms rebuild at 3k notes).
    - `bt_link_resolve()` implementing all A.3 stages (1–13), Windows unsafe-path rejections (UNC, drive letters, ADS, reserved devices, trailing dots/spaces, control chars), and `vault_resolve()` symlink/junction containment.
    - Pure security policy helper `bt_link_check_open_policy()` implementing A.9 block-list (32 executable extensions), allow-list, confirm-required classification, scheme checks, 2,048-byte URL cap, and userinfo detection/stripping.
  - Extend `src/frontmatter.h/.c` to parse `aliases`.
  - Add `migrations/0002_links_resolution.sql` (`link_resolutions` table: `src_note_id`, `raw_target`, `resolved_path`, `status`, `line_no`, `kind`) and wire re-resolution on upsert/delete/rename.
  - Wire `bt_graph_build()` edge resolution to `bt_link_resolve()` so ghost nodes match `BT_LINK_UNRESOLVED` 1-to-1 (without touching `bt_graph_layout()`).
  - Add `bt.exe --selftest-links <vault_dir>` (headless, read-only).
  - Add CMocka tests: Group R (R01–R38), Group P (parser + 10k-link perf + seeded incremental-vs-full fuzz), Group U (all A.9 security rules), and graph-ghost equivalence test.

- **Phase L2 — Editor Interaction, Hover, Open Targets & Navigation History**
  - Add Dusk link tokens (`@bt_link`, `@bt_link_hover`, `@bt_link_broken`, `@bt_link_blocked`) to `data/theme.css`, `src/theme.h`, `src/theme.c`, and `tests/unit/test_theme.c`.
  - Refactor `src/ui/editorpane.c`:
    - Sorted `BtLinkSpan` array with incremental paragraph rescan + binary-search lookup on byte offset.
    - `GtkGestureClick` (button 1 + middle button) & `GtkEventControllerMotion`: drag-threshold check, empty-selection check, caret-inside-link edit rule, `Alt+Click` caret-only, `links_open_on_click` (`click` default vs `ctrl_click`).
    - Pointer cursor on hover (only while Ctrl held in `ctrl_click` mode), hover color tag, broken underline (`PANGO_UNDERLINE_ERROR` / `PANGO_UNDERLINE_LOW`) + blocked danger underline, status-specific tooltips (plus `"AI draft"` annotation inside `## AI draft`).
    - Flush unsaved edits via `vault_atomic_write()` before navigating; abort navigation on flush failure with status-bar error.
    - Anchor jump: scroll heading to ~1/3 viewport height + 600 ms subtle flash (skipped under Reduce Motion).
  - Implement async open-target dispatcher (`GtkFileLauncher` / `GtkUriLauncher`, zero shell calls) with A.9 enforcement: session-scoped one-time external URL confirm popover (with userinfo warning), non-allow-listed file confirm popover, directory reveal confirm popover.
  - Implement per-session navigation history (`BtNavHistory`, cap 100, `(relpath, caret_offset, scroll_y)`): `Alt+Left` / `Alt+Right`, mouse back/forward buttons, pushing from link clicks, sidebar, and Ctrl+P, skipping deleted notes.

- **Phase L3 — Create-Note Flow, Ambiguity Chooser, Keyboard, Settings & STOP 1 (Completed)**
  - Recon & shortcut conflict audit: commits `1e0dcc7` (`docs: L3 recon (L3 pre)`) and `e89e1f8` (`docs: L3 shortcut audit (L3 pre)`).
  - Unresolved link planner, template, create-only atomic commit, index re-resolution, and `BtCreatePopover` UI: commits `1befaf2` (`L3-1a`), `6c2b9f5` (`L3-1b`), `ddc13c5` (`L3-1c`), `66ffcae` (`L3-1d`), and `33db34b` (`L3-1e`).
  - Ambiguity chooser selection model (`BtChooserModel`) and `BtChooserPopover` UI: commits `18cd165` (`L3-2a`), `e1712a5` (`L3-2b`), and `f5582d0` (`L3-2b fix`).
  - Keyboard shortcuts (`Ctrl+Enter`, `Ctrl+Alt+Right`, `Ctrl+Alt+Left`): commit `2759009` (`L3-3`).
  - App menu toggle `Confirm external URLs` persisted under `[editor] confirm_external_urls` in `state.ini`: commit `10ece82` (`L3-4`).
  - Legacy stub wikilink writer routed away and removed with grep proof: commits `aeef6df` (`L3-5a`) and `d3720f6` (`L3-5b`).
  - Added `vault-template/examples/links-demo.md` and duplicate-basename demo folders: commit `052124b` (`L3-6a`).
  - Updated `docs/DECISIONS.md` (`ADR-023`..`ADR-026`), `docs/project.md`, and `CHANGELOG.md` (`L3-6b`), followed by full verification and Windows package smoke test (`L3-6c`) → **STOP 1**.

### Part B — LaTeX / Math Rendering

- **Phase M1 — Math Delimiter Tokenizer & Emphasis Fix**
  - Create `src/math/meson.build` (`libbt_math`, HarfBuzz-only, zero GTK/Cairo).
  - Implement `src/math/mathtok.h/.c`: `bt_math_scan()` & `bt_math_scan_range()` with pandoc-style `$...$` rules (B.2.2), multi-line `$$...$$` (200-line / 16 KB cap, blockquote `>` stripping), `\(...\)`, `\[...\]`, top-level environments, skipping code/frontmatter/links/URLs/HTML comments, FNV-1a/Wyhash normalized hash, CRLF + BOM safety.
  - Fix emphasis bug in both `data/language-specs/bt-markdown.lang` (`bt:math-display` & `bt:math-inline` before `markdown:markdown`, no child contexts) and `src/ui/editorpane.c` (skip math spans during emphasis & link tagging).
  - Unit tests: Group T (including screenshot lines + 5k-span perf + incremental property test) and Group H (zero emphasis tags in math, intact emphasis outside).

- **Phase M2 — LaTeX Subset Parser to AST**
  - Implement `src/math/mathparse.h/.c`: arena-allocated AST (B.8), table-driven command registry covering all Tier 1 (and feasible Tier 2) commands from B.6, explicit blocklist for TeX programming macros (B.6.10), total parser returning partial AST + precise byte-offset error list, enforcing B.17 limits (4 KB / 16 KB length, 64 depth, 5,000 nodes, 20×20 matrix).
  - Unit tests: Group P (all B.6.9 & Appendix 1 samples, every error class + column check, limits) + Group F tokenizer/parser fuzz harness.

- **Phase M3 — Bundled Font & OpenType MATH Layout Engine**
  - Bundle `STIXTwoMath-Regular.otf` (v2.13, SIL OFL 1.1) in `data/fonts/` + `data/fonts/LICENSE-STIXTwoMath.txt` via GResource.
  - Implement `src/math/mathfont.h/.c` (`hb_blob_create`, `hb_face_create`, `hb_ot_math_has_data()` startup check, cached MATH constants, Unicode math alphanumeric mapping, glyph coverage report).
  - Implement `src/math/mathlayout.h/.c`: em-unit layout engine (D/T/S/SS + cramped styles, full 8×8 TeX atom spacing table + unary `Bin→Ord`, fractions/binom, radicals + glyph assembly, big operators with display variants & limits, stretchy `\left...\right` delimiters, wide accents, matrices/cases/aligned, 20k draw-op cap & 50 ms soft time cap) producing flat immutable `BtMathLayout` draw lists.
  - Unit tests: Group L (all B.10 relative metric invariants + NaN/Inf property check) + `bt.exe --selftest-math <vault_dir>` (with unknown-command histogram) + `tests/bench/bench_math.c`.

- **Phase M4 — Cairo Renderer, Golden Images, Popover Preview & STOP 2**
  - Add Dusk math tokens (`@bt_math_fg`, `@bt_math_source`, `@bt_math_error`, `@bt_math_block_bg`, `@bt_math_preview_bg`) to `theme.css`/`theme.h`/`theme.c`/`test_theme.c`.
  - Implement `src/ui/mathrender.h/.c`: Cairo-FT draw-list replay at device pixels (unhinted, antialiased, theme-colored), LRU layout cache (2,000 entries) + LRU texture cache (32 MB budget) producing baseline-tagged `GdkTexture`s.
  - Add `vault-template/examples/math-gallery.md`, golden PNG test suite (`tests/golden/math/` at 1x & 2x + `scripts/update-golden.ps1`), and E4 live preview popover in editor.
  - Run `--selftest-math` on `life2`, bench JSON, package dist → **STOP 2**.

- **Phase M5 — Editor In-Place Rendering Spike & Integration**
  - Time-boxed throwaway spike evaluating **E1** (invisible source + inline `GdkPaintable` via `gtk_text_buffer_insert_paintable` inside `begin_irreversible_action` with save/copy/offset filtering), **E2** (`GtkTextView::snapshot_layer` overlay), and **E4** (popover fallback) against all **8 hard criteria** (including 10,000-op seeded randomized file-integrity test).
  - Record spike evidence in ADR-021. If E1 or E2 passes all 8 criteria, implement `src/ui/mathview.h/.c` with reveal-on-caret (150 ms hysteresis), live preview popover (120 ms debounce), lazy idle rendering (≤ 4 ms/tick), centered display math with horizontal overflow, and baseline-aligned inline math. Otherwise retain E4 safely and report blockers.

- **Phase M6 — Errors, Macros, Copy-as-Source, Accessibility, Evidence & STOP 3**
  - Error underlines + tooltips + status-bar `Math: N errors in this note`.
  - Per-vault `.bt/macros.json` loader (`src/math/mathmacros.h/.c`: `#1..#9`, depth 32, 8 KB cap, cycle detection, file watcher, read-only to agents).
  - Context menu (`Copy LaTeX source`, `Copy with $ delimiters`, `Show source`), `Ctrl+Shift+M` global toggle persisted in `state.ini`, menu toggle `Render math`.
  - Final `docs/math.md`, `docs/project.md`, `docs/DECISIONS.md`, `CHANGELOG.md`, full evidence capture + bench + `--selftest-math` on `life2` + packaged dist → **STOP 3**.

---

## 4. ADR Drafts (To Be Committed to `docs/DECISIONS.md` Upon Phase Approval)

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

---

## 5. Risk Register (P0 Baseline)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| HarfBuzz MATH API or Cairo-FT missing on MSYS2 | **Retired (Low)** | High | **Verified in P0 spike:** HarfBuzz 14.2.1 + Cairo-FT 1.18.4 + FreeType 26.6.20 all work and rendered `\frac{a+b}{c}`. |
| Inline math rendering (E1/E2) corrupts offsets, copy, or undo | Medium | High | Mandatory M5 spike + 10,000-op randomized integrity test; E4 popover ships first at STOP 2 as safe baseline. |
| ASan/UBSan unavailable on MSYS2 UCRT64 GCC | **Confirmed** | Medium | Strict `-Dwerror=true`, bounds-checked arena, deterministic fuzz harness locally; Linux CI for ASan/UBSan. |
| Emphasis / highlighter conflicts persist | Medium | Medium | Dual fix (`bt-markdown.lang` context order + `editorpane.c` skip) + regression test on `02-bootstrap.md` lines. |
| Graph ghost nodes diverge from editor unresolved links | Low | Medium | Single `bt_link_resolve()` shared by editor, index, and `bt_graph_build()` + automated equivalence test. |
| Unsafe link target launches executable or escapes vault | Low | High | Pure-C allow/block policy + `vault_resolve()` + zero shell execution + Group U mock tests. |
| STIX Two Math glyph gaps on rare Tier 2 commands | Medium | Low | Startup coverage audit + `.notdef` error mark + unknown-command histogram from `--selftest-math`. |
| Locked `dist\brainterm\bt.exe` causes stale-binary test | Medium | Medium | `scripts/package-win.ps1 -KillRunning -SmokeTest` + SHA-256 & build-stamp verification every phase. |
