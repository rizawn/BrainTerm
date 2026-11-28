# BrainTerm — Product & Application Overview

## 1. Product Purpose, Audience & Design Philosophy

BrainTerm embodies a cohesive local-first thesis: **"My notes are the AI's memory, my terminal is the AI's hands, my graph is the map of my brain."** Instead of splitting note writing, knowledge retrieval, visualization, and agent execution across separate tools, BrainTerm combines a plain-Markdown vault, an offline FTS5 + lexical vector engine, an interactive OpenGL/Cairo Brain Graph, and an embedded multi-tab PTY terminal inside one native C17/GTK4 application.

### Target Audience & Competitive Advantage
- **Primary Users:** Software developers, systems researchers, technical writers, academic scientists, and builders who maintain hundreds to thousands of plain `.md` notes alongside terminal workflows (`claude`, `codex`, `gemini`).
- **Workspace Cohesion:** Eliminates constant alt-tabbing between Markdown editors, search scripts, and shell sessions inside one vault-rooted GTK4 window.
- **No Lock-In Advantage:** Unlike proprietary synced wikis or browser wrappers, BrainTerm operates directly on ordinary files and standard Git checkpoints.
- **Closed AI Loop:** Real `ConPTY` / POSIX PTY execution produces trackable edits and transcripts that automatically become searchable via FTS5, clickable wikilinks, and Brain Graph nodes.

### Normative Product Principles ([docs/project.md](../../project.md) P1–P7)
| Principle | Concrete Operational Guarantee |
|---|---|
| **P1 Local-First** | Zero user accounts, cloud servers, or telemetry; full-text lookup, 128-dim lexical vectors, LaTeX math, and Brain Graph execute completely offline. |
| **P2 Plain Markdown** | Notes remain standard UTF-8 `.md` files with YAML frontmatter readable by Git, Vim, VS Code, or Obsidian without lock-in. |
| **P3 Human != AI** | Frontmatter differentiates `source: human`, `source: ai`, and `source: import`, separating human `tags` from AI-proposed `tags_ai`. |
| **P4 Never Delete** | Automation never alters human prose bodies or overrides promoted/dismissed human tag locks (`>review ai tags`). |
| **P5 Index = Cache** | `<vault>/.bt/index.db`, embedding vectors, and `.brainterm/` palette caches are disposable artifacts rebuildable from `.md` sources. |
| **P6 Terminal-First** | Zero chat-bubble sidebars; `?question` queries compose sanitized citation prompts directly for CLI agents inside embedded PTY tabs. |
| **P7 Reversible** | Automatic `libgit2` commits snapshot repository state before agent runs and organizing batches with visual rollback. |

---

## 2. User Interface Architecture, Styling & Interactive Elements

### Visual Design System & Bundled Assets
BrainTerm styles every GTK4 / `libadwaita` surface using the custom **"Dusk"** design system compiled into GResources (`data/theme.css`, verified 1:1 against `src/theme.h` via `test_theme`):
- **Dusk Surface Palette:** Uses `#0E0F12` window backdrop, `#15171C` panel cards (`#bt-sidebar`, `#bt-graph-card`, `#bt-termtabs`), and `#8B7CF6` focus/link accent.
- **Runtime Scheme Switcher:** `Preferences` exposes `Follow System`, `Dark (Ink)`, and `Light (Paper — planned)`, while developer builds support live hot reload via `BRAINTERM_CSS_DEV`.
- **Always-Dark Terminal Rule (`ADR-002`):** Embedded PTY canvases stay intentionally dark across themes so 16-color ANSI TUIs retain legibility.
- **Bundled OFL Typography (`data/fonts/`):** Ships `InterVariable.ttf` (UI), `JetBrainsMono-Regular.ttf` / `JetBrainsMono-Bold.ttf` (editor, code, VT cells), and `STIXTwoMath-Regular.otf` (HarfBuzz formulas).
- **Vector Iconography (`data/icons/`):** Embeds `16` bespoke monochrome symbolic SVG assets (`bt-*-symbolic.svg`) across toolbars, sidebars, graph actions, and status strips.

