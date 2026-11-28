# BrainTerm — Post-Update Executive Summary

**Document Date:** 2026-11-27  
**Repository Base Branch:** `master` @ `f0eef3c80855cdf9baa59a70e82bb32f0b448ae8` (`[origin/master: ahead 7]`)  
**Working Tree Scope:** `42` modified tracked files (`+3,185 / -198` lines), `5` new release docs in `docs/`, and this `8`-part audit package under `docs/reports/post-update-review/`  
**Verified Build Stamp:** `BrainTerm 0.1.0 (build f0eef3c-dirty · 2026-11-27 17:48:43)`  
**Verified Toolchain & Hardware:** Windows 11 (`MSYS2 UCRT64`, `gcc 16.1.0`, C17, `warning_level=3`, `werror=true`, NVIDIA GeForce RTX 5060 Laptop GPU)  

---

## 1. Plain-Language Overview: What BrainTerm Is

**BrainTerm** is a local-first, plain-Markdown personal knowledge workspace and terminal cockpit implemented in **C17** using **GTK4**, **libadwaita**, **GtkSourceView 5**, **SQLite FTS5**, **HarfBuzz / FreeType / Cairo**, **OpenGL 3.3 Core**, **libvterm**, **ConPTY / POSIX PTY**, **libyaml**, **md4c**, **cJSON**, and **libgit2**. It contains zero WebViews, JavaScript runtimes, Electron layers, cloud accounts, or telemetry daemons.

Designed for software engineers, researchers, and technical writers who manage hundreds or thousands of local `.md` notes alongside terminal workflows (`claude`, `codex`, `gemini`), BrainTerm ships as two coordinated executables backed by plain files on disk:

1. **Plain-Markdown Vault (`L0` Authoritative Source of Truth):**
   - Stores notes as standard UTF-8 `.md` files with YAML frontmatter readable by Git, VS Code, or Obsidian with zero vendor lock-in.
   - Enforces canonical path containment (`vault_resolve()`), temporary `.md.tmp` atomic writes, create-only new-note commits, and non-destructive external conflict resolution (`Review diff`, `Keep mine`, `Reload`).
   - Supports external edit drift monitoring (`watcher.c` with `500 ms` debounce and `500`-event storm rescan) plus `F2` rename and safe delete.
2. **GTK4 Editor, Typeset Math & Brain Graph (`bt.exe` UI):**
   - Renders `GtkSourceView 5` authoring (`bt-markdown.lang`), `Ctrl+F`/`Ctrl+H` Find & Replace, clickable `[[wikilinks]]`, ambiguity choosers, and 100-jump Back/Forward navigation (`BtNavHistory`).
   - Compiles inline `$...$` and display `$$...$$` LaTeX expressions in place via `libbt_math` (`HarfBuzz` OpenType `MATH` + `STIXTwoMath-Regular.otf`).
   - Projects vault topology and note links across **2D (`Clean` & `Glow`)**, **3D Cortex**, **Galaxy**, and **Timeline** views backed by OpenGL 3.3 Core or Cairo fallback.
3. **Derived Offline Memory & Embedded PTY Terminal (`L1` Cache & Execution):**
   - Maintains a single-writer background SQLite WAL database (`bt-db-writer`) indexing `unicode61` FTS5 search text, wikilink resolutions, `tags_ai` proposals, sessions, and optional `128-dim` lexical vectors.
   - Spawns up to 8 real shell tabs (`ConPTY` + Win32 Job Objects on Windows; `forkpty` on POSIX) rooted at the vault directory with automated `libgit2` safety checkpoints before AI runs.
4. **Headless Read-Only MCP Bridge (`bt-mcp.exe`):**
   - Exposes `search_memory`, `ask_memory`, `recent`, `get_note`, and `backlinks` over stdio JSON-RPC 2.0 using read-only vault and database access for Claude Desktop and Cursor.

---

## 2. What BrainTerm Was vs. What the Update Delivered

Inspecting `git log origin/master..HEAD` (`7` local commits) alongside uncommitted working-tree changes reconstructs two coordinated stages since `origin/master` (`2086213` / `1b54694`):

### Baseline State Before the Latest Update (`origin/master`)
- Brain Graph rendered only an additive white 3D point cloud without directory nodes, depth-sorted screen picking, Galaxy spirals, Timeline swimlanes, or Obsidian-style de-overlapped 2D positioning.
- Importers, tag governance (`organizer.c`), cached offline embeddings (`embed.c`), backlinks lists, and editor Find & Replace existed only partially or lacked GUI workflows; `BtNavHistory` did not update across note renames/deletions; and `bt-mcp` lacked a `top_k` schema property and >64 KB payload handling.

