# Changelog — BrainTerm v0.1.0-mvp

## Unreleased (v0.2.0-prebeta — Full Feature Integration, Graph G3 & Hardening)
### Interactive Workspace, Editor & Memory UI
- Added in-document **Find & Replace** (`Ctrl+F` / `Ctrl+H`) in the editor with match counting, case sensitivity toggle, `Replace` / `Replace All`, `search-match` style definitions in `bt-ink-dark.xml` & `bt-ink-light.xml`, and clean empty-vault **Starter Vault** onboarding (`Open Vault…` / `Create Starter Vault…` from `vault-template/`).
- Wired **Chat Importer** (`>import chats`) for idempotent ChatGPT/Claude/Codex JSON & JSONL transcripts, **Review AI Tags** (`>review ai tags`) with `Promote`/`Dismiss` backed by `bt_organizer_*` and `R9` malformed-YAML refusal, **Sync Embeddings** (`>sync embeddings`), and **Backlinks & Orphans** (`Alt+B`).
- Upgraded Ask / Search Bar (`Ctrl+E`) with keyboard Up/Down navigation, `Alt+Up/Down` query history, a **Hybrid** FTS5 + offline vector search toggle showing cached vector count, and configurable AI CLI target (`claude` / `codex` / `gemini`) with preflight availability warnings before checkpoint/tab spawn.
- Added post-rollback vault synchronization so session rollback immediately updates the SQLite index, editor buffer, and link snapshot; persisted `[editor] show_line_numbers`, `[search] hybrid_enabled`, and `[agent] preferred_cli` in `state.ini`.
- Added an in-app **Keyboard Shortcuts** dialog (`F1` / menu), a **New Starter Vault…** menu entry (`app.create-starter-vault`), accessible labels (`GTK_ACCESSIBLE_PROPERTY_LABEL`) on icon-only header/sidebar/graph buttons, and automatic Back/Forward navigation history updates when notes are renamed or deleted (`bt_nav_history_rename_path`, `bt_nav_history_remove_path`).

### Brain Graph G3 & Terminal Polish
- Added **Brain Graph G3** interactions in 2D: interactive **drag-to-pin** with neighbor spring reheat, incremental live force settling (automatically stopping at 0 idle redraws once converged), fast GPU VBO position-only updates (no Pango atlas rebuild during drag/settle), **`Re-layout`** (`R`), context-menu Pin/Unpin/Unpin All, and **`Highlight Orphans`** (`O`) across both OpenGL and Cairo renderers.
- Added automatic terminal focus preservation on tab switch/close, middle-click tab close, and screen-reader/tooltip labels on terminal close buttons and the vault tree view.

### Core Database, Embeddings, MCP v2 & Release Packaging
- Expanded `BtIndex` with `bt_index_list_recent_notes()` (`ORDER BY updated DESC`), `bt_index_list_orphan_notes()`, `bt_index_list_backlinks()`, `bt_index_list_pending_ai_tags()`, `bt_index_list_embeddings()`, and `bt_index_embedding_count()`; updated `bt_embed_hybrid_search()` to load persisted SQLite vectors without redundant disk reads and guard against non-finite (`NaN`/`Inf`) vector components.
- Hardened input bounds and fault isolation across core modules: bounded YAML frontmatter parsing (256 KiB cap), bounded importer reads (64 MiB cap), unique hashed fallback slugs for non-ASCII titles (`untitled-<hash>`), malformed-JSONL error reporting without empty note creation, `%`/`'`/`!` shell metacharacter sanitization alongside POSIX metacharacters, `.bak` backup on corrupt `dismissed_tags.json`, and `top_k` schema exposure on `bt-mcp` `search_memory`.
- Upgraded `bt-mcp` to read dynamic >64 KB JSON-RPC lines (up to 8 MiB), advertise `capabilities` and tool `inputSchema`, sort `recent` by newest update, expose read-only `backlinks`, and support optional `hybrid` vector fusion on `search_memory`.
- Hardened `scripts/package-win.ps1` to resolve DLLs for `bt-mcp.exe`, verify `bt-mcp.exe` SHA-256 and isolated-PATH JSON-RPC initialization alongside `bt.exe --smoke-test`, and bundle `docs/THIRD_PARTY_LICENSES.md` + font licenses into `dist/brainterm/licenses/`.
- Added `docs/THIRD_PARTY_LICENSES.md`, `docs/ROADMAP.md`, `docs/KNOWN_LIMITATIONS.md`, and `docs/RELEASE_CHECKLIST.md`.

### Clickable Links & Math Tokenizer (v0.2.0 Baseline)
- Click any `[[wikilink]]`, Markdown link, or URL directly in the editor to open it; when the cursor is already inside a link, clicking edits the link text instead (`Alt+Click` always places the cursor, `Ctrl+Click` always opens).
- Clicking a link to a note that does not exist yet now opens a preview popover showing the exact path and any new folders to be created before creating the note.
- Clicking a link that matches multiple notes with the same name opens a chooser popover so you can pick the right folder or copy the full `[[folder/note]]` link with `Ctrl+C`.
- Navigate between links from the keyboard with `Ctrl+Alt+Right` (next link) and `Ctrl+Alt+Left` (previous link), open the link at the cursor with `Ctrl+Enter`, and jump back or forward through your reading history with `Alt+Left` and `Alt+Right`.
- Added menu toggles for **Open links with single click** and **Confirm external URLs**, plus automatic blocking of unsafe script/executable links and warnings when a URL contains embedded login credentials.
- Fixed italic formatting inside `$...$` and `$$...$$` math formulas so underscores and asterisks inside equations no longer slant surrounding text.
- Added `examples/links-demo.md` to the new-vault template demonstrating resolved, aliased, anchored, unresolved, ambiguous, external, blocked, and placeholder links.

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