```text
+-----------------------------------------------------------------------------------+
| [Sidebar F9]  BrainTerm · Vault Name     [Editor | Sessions | Graph] [Agent v][≡] |
+-------------------+---------------------------------------+-----------------------+
| Vault        [+][^]| [Find / Replace bar (Ctrl+F / H)]     | View: 2D|3D|Galaxy|   |
|                   | [External Conflict Banner if drifted] |   Timeline  Quality   |
| > projects/       |                                       |   Style: Clean|Glow   |
|   conpty.md       |   GtkSourceView 5 Markdown Editor     |   Links Tags Ghosts   |
|   welcome.md      |   - Custom bt-markdown syntax         |   [+][-][Fit][Reset]   |
|                   |   - Clickable [[wikilinks]] & URLs    |   [Orphans][Re-layout]|
|                   |   - Live $...$ & $$...$$ LaTeX math   |                       |
|                   +---------------------------------------+   Brain Graph Canvas  |
|                   | [Terminal Tab 1: shell] [x]  [+]      |   (OpenGL 3.3 / Cairo)|
|                   |   ConPTY + libvterm Grid (Always Dark)|   + Legend & Tooltip  |
+-------------------+---------------------------------------+-----------------------+
| [Folder] Vault · N notes · [Notice]                          [git oid] · build .. |
+-----------------------------------------------------------------------------------+
```

### Detailed Panes, Toolbars & Utility Dialogs (`src/app.c` & `src/ui/*`)

#### Top Chrome (`#bt-header`) & Primary Menu
- **Header Start:** Sidebar toggle (`F9`, explicit a11y label `"Toggle Sidebar"`) and `AdwWindowTitle` showing vault title, active note path, and dirty dot (`●`) / saved (`○`) indicator.
- **Header End:** Mode switcher (`Editor`, `Sessions`, side-by-side `Graph` split `Ctrl+G`), `Agent` `AdwSplitButton` (`Claude here` click action; menu `Codex here`, `Gemini here`, `Rollback to checkpoint`), and Main Menu (`≡`, a11y label `"Main Menu"`).
- **Primary Menu — Vault Section:** `Open Vault…`, `New Starter Vault…`, `New Note`, `Import Chats / Sessions…`.
- **Primary Menu — Memory & Git Section:** `Backlinks & Orphans…`, `Rebuild Index`, `Audit Index`, `Organize Vault`, `Review AI Tags…`, `Sync Embeddings`, `Checkpoint`, `Rollback…`.
- **Primary Menu — Settings & Help Section:** `Open links with single click`, `Confirm external URLs`, `Preferences`, `Keyboard Shortcuts` (`F1`), `About BrainTerm`.

#### Vault Explorer Sidebar (`#bt-sidebar`, `BtFileTree`)
- Displays hierarchical vault folders and notes (`"Vault notes"` accessible label) alongside `+` New Note (`"New Note"`) and Collapse (`"Collapse Sidebar"`) header actions.
- Right-click context menu offers `Open`, `Rename`, `Duplicate`, `Delete`, `New Note`, `Open terminal here`, `Copy path`, `Reveal`, and `Rescan`.

#### Editor & Sessions Stack (`#bt-view-stack`, `AdwViewStack`)
- **`BtEditorPane` (`Editor` view):** `GtkSourceView 5` using `bt-markdown.lang`, ~72ch padded reading column, toggleable line numbers, and `1 s` autosave debounce (`Ctrl+S` immediate flush).
- **Editor Overlays:** Collapsible Find/Replace bar (`#bt-find-bar`, `#bt-replace-row`), external drift conflict banner (`Review diff`, `Keep mine`, `Reload`), create/ambiguity popovers, rendered LaTeX previews, and empty-vault Starter Vault buttons (`Open Vault…` / `Create Starter Vault…`).
- **`BtSessionsPanel` (`Sessions` view):** Lists tracked terminal and CLI agent runs from `.bt/sessions.json` with one-click **Resume** and **Rollback** controls.

#### Brain Graph Card (`#bt-graph-card`, `BtGraphView`)
- **Canvas:** Split-pane OpenGL 3.3 Core (`#bt-graph-gl`) or software Cairo (`#bt-graph-cairo`) viewport.
- **Mode & Style Controls (`#bt-graph-toolbar`):** `View: 2D | 3D | Galaxy | Timeline`, Timeline toggles (`Organic | Kept`, `Lane: Folder | Type`), `Quality: Low | High`, and `Style: Clean | Glow`.
- **Filters & Actions:** Toggles (`Links`, `Tags`, `Ghosts`, `Pulse`, `Reduce motion`, `Color by: Folder | Type`), accessible icon controls (`Zoom in`, `Zoom out`, `Fit graph`, `Reset view`), `Orphans` (`O`), and `Re-layout` (`R`).
- **HUD & Overlays:** Corner mode badge (`#bt-graph-mode-badge`), hover readout (`#bt-graph-hover-readout`), bottom-left scrollable Andromeda legend, and `Ctrl+Shift+F` FPS debug overlay.