### Stage 1 — Seven Committed Graph Redesign & Clean 2D Commits (`7861933`..`f0eef3c`)
- **Folder Nodes & Depth-Sorted Picking (`7861933`):** Promoted folders to collapsible `BT_NODE_FOLDER` nodes and added spatial-hash `bt_graph_pick_screen()`, local fisheye hover spread, and 4-way label placement (`src/graph.c`, `src/graph_state.c`).
- **Andromeda Palette (`bd42b04`):** Added `12` Andromeda folder swatches, redmean perceptual distance selection (`src/graph_palette.c`), and `.brainterm/folder-colors.json` persistence.
- **Galaxy & Timeline Views (`9299559`, `d617584`):** Added 4-arm logarithmic Galaxy spirals (core bulge shader, `3%` halo, rim warp, `700 ms` transition) and Timeline mode (`Organic` beeswarm / `Kept` grid, brushing, event ledger).
- **Obsidian-Style Clean 2D Layout & Presets (`003e475`, `43205a7`, `f0eef3c`):** Implemented pure-C deterministic 2D force relaxation (`src/graphlayout.c`, **G1**) with component packing, halo ring, `rem_overlaps == 0`, and persisted `Style: Clean | Glow` presets (**G2**).

### Stage 2 — Uncommitted Pre-Beta Completion, Hardening & Packaging (`2026-11-25`..`2026-11-27`)
- **Graph `G3` Interactive Engine & GPU Optimization:** Fixed layout spring reheat and node position sync, added VBO-only position uploads (`bt_graph_gl_mark_positions_dirty()`), added 2D drag-to-pin + Pin/Unpin menus, eliminated idle redraw CPU usage on convergence, and added `Re-layout` (`R`) and `Highlight Orphans` (`O`).
- **GUI Workflow Closure & Accessibility:** Added Find & Replace (`Ctrl+F`/`Ctrl+H`), Starter Vault onboarding + `New Starter Vault…`, `>import chats`, `>review ai tags`, `>sync embeddings`, `Alt+B` Backlinks & Orphans, Ask Bar (`Ctrl+E`) 10-entry history + `Hybrid` toggle, searchable `F1` Keyboard Shortcuts modal, `BtNavHistory` rename/delete sync, terminal focus/middle-click close, and `GTK_ACCESSIBLE_PROPERTY_LABEL` attributes.
- **Security, Parser Bounds & `bt-mcp` v2:** Loaded cached SQLite vectors once per query (`vault == NULL` safe) with `vec_is_finite()` `NaN`/`Inf` rejection; capped YAML headers (`256 KiB`) and importers (`64 MiB`); hashed empty-ASCII Unicode slugs (`untitled-%.6s`); errored cleanly on 0-object `.jsonl`; backed up corrupt `.bt/dismissed_tags.json` to `.bak`; stripped `'%'`, `'\''`, `'!'` alongside POSIX metachars; hardened `bt-mcp` (`vault_open_readonly()`, `8 MiB` line buffer, `capabilities`, `top_k` + `hybrid` schema, `backlinks`); and updated CI, `scripts/package-win.ps1`, and docs.

---

## 3. Feature Maturity & Quantitative Verification Summary

| Capability Area | Verified Status | Primary Evidence Targets |
|---|---|---|
| Vault Scanning, Path Guard & Atomic Writes | **PASS** | `pathguard` (`3`), `atomicwrite` (`8`), `scan` (`2`), `crashrig_r1` (`3`) |
| Editor, Find/Replace, Links (`L1–L3`) & Math (`M1–M5`) | **PASS** | `editor_links` (`27`), `createnote` (`5`), `chooser` (`11`), `linkresolve` (`4`), `math_*` |
| Brain Graph (`2D Clean/Glow`, `3D`, `Galaxy`, `Timeline`, `G3`) | **PASS** | `graph` (`2`), `graph_v2` (`11`), `graphlayout` (`5`), `fix_ui` (`7`), `bench_graph_*` |
| Embedded Terminal (`ConPTY`) & Agent Launchers | **PASS (Windows)** | `vtcorpus` (`5`), `pty_cleanup` (`1`), `tabs` (`1`), `agents_cmd` (`2`) |
| Importers, Organizer, Offline Vectors & `bt-mcp` | **PASS** | `importers` (`3`), `organizer` (`2`), `embeddings` (`2`), `mcp` (`2`) + live E2E |
| Linux Desktop (`pty_unix.c`), IME, TUI Resize & Sanitizers | **UNVERIFIED Locally** | Ubuntu CI awaits push; manual IME/`vim` QA pending; MSYS2 lacks `-lasan` |
| Logseq Parity Spec (`docs/logseq-parity.md` `P1–P9`) | **NOT APPLICABLE** | Specification-only roadmap backlog explicitly deferred beyond pre-beta |

### Quantitative Verification Snapshot (Measured on Current Repository)
- **Strict Compiler Hygiene (`meson compile -C build -Dwerror=true`):** **0 warnings / 0 errors** across `bt.exe`, `bt-mcp.exe`, `libbt_core.a`, `libbt_math.a`, and all `37` test/bench binaries.
- **Automated Correctness (`--no-rebuild`, `-j 1`):** **`30 / 30` targets (`144` CMocka cases)** passed, `0` failed, `0` skipped.
- **Benchmarks (`7 / 7` OK):**
  - `1k` scan **`1.8 ms`**; `1k` FTS5 search **`0.31 ms`**; `createnote` **`0.0059 ms`** avg (`3k` vault); VT redraw **`0.047 ms/frame`**.
  - Pure-C `graphlayout` deterministic de-overlap (`rem_overlaps == 0`): **`898.30 ms`** (`1k`), **`5,176.01 ms`** (`3k`).
  - Cairo graph `1k/3k High`: **`13.12 ms` (`76.2 fps`)**; RTX 5060 GL `1k/3k 2D High` **`0.098 ms` p95** / `3D High` **`0.376 ms` p95**.
