# BrainTerm — Engineering Audit, Security & Verification Report

## 1. Compiler & Build Toolchain Verification

- **Configured Meson Options (`meson configure build`):** `c_std=c17`, `warning_level=3`, `werror=true`, `tests=true`, extra compiler flags `-Wpedantic -Wshadow -Wformat=2 -Wno-overlength-strings`.
- **Full Build Targets Compiled (`meson compile -C build`):** `libbt_core.a` (`25` shared + `2` OS platform files), `libbt_math.a` (`5` files), `bt.exe`, `bt-mcp.exe`, `bt-resources`, `bt_build_stamp_h`, and all `37` test/benchmark executables.
- **Compiler Result:** **PASS — 0 warnings / 0 errors** across every C translation unit and GLib resource build.

---

## 2. Detailed Engineering & Security Audit Findings (`BT-AUD-001` – `BT-AUD-010`)

### BT-AUD-001 — High (FIXED): Shell Metacharacter Gaps (`%`, `'`, `!`) in Agent & Ask Launchers
- **Category & Files:** Command Injection Hardening (`src/agents.c`, `src/search.c`)
- **Impact Before Fix:** Initial sanitization stripped POSIX shell tokens (`"`, `` ` ``, `$`, `\`, `;`, `|`, `&`, `<`, `>`, `(`, `)`) while leaving `'%'`, `'\''`, and `'!'` unfiltered, permitting `cmd.exe` `%VAR%` environment expansion inside quoted prompt arguments or single-quote breaks.
- **Implemented Fix:** Extended `sanitize_title_literal()` and `append_sanitized()` identically to strip `'%'`, `'\''`, and `'!'`.
- **Verification:** Hostile note title and Ask-in-agent tests in `tests/unit/test_agents_cmd.c` (`agents_cmd` **PASS**).

### BT-AUD-002 — Medium (FIXED): Importer Resource Bounds, Non-ASCII Slugs & Empty JSONL
- **Category & Files:** Input Bounds & Parser Correctness (`src/importer.c`)
- **Impact Before Fix:** Importers read files unbounded via `g_file_get_contents()`, collapsed non-ASCII titles (Japanese/Arabic) onto shared `untitled.md` paths, and wrote blank notes from malformed non-empty `.jsonl` transcripts.
- **Implemented Fix:** Added `read_import_file_bounded()` (**`64 MiB`** limit), FNV-1a hashed `untitled-%.6s` slugs, YAML title escaping, and `BT_ERROR_INVALID` on 0-object JSONL files.
- **Verification:** Boundary, slug, and error tests in `tests/unit/test_importers.c` (`importers` **PASS**).

### BT-AUD-003 — Medium (FIXED): Unbounded YAML Frontmatter Slice
- **Category & Files:** Parser Ceiling (`src/frontmatter.c`)
- **Impact Before Fix:** Arbitrarily large `--- ... ---` headers passed unconstrained into `libyaml`.
- **Implemented Fix:** Enforced `ylen > (256u * 1024u)` ceiling flagging `fm->parse_error = TRUE` cleanly.
- **Verification:** Oversized YAML test in `tests/unit/test_frontmatter.c` (`frontmatter` **PASS**).

### BT-AUD-004 — Medium (FIXED): Silent Discard of Corrupt `.bt/dismissed_tags.json`
- **Category & Files:** Data Integrity / Human Lock Preservation (`src/organizer.c`)
- **Impact Before Fix:** Corrupt `.bt/dismissed_tags.json` files were silently overwritten with `{}`, erasing dismissed-tag locks (`P4`).
- **Implemented Fix:** Preserved corrupt JSON as `.bt/dismissed_tags.json.bak` via `g_rename()` alongside `g_warning()`, plus `R9` malformed YAML refusal enforcement.
- **Verification:** Corrupt lock preservation & `R9`/`R10` checks in `tests/unit/test_organizer.c` (`organizer` **PASS**).

### BT-AUD-005 — Medium (FIXED): Non-Finite Vector Guard & Documentation Parity
- **Category & Files:** Numerical Safety & Documentation Accuracy (`src/embed.c`, `docs/KNOWN_LIMITATIONS.md`)
- **Impact Before Fix:** Corrupted SQLite `NaN`/`Inf` BLOBs could poison cosine similarity scores, and docs misdescribed vectors as `256-dim IDF-weighted`.
- **Implemented Fix:** Added `vec_is_finite()` / `isfinite()` guards rejecting invalid vectors (`0.0`), cached persisted vectors once per query (`vault == NULL` safe), and documented `BT_EMBED_DIM = 128` accurately.
- **Verification:** Cached lookup & `NaN` vector tests in `tests/unit/test_embeddings.c` (`embeddings` **PASS**).

### BT-AUD-006 — Low (FIXED): Missing `top_k` Schema Property in `bt-mcp`
- **Category & Files:** Protocol Contract Alignment (`src/mcp/main.c`)
- **Impact Before Fix:** `tools/list` omitted `top_k` even though `search_memory` honored `top_k` arguments.
- **Implemented Fix:** Exposed `query`, `top_k` (`integer`), and `hybrid` (`boolean`) cleanly alongside read-only vault open and **`8 MiB`** line buffering.
- **Verification:** Protocol & >64 KB payload tests in `tests/unit/test_mcp.c` (`mcp` **PASS** + live E2E).

### BT-AUD-007 — Medium (FIXED): Stale `BtNavHistory` Entries After Note Rename / Deletion
- **Category & Files:** Editor History Consistency (`src/navhistory.h`/`.c`, `src/app.c`)
- **Impact Before Fix:** Back/Forward jumps kept orphaned paths after renames or deletions.
- **Implemented Fix:** Implemented `bt_nav_history_rename_path()` and `remove_path()` wired cleanly into rename and delete handlers.
- **Verification:** History sync & editor UI tests in `tests/unit/test_editor_links.c` (`editor_links` **PASS**).

### BT-AUD-008 — Low (FIXED): Shortcuts Discoverability, Onboarding Re-Entry & Accessibility Labels
- **Category & Files:** UI Usability & Accessibility (`src/app.c`, `src/graphview.c`, `src/ui/filetree.c`, `src/ui/termtabs.c`)
- **Impact Before Fix:** Icon-only toolbar buttons lacked `GTK_ACCESSIBLE_PROPERTY_LABEL` tags, Starter Vault existed only on empty states, and shortcut lookup lacked a modal dialog.
- **Implemented Fix:** Added `New Starter Vault…`, searchable `F1` Keyboard Shortcuts modal (compatible with GTK `>= 4.10` / Adw `>= 1.4`), and explicit accessibility labels.
- **Verification:** Headless widget & action checks in `tests/unit/test_fix_ui.c` (`fix_ui` **PASS**).

### BT-AUD-009 — Low (OPEN Technical Debt): Retained Legacy Helpers in `noteutil.c`
- **Locations:** `src/noteutil.c`, `src/linkresolve.c`
- **Assessment & Next Owner:** Early M1 link helpers stay compiled in `noteutil.c` solely for `test_noteutil` compatibility while editor, SQLite, and Graph call `linkresolve.c`. Low risk; retire alongside legacy tests.

### BT-AUD-010 — Informational (OPEN Platform / Sanitizer Gap): MSYS2 Sanitizers & Interactive QA
- **Locations:** `.github/workflows/ci.yml`, `docs/RELEASE_CHECKLIST.md`
- **Assessment & Next Owner:** MSYS2 UCRT64 `gcc 16.1.0` lacks `-lasan` and `-lubsan` runtime archives, and interactive Linux desktop (`pty_unix.c`), Windows CJK/dead-key IME composition, and manual `vim`/`claude` ConPTY drag-resize await CI push and human verification.

---

## 3. Security Threat & Boundary Verification Matrix

| Attack / Fault Vector | Enforced Mitigation Mechanism | Verification Evidence |
|---|---|---|
| **Path traversal / symlink escape** | `vault_resolve()` blocks `..`, drives, UNC, ADS `:`, reserved Win32 names, symlinks/junctions via `bt_platform_realpath()` | `pathguard` (`3`), `linkresolve`, live `bt-mcp` `../../etc/passwd` rejection |
| **Executable / unsafe URI launch** | `bt_link_check_open_policy()` blocks 32 executable extensions, restricts URLs to `http`/`https`/`mailto` (`<= 2048 B`), strips `user:pass@` | `linkresolve`, `--selftest-links vault-template` (`BLOCKED=1`) |
| **CLI prompt shell injection** | `sanitize_title_literal()` & `append_sanitized()` strip control chars plus `"`, `'`, `` ` ``, `$`, `%`, `!`, `\`, `;`, `\|`, `&`, `<`, `>`, `(`, `)` | `agents_cmd` (`2`) |
| **Oversized / hostile inputs** | Math AST caps (`4 KB`/`16 KB`, depth `64`, `5,000` nodes, `20x20` matrix), `256 KiB` YAML cap, `64 MiB` importer cap, `8 MiB` MCP line cap | `math_tok`, `frontmatter`, `importers`, `mcp` (`>64 KB` line check) |
| **MCP write / DB corruption** | `bt-mcp` uses `vault_open_readonly()`, `bt_index_open_readonly()`, rejects writes (`-32601`), preserves `.md.tmp` | `mcp` + live `bt-mcp.exe` CLI check |
| **Orphaned child shells** | Win32 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (`pty_win.c`); POSIX `kill(-pid)` (`pty_unix.c`) | `pty_cleanup`, `tabs` |

---

## 4. Automated Correctness Suites (`30 / 30` Targets, `144` CMocka Cases)

Executed after final edits via `meson test -C build --no-rebuild --suite unit --suite crash-rig --suite vtcorpus --print-errorlogs -j 1`: **30 / 30 OK, 0 Failed, 0 Skipped**.

| Suite Target | Cases | Elapsed | Verified Coverage Scope |
|---|---|---|---|
| `pathguard` | `3` | `0.02s` | `vault_resolve()` traversal, drive/UNC/ADS, reserved names, symlinks |
| `atomicwrite` | `8` | `0.10s` | `.md.tmp` atomic writes, no overwrite loss, parent dirs |
| `scan` | `2` | `0.02s` | Vault note discovery, ignore patterns, read-only vs writable `.md.tmp` sweep |
| `noteutil` | `3` | `0.01s` | Fuzzy matching & legacy M1 wikilink helper coverage |
| `crashrig_r1` | `3` | `1.16s` | Child crash injection across `BtWriteStage` + R1 cleanup |
| `vtcorpus` | `5` | `0.01s` | `libvterm` escapes, wide chars, alt-screen, OSC-52 disabled |
| `pty_cleanup` | `1` | `0.05s` | Win32 Job Object process-group cleanup on close |
| `checkpoint` | `2` | `0.11s` | `libgit2` commit/log/rollback + `.git/index.lock` R10 refusal |
| `sessions` | `1` | `0.02s` | `.bt/sessions.json` read/write round-trip |
| `agents_cmd` | `2` | `0.03s` | Hostile title/ask metachar stripping (`%`, `'`, `!`, POSIX) & `AGENTS.md` safety |
| `tabs` | `1` | `0.08s` | ConPTY multi-tab creation, containment, 8-tab cap, clean teardown |
| `frontmatter` | `4` | `0.02s` | YAML round-trip, aliases, duplicate keys, `256 KiB` cap enforcement |
| `mdparse` | `1` | `0.01s` | CommonMark H1, plain body, inline tags, code-excluded wikilinks |
| `index` | `4` | `0.14s` | Migrations `0001..0002`, FTS5 triggers, recent/orphan/backlink/AI/embedding SQL |
| `watcher_conflict` | `2` | `0.14s` | `bt_conflict_decide()` outcomes, R4 500-event storm guard, R3 `body_hash` drift |
| `organizer` | `2` | `0.96s` | 50-note heuristic batch, R9/R10 refusal, promote/dismiss locks, `.bak` backup |
| `graph` | `2` | `0.06s` | 1-note = 1-node invariant, ghost targets, picking, filters |
| `graph_v2` | `11` | `0.02s` | Folder nodes, spatial-hash picking, Andromeda palette, Galaxy spiral, Timeline |
| `graphlayout` | `5` | `0.59s` | Deterministic 2D layout, `0` residual overlaps, warm start, G3 reheat |
| `linkresolve` | `4` | `0.13s` | Stage 1–4 lookup, subtree rank, A.9 URI/ext policy, `--selftest-links` |
| `createnote` | `5` | `3.85s` | Segment bounds, create-only race protection, missing dirs, template keys |
| `chooser` | `11` | `0.02s` | `BtChooserModel` wrap nav, 10-row paging, explicit `[[folder/name]]` copy |
| `math_tok` | `4` | `0.01s` | Math delimiters, `$5` currency safety, incremental range scans |
| `math_highlight` | `4` | `0.70s` | Dual emphasis fix inside `$...$`, `GtkTextView` inline tagging |
| `editor_links` | `27` | `3.66s` | Click/caret rules, flush before nav, shortcuts, Find/Replace, `BtNavHistory`, `0` UI log warnings |
| `importers` | `3` | `0.08s` | ChatGPT/Claude/Codex idempotency, YAML escaping, hashed slugs, JSONL errors |
| `embeddings` | `2` | `0.05s` | Cached vector toggle, RRF hybrid recall, `NaN` vector safety |
| `mcp` | `2` | `0.14s` | Stdio JSON-RPC (`top_k`/`hybrid`, read-only blocks), `>64 KB` reader |
| `theme` | `8` | `0.87s` | Exact `theme.css` ↔ `theme.h` token parity, CSS errors `= 0`, WCAG AA contrast |
| `fix_ui` | `7` | `1.94s` | Build stamp, state restore, stack pages, split geometry, G3 Orphans/Re-layout |

---

## 5. Benchmarks, Live Integration Matrix & Platform Separation (`7 / 7` OK)

Executed on Windows 11 MSYS2 UCRT64 (`NVIDIA GeForce RTX 5060 Laptop GPU`, OpenGL `4.6.0 NVIDIA 616.64`):

| Benchmark / Runtime Measurement | Measured Result | Budget / Scope |
|---|---|---|
| `bench_scan_1k` | `1.8 ms` (`1000` notes) | Budget `2000 ms` — **PASS** |
| `bench_vterm` | `0.047 ms/frame` avg (`1000` frames) | Budget `16.0 ms` — **PASS** |
| `bench_search_1k` | `0.31 ms` (`1000` notes, `20` hits) | Budget `100 ms` — **PASS** |
| `bench_graphlayout` | `1k`: `898.30 ms` (`300` iters, `rem_overlaps=0`); `3k`: `5176.01 ms` (`rem_overlaps=0`) | Zero residual disc overlaps — **PASS** |
| `bench_createnote` | `0.0059 ms` avg, `0.0280 ms` max (`3k` notes, `200` iters) | Budget `5.0 ms` — **PASS** |
| `bench_graph_1k` (`cairo-1.18.4`) | `1k/3k low`: `12.97 ms` (`77.1 fps`), `high`: `13.12 ms` (`76.2 fps`); `3k/9k high`: `10.62 ms` (`94.1 fps`) | Budget `16.6 ms` — **PASS** |
| `bench_graph_gl` (GL `4.6.0`, `bloom=1`) | `1k/3k 2D high p95 = 0.098 ms`, `3D high p95 = 0.376 ms`; `3k/9k 2D high p95 = 0.127 ms`, `3D high p95 = 0.572 ms` | Budget `16.6 / 22.2 ms` — **PASS** |
| Isolated `--smoke-test` (`dist/brainterm/bt.exe`) | `1,931.4 ms` wall (`BT_SMOKE ok notes=2 ... term=alive ... index=ok graph=nodes:2`) | Clean `System32`-only `PATH` + `BT_CONFIG_DIR` |
| `--bench-first-open` (`examples/links-demo.md`) | `cold_open_call = 5.281 ms` (`33.513 ms` frame); `warm_open_call = 1.154 ms` (`12.039 ms` frame); `subsequent_p95 = 6.06 ms` | Observed on sandboxed `vault-template` |
| Idle Settled GUI (~`3 s`, 16 logical CPUs) | `WorkingSet64 = 230.7 MB`, `PrivateMemorySize64 = 222.4 MB`, CPU delta `531.3 ms / 1.5 s` (`2.21%`) | Graceful `CloseMainWindow()` exit |
| `bt-mcp.exe` Live CLI E2E | `MCP_EXIT=0`, `initialize` (`readOnly:true`), `search_memory` (`top_k:3`) hit `3` notes, `../../etc/passwd` & `write_note` refused (`-32601`), malformed `-32700` | Tested on `dist/brainterm/bt-mcp.exe` |

### Cross-Subsystem Integration & Recovery Guarantees
| Pipeline / Recovery Check | Connected Modules | Result & Evidence |
|---|---|---|
| **Vault + Editor + History** | `vault.c` <-> `editorpane.c` <-> `navhistory.c` <-> `app.c` | **PASS** (`editor_links`, `atomicwrite`, `crashrig_r1`) |
| **Editor + SQLite FTS5** | `editorpane.c` <-> `mdparse.c` <-> `index.c` (`bt-db-writer`) | **PASS** (`index`, `editor_links`, `R2` integrity recovery) |
| **Links + Create + Ghosts** | `linkresolve.c` <-> `createnote.c` <-> `graph.c` (`BT_NODE_GHOST`) | **PASS** (`linkresolve`, `createnote`, `graph`, `--selftest-links`) |
| **Search + Vectors + RRF** | `search.c` <-> `embed.c` <-> `askbar.c` | **PASS** (`embeddings`, `bench_search_1k`) |
| **ConPTY + Job Object + Tabs** | `pty_win.c` <-> `vtermbridge.c` <-> `termtabs.c` | **PASS (Windows)** (`pty_cleanup`, `tabs`, `vtcorpus`) |
| **Organizer + Git Checkpoints** | `organizer.c` <-> `checkpoint.c` <-> `frontmatter.c` | **PASS** (`organizer`, `checkpoint`, `R9`, `R10`, `.bak`) |
| **MCP + Read-Only Vault** | `src/mcp/main.c` <-> `vault.c` <-> `index.c` | **PASS** (`mcp` + live `bt-mcp.exe` E2E) |
| **Linux Desktop / IME / ASan+UBSan** | `pty_unix.c`, Wayland/X11, Windows IME, `gcc -lasan` | **UNVERIFIED Locally** (awaiting CI push & human QA) |

### Portable Bundle & Self-Test Telemetry Details (`dist/brainterm/`)
- **Packaged Executable Integrity:** `dist\brainterm\bt.exe` (`3,477,076` bytes / `3.32 MB`, SHA-256 `CD9944316AFF1EBEA0B14B08C375893195ED1B0A3CB69087843039A00C674035`), `bt-mcp.exe`, full UCRT64 DLL closure, and `licenses/` (`89.60 MB` bundle total).
- **Smoke Outputs:** `bt.exe --smoke-test` emitted `BT_SMOKE ok notes=2 lang=bt-markdown term=alive ui=adw-ink scheme=dark tabs=1 checkpoint=ok sessions=1 index=ok ask=search graph=nodes:2`; `bt-mcp.exe --smoke-test` emitted `BT_MCP_SMOKE ok readOnly=true`.
- **Link Self-Test (`--selftest-links vault-template`):** Completed in **`2.82 ms`** across `7` starter notes (`11` total links, **`256.45 ms` / 1k links** rate): `RESOLVED=5`, `AMBIGUOUS=1`, `UNRESOLVED=1`, `PLACEHOLDER=1`, `EXTERNAL=2`, `BLOCKED=1`.
- **Note-Open Frame Timing (`--bench-first-open`):** `cold_open_call_ms=5.281`, `cold_first_frame_wall_ms=33.513`, `warm_open_call_ms=1.154`, `warm_first_frame_wall_ms=12.039`, `subsequent_p95_ms=6.06`.