#### Terminal Notebook (`#bt-termtabs`) & Status Bar (`#bt-statusbar`)
- Hosts up to 8 live tabs displaying working directory labels, accessible close buttons, middle-click closure, and `Ctrl+Shift+F` scrollback find.
- Status bar displays vault name, indexed note count, transient status notices, active git checkpoint SHA, and `BT_BUILD_STAMP`.

#### Modal Dialogs & Search Overlays
- **Quick Open (`Ctrl+P`):** Incremental fuzzy title and path note switcher.
- **Ask / Search / Command Bar (`Ctrl+E`):** Unified modal supporting keyword lookup (`Hybrid` toggle, `Up`/`Down` list selection), `?question` Ask mode (command preview citing top 5 hits), `>` commands (`>import chats`, `>review ai tags`, `>sync embeddings`, `>organize vault`), `Alt+Up/Down` 10-entry history, and active CLI badge.
- **Memory & Utility Dialogs:** Backlinks & Orphans (`Alt+B`), Review AI Tags (`Promote`/`Dismiss` permanent human locks), Import Chats (`Conversations JSON`/`Session JSONL`), searchable `F1` Keyboard Shortcuts cheatsheet, Git Rollback diff selector, and `Preferences` (`Follow System`/`Dark`/`Light`, `show_line_numbers`, `hybrid_enabled`, `preferred_cli`).

---

## 3. End-to-End Functional Workflows

### 3.1 First Launch, Sandbox Config & Starter Vault Creation
1. Launching `bt.exe` registers private OFL fonts (`InterVariable`, `JetBrainsMono`, `STIXTwoMath`) prior to GTK realization.
2. Settings load from `%LOCALAPPDATA%/brainterm/state.ini` unless overridden by `BT_CONFIG_DIR` for isolated QA runs.
3. Passing `--vault <dir>` opens a vault directly; otherwise, BrainTerm restores the previous vault or presents empty-state onboarding.
4. Selecting **Create Starter Vault…** (empty state) or `New Starter Vault…` (Main Menu) populates `vault-template/` into an empty directory (`AGENTS.md`, `CLAUDE.md`, `.gemini/settings.json`, `.gitignore`, and `5` starter notes including `examples/links-demo.md`), rejecting non-empty directories (`BT_ERROR_INVALID`).

### 3.2 Daily Note Writing, Find/Replace, Links, Math & Brain Graph
- **Safe Editing & Autosave:** Edits persist atomically via `.md.tmp` commits on `Ctrl+S`, note switching, or `1 s` idle debounce, updating FTS5, links, and Brain Graph automatically.
- **In-Document Find & Replace:** `Ctrl+F` opens Find only (`#bt-find-bar`); `Ctrl+H` exposes `#bt-replace-row` (`Replace`, `Replace All`, `Case sensitive`).
- **Clickable Wikilinks & History:** Single-clicking (when enabled) or `Ctrl+Click` / `Ctrl+Enter` opens `[[wikilinks]]`, `[label](url)`, `#headings`, and URLs while preserving in-span editing (`Alt+Click` moves cursor; `Ctrl+Alt+Right/Left` steps spans).
- **Safe Creation & Disambiguation:** Unresolved wikilinks open `BtCreatePopover` confirmation before file creation; ambiguous stems open `BtChooserPopover` (`Ctrl+C` copies `[[folder/name]]`). `Alt+Left` / `Alt+Right` traverse `BtNavHistory`, automatically updated across note renames and deletions.
- **Inline Math Compilation:** Leaving `$...$` or `$$...$$` renders HarfBuzz OpenType `MATH` equations inline while keeping raw LaTeX text editable in Markdown.
- **Brain Graph Navigation:** Switch across `2D` (`Clean`/`Glow`), `3D`, `Galaxy`, and `Timeline`; drag 2D nodes to pin them, right-click to Pin/Unpin, press `R` to re-layout, `O` to isolate orphans, `Space` to fold folders, and `Tab` to cycle overlaps.

