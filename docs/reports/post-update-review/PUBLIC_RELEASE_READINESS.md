# BrainTerm — Public Release Readiness Assessment

**Assessment Date:** 2026-11-27  
**Assessed Revision & Build Stamp:** `BrainTerm 0.1.0 (build f0eef3c-dirty · 2026-11-27 17:48:43)`  
**Verified Portable Output (`dist/brainterm/`):** `bt.exe` (`3,477,076` bytes / `3.32 MB`, SHA-256 `CD9944316AFF1EBEA0B14B08C375893195ED1B0A3CB69087843039A00C674035`), `bt-mcp.exe`, UCRT64 DLL closure, and `licenses/` (`89.60 MB` bundle)  

---

## 1. Maturity Classification & Distribution Tier Assessment

**Assigned Maturity Level:** **Beta (Windows Portable Pre-Beta Candidate)**

BrainTerm has graduated cleanly past Prototype and Early Alpha stages. All 16 core functional subsystems (`vault`, `frontmatter`, `mdparse`, `index`, `linkresolve`, `createnote`, `choosermodel`, `navhistory`, `watcher`, `checkpoint`, `sessions`, `agents`, `importer`, `organizer`, `embed`, `libbt_math`, `ConPTY`, `Brain Graph 2D/3D/Galaxy/Timeline`, and `bt-mcp` v2) compile cleanly under `-Dwerror=true` (**0 warnings / 0 errors**), pass **`30 / 30` correctness targets (`144` CMocka cases)** and **`7 / 7` benchmarks**, and pass isolated `System32`-only `PATH` smoke verification:
- `bt.exe --smoke-test`: `BT_SMOKE ok notes=2 lang=bt-markdown term=alive ui=adw-ink scheme=dark tabs=1 checkpoint=ok sessions=1 index=ok ask=search graph=nodes:2`
- `bt-mcp.exe --smoke-test`: `BT_MCP_SMOKE ok readOnly=true`

### Release Distribution Tier Breakdown
| Distribution Target | Readiness Status | Rationale & Conditions |
|---|---|---|
| **Developer / Contributor Local Build (Windows 11)** | **READY** | Clean `-Dwerror=true` build, `30/30` test pass, `7/7` benchmarks, lock-safe packager. |
| **Controlled Windows Portable Pre-Beta (`dist/brainterm/`)** | **READY (With Notice)** | Isolated `PATH` verified; requires shipping `licenses/` and warning users binary is unsigned. |
| **Linux Desktop / Cross-Platform General GA (`v0.2.0+`)** | **BLOCKED** | Blocked by missing root `LICENSE`, unverified interactive Linux/IME/TUI QA, unsigned EXEs, and pending CI. |

### Evidence Highlights Supporting Pre-Beta Rating
- **Compiler & Test Cleanliness:** `0` warnings/errors (`werror=true`), `30/30` correctness suites (`144` CMocka cases), `7/7` GPU/CPU benchmarks passing.
- **Data Safety Proof:** Out-of-process crash simulation (`crashrig_r1`), R2 SQLite integrity rebuild, R9/R10 write refusals, and `.bt/dismissed_tags.json.bak` preservation verified.
- **Isolation & Security Proof:** `bt-mcp.exe` opens vaults read-only (`-32601` on traversal/write), agent prompts strip `'%'`, `'\''`, `'!'`, and POSIX metachars, and `vault_resolve()` blocks symlink escapes.

---

## 2. Independent 15-Dimension Readiness Scorecard

