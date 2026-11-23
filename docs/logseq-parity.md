# BrainTerm — Logseq-Style Knowledge Management Parity Spec

| Field | Value |
|---|---|
| Document | `docs/logseq-parity.md` |
| Version | 1.0 — 2026-11-22 |
| Baseline | `docs/project.md` v2.0 (P1–P7, ADR-001..ADR-009) |
| Reference | Public Logseq documentation + local UX observation (installed Electron binary confirmed at `Logseq.exe` v2.0.2; never unpacked or decompiled) |

---

## 1. Clean-Room & Legal Boundary

- **License check:** Logseq (`logseq/logseq`) is licensed under **GNU AGPL-3.0**.
- **Hard boundary:** BrainTerm MUST NOT contain any Logseq source code, translated code, icons, CSS, fonts, localization strings, or assets extracted from `app.asar` or the Logseq repository.
- **Allowed interoperability:** Compatibility with plain-text Markdown file conventions (`pages/`, `journals/`, `[[wikilinks]]`, `key:: value`, `id:: <uuid>`, `((uuid))`, task keywords, `SCHEDULED:`/`DEADLINE:`) is data-format interoperability only.
- **Process:** Every phase is specified here in our own words and implemented strictly from this spec and `docs/DECISIONS.md`.

---

## 2. File-Format & Guardrail Invariants

1. **Plain Markdown truth (P1, P2, P5):** Notes remain plain `.md` files editable by humans and CLI agents. YAML frontmatter (`docs/project.md` §7.1) remains canonical for page metadata. SQLite (`vault/.bt/index.db`) holds only rebuildable derived state.
2. **Round-trip safety:** Unknown frontmatter keys, unknown body syntax, comments, and line endings (CRLF/LF) are preserved byte-for-byte.
3. **Single write choke point:** All file writes pass through `vault.c` (`vault_resolve()` path guard + `vault_atomic_write()`).
4. **Human Lock & Agent guardrails (P3, P4, P7):** Agents (auto-organizer, terminal CLIs, `bt-mcp`) may write ONLY frontmatter `tags_ai`/`links` and `## AI draft` sections, always after an in-process libgit2 checkpoint (`checkpoint.c`). Human-authored lines are never overwritten by agents.
5. **Non-blocking UI (ADR-007):** All SQLite queries, file I/O, backlink scans, and query-block evaluations run off the GTK main thread (on `bt-db-writer` or `GThreadPool`) and marshal UI updates via `g_idle_add` / `g_main_context_invoke`.
6. **UI restraint:** Reuse Dusk tokens (`data/theme.css`, `src/theme.h`), existing cards, and right-side / sidebar slots. Do not redesign the shell.

---

## 3. Feature Parity Matrix (Phases P1–P9)

### P1 — Backlinks & Local Graph

