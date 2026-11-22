# Changelog — BrainTerm v0.1.0-mvp

## Shipped (M1–M8)
- **M1 (Vault & Editor):** Plain-Markdown vault open/scan (1.8 ms / 1k notes), ignore list, `vault_resolve()` path guard, atomic write + R1 `.md.tmp` crash recovery, GtkSourceView 5 + custom `bt-markdown.lang` (frontmatter, `[[wiki-link]]`, `#tag`, `## AI draft`), 1 s autosave, `Ctrl+P` quick open, Windows UCRT64 packaging smoke test.
- **M2 (Embedded Real Terminal):** Windows ConPTY (`CreatePseudoConsole` + Win32 Job Object `KILL_ON_JOB_CLOSE`) and POSIX `forkpty`, `libvterm 0.3.3` state machine, 10k-line trimmed scrollback ring + `Ctrl+Shift+F` search, coalesced Pango/Cairo grid renderer (0.046 ms/frame VT redraw), 100 ms debounced resize, OSC 52 off by default, VT conformance corpus + anti-zombie sweep test.
- **M3 (Multi-tab + AI Launcher + Checkpoints):** 8-tab terminal notebook with cwd badges and `Ctrl+Tab`/`Ctrl+1..8`, right-click "Open terminal here", Claude/Codex/Gemini CLI detection + fixed-form sanitized launch commands (`TERM_PROGRAM=brainterm`, `BT_ACTIVE_NOTE`), `vault-template/` (`AGENTS.md`, `CLAUDE.md`, `.gemini/settings.json`), `libgit2` in-process checkpoints + rollback (R10 refusal on locked repo), Sessions panel.
- **M4 (SQLite + FTS5 + Ask Bar):** §7.2 schema + FTS5 external-content triggers + `PRAGMA user_version=1` migrations + R2 corruption auto-rebuild, `libyaml` frontmatter parser/serializer, MD4C body extractor, ADR-007 single-writer SQLite thread, `Ctrl+E` Search / Ask (evidence pane + "Open in agent", no chat UI) / Command bar.
- **M5 (Watcher & Conflict Banner):** `GFileMonitor` watcher with 500 ms debounce, R4 rename-storm guard, R3 differential `body_hash` audit, non-destructive conflict banner (`[Review diff] [Keep mine] [Reload]`).
- **M6 (Auto-Organize & Human Lock):** No-AI rules backend (default), fixed-argv CLI backend, local-model hook, P4 human lock + promote/dismiss persistence, checkpoint-per-batch with R10 refusal.
- **M7 (2D Brain Graph):** Every `.md` file = 1 node invariant, forced brainstem/spine + force-relaxed cortex layout, ghost nodes + broken-link protection, filters, growth animation, 1k nodes / 3k edges at 10.46 ms/frame (> 60 fps).
- **M8 (Importers, MCP, Optional Embeddings):** ChatGPT/Claude/Claude Code/Codex importers (idempotent, P3 separation, original dates), standalone read-only `bt-mcp` stdio JSON-RPC server (`search_memory`, `ask_memory`, `recent`, `get_note`), optional offline embeddings + RRF hybrid search. 3D graph (MAY) cut for v0.1.0.

## Known Unverified (Requires Human Interactive Session)
- Live interactive TUI feel (`vim`, `htop`, `claude` TUI under manual window drag-resize).
- Windows IME composition (Vietnamese/CJK) in GtkSourceView 5 and terminal.
- Full 90-second interactive M7 demo script rehearsal.
- `pty_unix.c` compilation & execution on Linux (covered by Ubuntu GitHub Actions CI).