- **Headless CLI & Portable Package Checks (`dist/brainterm/`):**
  - `bt.exe`: `3,477,076` bytes (`3.32 MB`, SHA-256 `CD9944316AFF1EBEA0B14B08C375893195ED1B0A3CB69087843039A00C674035`), full bundle **`89.60 MB`** with `licenses/`.
  - `--selftest-links vault-template`: **`2.82 ms`** across `11` links (`RESOLVED=5 AMBIGUOUS=1 UNRESOLVED=1 PLACEHOLDER=1 EXTERNAL=2 BLOCKED=1`).
  - Isolated `--smoke-test` (`System32`-only `PATH` + sandbox `BT_CONFIG_DIR`): **`BT_SMOKE ok ...`** (`1,931.4 ms` cold startup), **`BT_MCP_SMOKE ok readOnly=true`**, and live `bt-mcp.exe` traversal/write refusal (`MCP_EXIT=0`).
  - `--bench-first-open` (`examples/links-demo.md`): **`33.513 ms`** cold frame / **`12.039 ms`** warm frame (`6.06 ms` p95); settled idle GUI RSS **`230.7 MB`** (`222.4 MB` private, **`2.21%`** CPU delta).

---

## 4. Public Release Verdict, Remaining Risks & Documentation Map

**Current Maturity Classification:** **Beta (Windows Portable Pre-Beta Candidate)**  
**Public Release Verdict:** **Ready for Controlled Windows Pre-Beta Evaluation — NOT YET Ready for General Cross-Platform Public GA**

### Key Release Blockers Before General Public GA
1. **Legal Blocker — Missing Root `LICENSE`:**
   - Author rights remain reserved until an open-source license compatible with bundled `LGPL-2.1+`, `GPL-2.0+/FTL`, `GPL2 custom`, `MIT`, and `OFL-1.1` runtime libraries is committed ([THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md)).
   - Distribution bundles in `dist/brainterm/licenses/` already retain all third-party attributions (`THIRD_PARTY_LICENSES.md`, `LICENSE-Inter.txt`, `LICENSE-STIXTwoMath.txt`, `OFL-JetBrainsMono.txt`).
2. **Unverified Interactive Platforms & Remote CI:**
   - Linux desktop execution (`pty_unix.c` + Wayland/X11) awaits interactive validation outside headless CI definitions.
   - Windows CJK/dead-key IME input, live `vim`/`claude` ConPTY drag-resize ergonomics, and AT-SPI screen-reader traversal require interactive human QA ([RELEASE_CHECKLIST.md](../../RELEASE_CHECKLIST.md)).
   - `.github/workflows/ci.yml` configures both Windows MSYS2 and Ubuntu `xvfb-run` (`-j 1`) runs and awaits a pushed commit on `master`.
3. **Toolchain Sanitizer & Binary Signing Gaps:**
   - MSYS2 UCRT64 `gcc 16.1.0` lacks `-lasan` and `-lubsan` runtime archives (`UNVERIFIED` locally; Linux CI can run sanitizers).
   - Portable Windows EXEs are currently unsigned, so web downloads will display Windows SmartScreen prompts until Authenticode signing is added.

**Included Technical Documents (`docs/reports/post-update-review/`):**
- [CHANGELOG_DETAILED.md](CHANGELOG_DETAILED.md) — Commit-by-commit diffs (`7` local commits), all `42` modified tracked files, and before/after tables.
- [APPLICATION_OVERVIEW.md](APPLICATION_OVERVIEW.md) — UI layout, controls, end-to-end user journeys, keyboard shortcuts, and scope limits.
- [ARCHITECTURE_AND_INTERNALS.md](ARCHITECTURE_AND_INTERNALS.md) — Compile-time layering, verified tech stack, module internals, and Mermaid flows.
- [FEATURE_REFERENCE.md](FEATURE_REFERENCE.md) — Technical reference covering all `16` implemented subsystems and limits.
- [ENGINEERING_AND_TEST_REPORT.md](ENGINEERING_AND_TEST_REPORT.md) — Audit findings (`BT-AUD-001`–`010`), threat boundary matrix, `30` test suites, `7` benchmarks, and telemetry.
- [DEVELOPER_HANDOFF.md](DEVELOPER_HANDOFF.md) — Build/test/packaging commands, invariants, code orientation hooks, and next steps.
- [PUBLIC_RELEASE_READINESS.md](PUBLIC_RELEASE_READINESS.md) — 15-dimension readiness scorecard, 15-item release checklist, license matrix, and unblock gates.