- **What Logseq does:** Every page footer displays *Linked References* (every block mentioning `[[Page]]`, grouped by source page with surrounding context) and *Unlinked References* (plain-text mentions of the page title without brackets). Hovering a `[[link]]` shows a quick preview popover; a local graph shows the immediate neighborhood of the active page.
- **What BrainTerm will do:**
  - **Backlinks panel** (tab/toggle alongside Graph in the right card, keeping existing shell intact): *Linked References* grouped by source note showing line number + trimmed context snippet, and collapsible *Unlinked References* (FTS5 phrase matches of the active note's title excluding already-linked notes and titles < 3 chars). Clicking any item jumps straight to that note and scrolls to the exact line.
  - **Editor hover preview:** Hovering over a `[[wikilink]]` in `editorpane.c` shows a lightweight Dusk popover with the target note's title, tags, and first lines (or a "Ghost note — Ctrl+Click to create" hint).
  - **Local Graph mode:** Toggle Global / Local inside the existing Graph card; BFS neighborhood of the active note with depth `1..3` plus tag and folder prefix filters.
- **What we skip & why:**
  - Inline editable transclusion of backlink blocks directly inside the references footer (avoids multi-buffer editing hazards; jump-to-line keeps one canonical editor buffer per file).
  - Automatic one-click mass conversion of all unlinked mentions (easy to create false-positive links; keep explicit jump-and-edit).
- **Data / format & schema decision (ADR-014):**
  - No change to `.md` syntax.
  - New migration `migrations/0002_backlinks.sql` (`PRAGMA user_version = 2`): adds `link_refs(id, src_note_id, dst_raw, dst_note_id, line_no, col_no, context)` with indexes on `dst_note_id`, `dst_raw COLLATE NOCASE`, and `src_note_id`. Existing `edges` table stays intact.
  - `mdparse.c` gains line/column/context capture per wiki-link occurrence (outside code spans/fences); `index.c` updates `link_refs` incrementally on upsert/delete and adds async worker APIs so the UI thread never blocks.

---

### P2 — Daily Journals & Quick Capture

- **What Logseq does:** Opens on a continuous reverse-chronological stream of daily journal files under `journals/`, auto-creating today's file on demand, and lets users dump notes quickly throughout the day.
- **What BrainTerm will do:**
  - Auto-create `journals/YYYY-MM-DD.md` on first open of a date from `templates/journal.md` (if present) or a clean built-in default template with BrainTerm YAML frontmatter (`id`, `title`, `created`, `updated`, `source: human`, `tags: [journal]`).
  - **Today** action (`Ctrl+J`), **Prev/Next Day** navigation, and a compact **Journals** section/filter in the sidebar.
  - **Quick Capture** (`Ctrl+Shift+J` or Ask bar `>capture ...`): modal/inline entry that atomically appends a bullet line (with optional `HH:MM` timestamp) to today's journal without leaving the current note.
- **What we skip & why:**
  - Infinite-scroll multi-file merged journal stream (conflicts with single-file GtkSourceView editing and clean per-file dirty/conflict semantics; prev/next day + sidebar list gives fast daily navigation without multi-buffer complexity).
  - Configurable non-ISO filename date formats for newly created BrainTerm journals (causes cross-machine ambiguity; see ADR-010).
- **Data / format decision (ADR-010):** Canonical path `journals/YYYY-MM-DD.md` (ISO-8601). `journals/` is a regular vault folder (indexed, graphed, git-checkpointed like any other note).

---

### P3 — Tasks

- **What Logseq does:** Recognizes inline task keywords (`TODO`, `DOING`, `DONE`, `LATER`, `NOW`, `WAITING`, `CANCELED`), priorities (`[#A]`..`[#C]`), and Org-style `SCHEDULED:` / `DEADLINE:` timestamps; lets users click to cycle state and query tasks across the graph.
- **What BrainTerm will do:**
  - Core parser (`src/tasks.c/h`) extracts both keyword tasks (`TODO`, `DOING`, `DONE`, `LATER`, `WAITING`, `CANCELED`; `NOW` mapped to `DOING` on parse) and GFM checkboxes (`- [ ]` / `- [x]`), plus optional `[#A]`/`[#B]`/`[#C]` priority and `SCHEDULED: <YYYY-MM-DD>` / `DEADLINE: <YYYY-MM-DD>` dates (inline or on immediate indented lines).
  - Migration `migrations/0003_tasks.sql`: `tasks(id, note_id, line_no, status, style, priority, scheduled, deadline, text)` updated incrementally per note.
  - **Tasks panel** (right-card tab): filter by status, page, tag, and date bucket (Overdue / Today / Upcoming / Unscheduled); click to jump to file & line; toggle/cycle status from either the panel or a shortcut in the editor via atomic surgical line write-back through `vault.c`.
- **What we skip & why:**
  - Time-tracking (`LOGBOOK:` clock-in/out durations) and recurring repeaters (`+1w`, `.+1d`) in v1 — keeps write-back minimal and deterministic; can be layered later without schema breaks.
- **Data / format decision (ADR-011):** Support both keyword and GFM task formats; toggle preserves the original line's style and line endings byte-for-byte outside the marker token.

---

### P4 — Inline Properties & Safe Queries

- **What Logseq does:** Supports `key:: value` properties at page or block level and dynamic query blocks (`{{query ...}}` simple queries + Clojure/Datalog `#+BEGIN_QUERY` advanced queries) that render live results inline.
- **What BrainTerm will do:**
  - Parse inline `key:: value` lines (outside code fences) in addition to YAML frontmatter and index them into `properties(note_id, line_no, key, value, origin)` (`migrations/0004_properties.sql`).
  - Core query engine (`src/query.c/h`) parses `{{query <expr>}}` blocks using a clean, bounded s-expression grammar (`and`, `or`, `not`, `[[page]]`, `#tag`, `(task ...)`, `(property ...)`, `(priority ...)`, `(between ...)`, `(path ...)`, `"text"`).
  - Compiles strictly to **parameterized SQLite prepared statements** (zero string interpolation), executes on `bt-db-writer` with `sqlite3_progress_handler` timeout (250 ms) and hard row cap (default 200, max 1,000).
  - Live results render in a read-only panel/card view and auto-refresh when the index changes.
- **What we skip & why:**
  - Clojure/Datalog `#+BEGIN_QUERY` scripting and custom JS/Clojure result view functions (violates pure-C stack ADR-001, introduces arbitrary code execution surface, and is unnecessary when parameterized SQLite covers 95% of real queries).
- **Data / format decision (ADR-012):** YAML frontmatter stays canonical for page metadata; inline `key:: value` is additive; query syntax is our documented safe subset of `{{query ...}}`.

---

### P5 — Blocks (Outliner-Lite)

- **What Logseq does:** Treats every bullet as a first-class outline tree node with keyboard indentation, reordering, folding, block zooming, UUID block references `((uuid))`, and live editable `{{embed ((uuid))}}` / `{{embed [[page]]}}`.
- **What BrainTerm will do:**
  - **Outliner-lite editor helpers** in `editorpane.c`: `Tab` / `Shift+Tab` indent/outdent current bullet subtree, `Alt+Up` / `Alt+Down` move bullet subtree up/down, fold/unfold indented children, and "Zoom into block" (focus view on a subtree with breadcrumb back to full note).
  - **Lazy Block IDs:** `id:: <uuidv4>` is written as an indented child line ONLY when a block is first referenced or embedded ("Copy block ref" / "Embed block").
  - Migration `migrations/0005_blocks.sql`: `blocks(id, note_id, line_no, end_line, indent, text)` indexed incrementally for blocks carrying `id:: <uuid>`.
  - **References & Embeds:** `((uuid))` gets hover preview + Ctrl+Click jump to source; `{{embed ((uuid))}}` and `{{embed [[page]]}}` render inline as read-only preview cards with a "Jump to source" action (cycle-checked, max depth 3).
- **What we skip & why:**
  - Mandatory bullet-only document mode (BrainTerm notes are general Markdown — prose, headings, fenced code, tables, and bullets all coexist).
  - Live two-way editing inside transcluded embeds (avoids split-brain buffers and dirty-conflict races; single-buffer editing with one-click jump is cleaner and crash-safe).
- **Data / format decision (ADR-013):** Standard `id:: <uuidv4>` child line, created lazily on first reference only; read-only inline embeds.

---

### P6 — Page Management

- **What Logseq does:** Supports hierarchical namespaces (`a/b/c`), `alias::` alternate names, `#tag` pages, an All Pages table with sort/filter, safe page renaming with automatic link rewriting, and random page discovery.
- **What BrainTerm will do:**
  - **Atomic Vault-Wide Rename (`src/rename.c/h`):** Takes a git checkpoint first (`bt-rename <old> -> <new>`), moves the file via `vault.c`, rewrites all `[[old]]`, `[[old|label]]`, `[[old#heading]]`, and frontmatter `links:` across referencing notes atomically (preserving CRLF/LF and unknown text, skipping code fences), updates the index, and exposes one-click rollback via the checkpoint.
  - **Namespaces & Aliases:** Support slash namespaces (`a/b/c.md` and `[[a/b/c]]` resolution + parent/child links in Backlinks) and frontmatter `aliases: [...]` / inline `alias::` indexed into `aliases(alias_lower, note_id)` (`migrations/0006_aliases.sql`) for wiki-link resolution, hover preview, backlinks, and Quick Open.
  - **Tag pages, All Pages & Random Note:** Clicking a `#tag` opens a virtual Tag View listing matching notes; "All Pages" sortable/filterable list view (title, folder, links, updated, source); `>random note` command / shortcut.
- **What we skip & why:**
  - Logseq's `___` filename encoding for namespaces (`a___b.md`) in native BrainTerm vaults — BrainTerm uses real subdirectories `a/b.md` (natural for CLI agents and `git`); the P8 importer translates `___` into subdirectories on import.
- **Data / format decision:** Real directories for namespaces (`a/b/c.md`); `aliases` supported in YAML frontmatter (preferred) and inline `alias::`.

---

### P7 — Authoring Helpers

- **What Logseq does:** Typing `/` opens a command palette for inserting templates, dates, tasks, embeds, and queries; supports exporting pages/graphs to Markdown or HTML.
- **What BrainTerm will do:**
  - **Slash menu (`/`):** Lightweight popup triggered on `/` at line start or after whitespace inside `editorpane.c` (keyboard navigable, Esc dismisses) to insert: Today/Tomorrow date, `TODO`/`DOING` task, `SCHEDULED`/`DEADLINE`, `{{query ...}}`, `{{embed ...}}`, and any `.md` template from `templates/` (with `{{date}}` / `{{time}}` / `{{title}}` placeholder substitution).
  - **Export (`src/exporter.c/h`):** Export active note or whole vault to clean standalone Markdown (optional embed expansion, frontmatter strip option) or self-contained Dusk-styled HTML using MD4C (strictly path-guarded; zero external CDN links).
- **What we skip & why:**
  - Rich WYSIWYG block widgets (drawers, LaTeX live editors, whiteboard cards) — keep the editor fast and plain-text-faithful.
- **Data / format decision:** User templates live in `templates/*.md` inside the vault (plain Markdown, tracked by git).

---

### P8 — Logseq Graph Importer

- **What Logseq does:** Stores a graph in `pages/`, `journals/`, `assets/`, and `logseq/` (EDN config/metadata).
- **What BrainTerm will do:**
  - Extend `src/importer.c/h` with `bt_import_logseq_graph(vault, index, src_dir, dry_run, &report, &err)`:
    - Strictly **READ-ONLY** on `src_dir` (never writes, deletes, or modifies the source Logseq folder).
    - Reads `pages/*.md` (translating `___` namespace separators into subdirectories under `pages/` or vault root) and `journals/*.md` (normalizing `YYYY_MM_DD.md` → `journals/YYYY-MM-DD.md`).
    - Converts top-of-file Logseq page properties (`title::`, `tags::`, `alias::`, etc.) into BrainTerm YAML frontmatter (`source: import`, preserving unknown properties), preserves inline `key:: value`, `id:: <uuid>`, `((uuid))`, and task markers verbatim.
    - Ignores `logseq/` config/cache directory completely.
    - **Idempotent** (re-running skips unchanged already-imported files) with a **dry-run report** listing files to create, skip, or warn about (e.g., invalid paths, collisions, Org-mode `.org` files skipped with notice).
- **What we skip & why:**
  - Importing `.org` (Org-mode) files or Logseq EDN config/plugins (`logseq/config.edn`, `logseq/custom.css`) — BrainTerm is Markdown-only and owns its own config/theme.
- **Data / format decision:** All imported files written atomically through `vault.c` after a git checkpoint (`bt-import logseq`).

---

### P9 — Agent Bridge (BrainTerm's Edge Over Logseq)

- **What Logseq does:** Has no built-in terminal, no git-checkpointed agent guardrails, and no native MCP server enforcing human-vs-AI text separation.
- **What BrainTerm will do:**
  - Extend `src/mcp/main.c` (`bt-mcp`) with tools:
    - Read-only (always enabled): `get_backlinks`, `list_tasks`, `run_query`, `get_journal`.
    - Gated write (opt-in flag `--allow-ai-drafts`, off by default per ADR-008): `append_to_journal` — takes a libgit2 checkpoint first and writes ONLY inside a `## AI draft` section of `journals/YYYY-MM-DD.md` via `vault_atomic_write()`, never touching human text.
  - Extend the Ask bar (`Ctrl+E`, `src/search.c`) to understand `tasks:` / `backlinks:` / `journal:` intents and surface matching tasks, backlinks, and daily entries alongside FTS5 search and commands.
- **What we skip & why:**
  - Arbitrary file-write or task-mutation MCP tools for external agents — violates P3/P4 (Human Lock). Agents propose in `## AI draft`; humans confirm or edit.
- **Data / format decision:** Every agent journal entry lands under `## AI draft` with RFC-3339 timestamp + agent identifier; human journal sections remain untouched.

---

## 4. Explicit Non-Goals

Whiteboards, PDF annotation, plugin marketplace / JavaScript plugin API, cloud sync, mobile clients, spaced-repetition flashcards (may revisit later), real-time multi-user collaboration, Clojure/Datalog scripting, Org-mode syntax.

---

## 5. Engineering, Performance & Verification Gates

- **Modularity:** Core logic in `libbt_core` (zero GTK includes); UI widgets under `src/ui/`.
- **Migrations:** Strict one-way numbered migrations (`0002_backlinks.sql` .. `0006_aliases.sql`), embedded in `src/index.c`, gated by `PRAGMA user_version`, with downgrade refusal and golden-rebuild determinism tests (P5).
- **Performance budgets:** Preserve all `docs/project.md` §10 budgets (scan 1k, FTS 1k, graph 1k/3k) and add 3,000-note benchmarks for backlinks (< 25 ms) and queries (< 50 ms). Zero SQLite/file I/O on the GTK main thread.
- **Testing:** CMocka unit tests for every parser, query compiler, rename engine, and importer (malformed inputs, CRLF, Unicode, huge notes, path-traversal attempts); crash-rig tests for vault-wide rename and task-toggle atomic write-back.
- **Release gate per phase:** Clean build (`-Dwerror=true`), all Meson tests passing, `scripts/package-win.ps1 -SmokeTest` passing, launch `dist\brainterm\bt.exe`, confirm build stamp, and update `docs/project.md` + `CHANGELOG.md`.
