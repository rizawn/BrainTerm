# BrainTerm Roadmap

## Completed Foundations (`0.1.0` -> Pre-Beta)

- **Core & Safety (`M1–M6`):** Plain Markdown + YAML frontmatter storage, strict `vault_resolve()` path guard, atomic `.md.tmp` crash recovery, `GFileMonitor` external conflict resolution, `libgit2` checkpoints & rollback, SQLite FTS5 single-writer memory engine, auto-organizer with human-lock protection.
- **Native Embedded Terminal (`M2–M3`):** Multi-tab (cap 8) ConPTY + Win32 Job Object / POSIX `forkpty` + `libvterm` terminal, 10k scrollback ring + search, one-click CLI detection & sanitized launch for `claude`, `codex`, `gemini` (`BT_ACTIVE_NOTE`, `TERM_PROGRAM=brainterm`).
- **Rich Authoring (`L1–L3`, `M1–M5`):** Clickable wiki/Markdown/heading/URL links with create/ambiguity popovers, history navigation, in-document Find & Replace (`Ctrl+F` / `Ctrl+H`), starter vault onboarding from `vault-template/`, and live auto-compiling OpenType `MATH` LaTeX rendering (`STIX Two Math`).
- **Brain Graph (`G1–G3`):** Deterministic 2D Clean/Glow layout, 3D Cortex, Galaxy spiral disk, Timeline (`Organic`/`Kept` + ledger), first-class folder nodes, ghost nodes, spatial picking, dual Cairo / OpenGL rendering, interactive 2D drag-to-pin, live spring settling, `Re-layout` (`R`), and `Highlight Orphans` (`O`).
- **GUI Memory & Companion Tools:** Chat importer dialog (`ChatGPT`/`Claude`/`Codex` JSON/JSONL), AI tag review modal (`Promote`/`Dismiss`), hybrid FTS5 + offline vector search pill & index sync (`bt_embed_*`), backlinks & orphan inspector (`Alt+B`), selectable agent CLI in Ask Bar (`Ctrl+E`), and standalone read-only `bt-mcp` v2 (`capabilities`, `inputSchema`, `backlinks`, `hybrid` flag, dynamic >64 KB JSON-RPC reader).

---

## Near-Term (Before Public `v0.2.0-beta`)

1. **Choose & add root `LICENSE`:** Publish the repository owner's chosen license file at the project root and update packaging metadata accordingly.
2. **Interactive Platform Verification:**
   - Live human testing of `pty_unix.c` + Wayland/X11 desktop behavior on Linux.
   - Windows IME composition (CJK / dead keys) and manual interactive resize testing of full-screen TUI tools (`vim`, `htop`, `claude`) in the embedded terminal.
3. **Distribution Installers:** Add Windows Inno Setup / ZIP release archive packaging on top of `scripts/package-win.ps1` and Linux AppImage / Flatpak manifest.

---

## Medium-Term (`v0.3.0` — Workspace & Query Power)

- **Logseq Parity Core (`docs/logseq-parity.md` `P1–P9`):** Task checkbox state queries, properties/Datalog-style vault query engine, vault-wide link-aware note rename, and static Markdown/HTML/PDF export (including rendered LaTeX math).
- **Local Neural Embeddings Backend:** Optional external local embedding HTTP/ONNX endpoint alongside the built-in zero-dependency FNV-1a token feature vector index.
- **Editor Math Controls:** User-configurable math scale factor and inline parse diagnostic hover tooltips.

---

## Long-Term (`v1.0.0`)

- Cross-platform binary releases with CI-signed installers/packages (Windows Authenticode / Linux Flatpak), user documentation site, and extensible MCP read/review protocol extensions.