| Dimension | Score (`/100`) | Evidence-Backed Assessment |
|---|---|---|
| **Core Functionality** | **90** | Plain Markdown vault, SQLite FTS5, links, HarfBuzz math, 4 graph modes, ConPTY, checkpoints, and `bt-mcp` operate cleanly end-to-end. |
| **Feature Completeness** | **88** | Starter Vault, Find/Replace, `>import chats`, `>review ai tags`, `>sync embeddings`, `Alt+B`, and `F1` close pre-beta workflows; `BT_ORG_LOCAL_MODEL` stays a stub; Logseq `P1–P9` deferred. |
| **Architecture** | **93** | Acyclic `libbt_math` <- `libbt_core` <- UI/`bt-mcp` layering, `ADR-007` single-writer SQLite queue, unified GL/Cairo renderer abstraction. |
| **Code Quality** | **87** | Zero warnings under `-Dwerror=true -Wall -Wextra -Wpedantic -Wshadow -Wformat=2`; only legacy M1 `noteutil.c` helper retention remains. |
| **Stability** | **85** | `crashrig_r1`, `watcher_conflict`, `pty_cleanup`, and settled zero-CPU graph loops pass; lacks multi-hour interactive soak & sanitizer runs. |
| **Data Integrity** | **94** | L0 `.md` truth, exclusive `.md.tmp` atomic writes, create-only note commits, `R9` YAML refusal, `R10` `.git/index.lock` refusal, `.bak` backup, `R2` DB rebuild. |
| **Security** | **89** | `vault_resolve()` symlink/ADS guard, 32-ext blocklist, URI allowlist + userinfo strip, `%`/`'`/`!`/POSIX filter, `256 KiB`/`64 MiB`/`8 MiB` bounds, read-only `bt-mcp`. |
| **Performance** | **93** | `1.8 ms` 1k scan, `0.31 ms` 1k FTS5, `0.047 ms` VT redraw, `76+ fps` Cairo & `0.098–0.572 ms` p95 RTX 5060 GL, `33.5 ms` cold / `12.0 ms` warm note frame, `230.7 MB` RSS. |
| **User Experience** | **83** | Cohesive Dusk UI, smooth G3 drag-to-pin, focus-preserving tabs, inline popovers, `F1` modal; narrow `~400 px` docked graphs can crowd pills. |
| **Accessibility** | **78** | Keyboard nav, `Reduce motion` gating, searchable `F1` cheatsheet, `GTK_ACCESSIBLE_PROPERTY_LABEL` tags; manual AT-SPI screen-reader pass open. |
| **Documentation** | **92** | `README.md`, `CHANGELOG.md`, `DECISIONS.md`, 5 release docs (`DEVELOPMENT_STATUS`, `KNOWN_LIMITATIONS`, `RELEASE_CHECKLIST`, `ROADMAP`, `THIRD_PARTY_LICENSES`), and this audit align cleanly. |
| **Testing** | **86** | `30/30` correctness suites (`144` CMocka cases) + `7/7` bench + `--selftest-links` + isolated `bt.exe`/`bt-mcp.exe` smoke checks; ASan/UBSan unavailable on MSYS2 GCC 16.1. |
| **Platform Compatibility** | **62** | Windows 11 UCRT64 verified thoroughly; Linux CI (`xvfb-run`) configured in `.github/workflows/ci.yml` but pending push and interactive desktop testing; macOS unsupported. |
| **Packaging** | **80** | Lock-safe `scripts/package-win.ps1` produces SHA-256-verified standalone `dist/brainterm/` (`89.60 MB`) with `licenses/`; unsigned EXEs and no installer/AppImage yet. |
| **Release Readiness** | **64** | Gated from general GA by missing root `LICENSE`, unsigned Windows binaries, pending remote CI run, and interactive Linux / IME / TUI QA. |

---

## 3. Comprehensive Public Release Readiness Checklist

| Release Requirement | Status | Observed Evidence & Remaining Gaps |
|---|---|---|
| **Installation Experience** | **PARTIAL** | Portable unzip-and-run folder passes clean-`PATH` smoke checks; Windows ZIP/Inno Setup installer and Linux AppImage/Flatpak not yet shipped. |
| **First-Run Experience** | **PASS** | Empty-vault onboarding + `New Starter Vault…` menu safely populate `vault-template/` into empty folders without clobbering existing directories. |
| **Core Workflow Stability** | **PASS** | Automated tests cover edit/save, Find/Replace, links, math, 4 graph modes + G3 pin, ConPTY tabs, checkpoints/rollback, importers, organizer, and MCP. |
| **Crash Handling & Recovery** | **PASS** | Out-of-process `crashrig_r1`, `R1` `.md.tmp` cleanup, `R2` SQLite rebuild, and `.bt/dismissed_tags.json.bak` preservation verified. |
| **Vault Data Protection** | **PASS** | Plain `.md` L0 source of truth, `P4` permanent tag locks, non-destructive conflict diff/keep-mine/reload, create-only note commits. |
| **Security & Sandboxing** | **PASS** | `vault_resolve()` symlink/ADS block, 32-ext blocklist, safe URI launcher policy, CLI metachar sanitization, bounded parsers, read-only `bt-mcp`. |
| **Project License (`LICENSE`)** | **FAIL (Blocker)** | Missing root `LICENSE` file in repository root; author rights currently remain reserved ([THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md)). |
| **Third-Party License Notices** | **PASS** | `dist/brainterm/licenses/` distributes `THIRD_PARTY_LICENSES.md`, `LICENSE-Inter.txt`, `LICENSE-STIXTwoMath.txt`, and `OFL-JetBrainsMono.txt`. |
| **Documentation Completeness** | **PASS** | Shortcuts, limitations, ADRs, roadmap, checklist, architecture handbook, and verification reports accurately match source code. |
| **Supported Operating Systems** | **PARTIAL** | Windows 11 (`MSYS2 UCRT64`) verified; Linux `pty_unix.c` / GTK4 desktop `UNVERIFIED` interactively; macOS explicitly excluded. |
| **Packaging & Code Signing** | **PARTIAL** | SHA-256 parity verified on `bt.exe` and `bt-mcp.exe`; executables lack Authenticode signatures (SmartScreen warning expected on web download). |
| **Upgrade / Schema Behavior** | **PARTIAL** | SQLite migration `1 -> 2` (`0002_links_resolution.sql`) tested; `state.ini` tolerates unknown keys cleanly; no installer upgrade flow yet. |
| **Backup & Rollback** | **PASS** | In-process `libgit2` auto-checkpoints before agent/organizer batches with diff preview & post-rollback UI/index reload. |
| **Known Limitations Disclosure** | **PASS** | Explicitly documents 128-dim lexical vectors, unavailable MSYS2 ASan/UBSan, unverified IME/Linux desktop, and deferred Logseq `P1–P9`. |
| **Issue Reporting & Release Notes** | **PARTIAL** | `CHANGELOG.md` and `RELEASE_CHECKLIST.md` updated cleanly; `.github/ISSUE_TEMPLATE/` issue forms not yet added. |