### 3.3 Agent Launch, Importers, Tag Review & External MCP Setup
- **Terminal & Ask Bar (`Ctrl+E`):** Type plain keywords for FTS5/`Hybrid` retrieval, `?question` to preview a sanitized agent command citing up to 5 top notes, or `>command` (`>import chats`, `>review ai tags`, `>sync embeddings`, `>organize vault`).
- **One-Click CLI Agents:** Clicking `Agent` or running from Ask mode verifies `claude`/`codex`/`gemini` on `PATH`, records a `libgit2` checkpoint, exports `TERM_PROGRAM=brainterm` + `BT_ACTIVE_NOTE`, and sends a sanitized prompt into the active terminal tab.
- **Importers & Tag Governance:** `>import chats` converts ChatGPT/Claude `.json` or Claude Code/Codex `.jsonl` logs into `inbox/import/<tool>/`; `>review ai tags` permanently promotes or dismisses `tags_ai` proposals (`P4`); `>sync embeddings` populates SQLite vector BLOBs.
- **Headless MCP Client Config (`bt-mcp.exe`):** Configure Claude Desktop or Cursor cleanly:
  ```json
  {
    "mcpServers": {
      "brainterm": {
        "command": "C:/path/to/dist/brainterm/bt-mcp.exe",
        "args": ["--vault", "C:/path/to/your/vault"]
      }
    }
  }
  ```

### 3.4 Clean Application Shutdown
1. Pressing `Ctrl+Q` or closing the window flushes pending buffer edits via `bt_editorpane_flush_save()`.
2. All open ConPTY/POSIX terminal tabs terminate cleanly within `2000 ms` (`bt_termtabs_close_all()`).
3. `GFileMonitor` stops, pending idle timers cancel, `link_snap` clears before child widget teardown, `bt-db-writer` joins, and `bt_checkpoint_shutdown()` releases `libgit2`.

---

## 4. Keyboard Shortcuts & Documented Functional Boundaries

| Shortcut | Context | Action |
|---|---|---|
| `Ctrl+O` / `Ctrl+N` / `F2` / `Ctrl+S` | Global | Open Vault / New Note / Rename Active Note / Atomic Save |
| `Ctrl+F` / `Ctrl+H` / `Ctrl+Enter` | Editor | Find / Find & Replace / Open or Create Link at Caret |
| `Ctrl+Alt+Right` / `Ctrl+Alt+Left` | Editor | Jump Caret to Next / Previous Link Span |
| `Alt+Left` / `Alt+Right` / `Ctrl+P` / `Ctrl+E` | Global | Back / Forward History / Quick Open / Search-Ask-Command Bar |
| `Ctrl+G` / `Alt+B` / `F` / `R` / `O` / `Space` | Global / Graph | Toggle Graph / Backlinks / Fit / Re-layout / Orphans / Toggle Folder |
| `Ctrl+\`` / `Ctrl+Tab` / `Ctrl+1..8` / `Ctrl+Shift+F` | Terminal / Debug | Toggle Terminal / Next Tab / Switch Tab / Scrollback Search or Graph FPS |
| `F9` / `F5` / `F1` / `Ctrl+Q` | Global | Toggle Sidebar / Rescan Vault / Keyboard Shortcuts Modal / Quit |

### Documented Operational Boundaries ([KNOWN_LIMITATIONS.md](../../KNOWN_LIMITATIONS.md))
- **Lexical Hash Vectors Instead of Neural Embeddings:**
  - Built-in semantic search (`src/embed.c`) encodes `128-dim` signed FNV-1a word and character-trigram hashes rather than deep transformer embeddings, favoring morphological matches over abstract paraphrases.
- **No Bundled Local LLM Runtime (`BT_ORG_LOCAL_MODEL`):**
  - `BT_ORG_LOCAL_MODEL` exposes a caller callback hook without shipping GGUF/llama.cpp weights; standalone users run `BT_ORG_NO_AI` heuristics or `BT_ORG_CLI` (`claude`/`codex`/`gemini`).
- **Narrow Docked Graph Toolbars:**
  - Splitting Brain Graph narrower than `~400 px` horizontally can crowd overlay pill toggles visually; maximize Graph (`Ctrl+G`) or collapse the sidebar (`F9`) on dense vaults.
- **Pending Interactive Human QA Scope:**
  - Live Linux desktop execution (`pty_unix.c` + X11/Wayland GL), Windows CJK/dead-key IME composition, and interactive `vim`/`claude` ConPTY drag-resize ergonomics remain `UNVERIFIED` by human QA.
- **Deferred Logseq Parity Spec ([docs/logseq-parity.md](../../logseq-parity.md) `P1–P9`):**
  - Block outliners, `TODO`/`DEADLINE` scheduling, Datalog/property tables, daily journals, spaced-repetition flashcards, and static HTML/PDF exports remain intentionally unimplemented future work.