---

## 4. Bundled Runtime Dependency & License Summary

`scripts/package-win.ps1` inspects `objdump -p` imports for both `bt.exe` and `bt-mcp.exe`, collecting UCRT64 runtime DLLs alongside `dist/brainterm/licenses/` ([THIRD_PARTY_LICENSES.md](../../THIRD_PARTY_LICENSES.md)):
- **Weak Copyleft Runtime DLLs:** `glib2` (`LGPL-2.1-or-later`), `gtk4` (`LGPL-2.1-or-later`), `libadwaita` (`LGPL-2.1-or-later`), `gtksourceview5` (`LGPL-2.1-or-later`), `pango` (`LGPL-2.1`), and `cairo` (`LGPL-2.1-or-later OR MPL-1.1`).
- **Strong Copyleft / Exception Libraries:** `freetype` (`GPL-2.0-or-later OR FTL`), `libgit2` (`GPL2 custom` with linking exception), and `gcc-libs` (`GPL-3.0-or-later WITH GCC-exception-3.1 AND LGPL-2.1-or-later`).
- **Permissive & Font Assets:** `libepoxy`, `harfbuzz`, `libvterm`, `md4c`, `libyaml`, `cjson` (`MIT`), `sqlite3` (`PublicDomain`), bundled fonts (`OFL-1.1`), and `cmocka` (`Apache-2.0`, test builds only).
- **Distribution Obligation:** Public archives must ship `dist/brainterm/licenses/` (`THIRD_PARTY_LICENSES.md`, `LICENSE-Inter.txt`, `LICENSE-STIXTwoMath.txt`, `OFL-JetBrainsMono.txt`) intact paired with an owner-selected root `LICENSE` compatible with dynamic linkage against the stack above.

---

## 5. Final Verdict & Concrete Four-Gate Unblock Plan

**Final Public Release Verdict:** **Beta (Windows Portable Pre-Beta Candidate) — Ready for Controlled Windows Pre-Beta Testing, NOT YET Ready for General Cross-Platform Public GA**

1. **Gate 1 — Legal License Selection:**
   - *Current State:* Repository root `LICENSE` is absent; third-party notices already bundle cleanly in `dist/brainterm/licenses/`.
   - *Action:* Choose and commit a root `LICENSE` compatible with bundled runtime libraries and update `README.md` + `docs/THIRD_PARTY_LICENSES.md`.
   - *Acceptance Criterion:* `LICENSE` exists at repository root covering project sources alongside bundled library obligations.
2. **Gate 2 — Linux & Sanitizer CI Verification:**
   - *Current State:* `.github/workflows/ci.yml` configures `windows` + `ubuntu` (`xvfb-run`), awaiting pushed commits; MSYS2 UCRT64 lacks `-lasan`/`-lubsan`.
   - *Action:* Push curated commits to `origin/master`, verify green `windows` and `ubuntu` (`xvfb-run`) jobs in `.github/workflows/ci.yml`, and execute Linux ASan/UBSan builds (`-Db_sanitize=address,undefined`).
   - *Acceptance Criterion:* Zero CI test failures across both operating systems and zero sanitizer findings.
3. **Gate 3 — Interactive Human QA Sign-Off:**
   - *Current State:* Automated Windows GUI/unit suites pass, while interactive UX edge cases await manual sign-off.
   - *Action:* Complete [RELEASE_CHECKLIST.md](../../RELEASE_CHECKLIST.md) §4 (Windows CJK/dead-key IME composition, live `claude`/`vim` ConPTY drag-resize, conflict banner diffs, interactive Linux X11/Wayland session, AT-SPI check).
   - *Acceptance Criterion:* All manual QA checkboxes signed off without graphical corruption or crash.
4. **Gate 4 — Distribution Packaging & Community Templates:**
   - *Current State:* Portable bundle builds cleanly (`89.60 MB`), unsigned, without installers or issue templates yet.
   - *Action:* Build signed Windows ZIP/Inno Setup installers, Linux AppImage/Flatpak packages, and `.github/ISSUE_TEMPLATE/` templates.
   - *Acceptance Criterion:* Clean web download/install verification without missing DLLs or broken metadata.
