# BrainTerm

> **"My notes are the AI's memory, my terminal is the AI's hands, my graph is the map of my brain."**

BrainTerm is a local-first, plain-Markdown second-brain with an embedded real terminal (ConPTY + `libvterm`), an auto-organizing memory engine, a 2D brain-shaped graph, and a read-only MCP memory bridge — built in pure **C17 + GTK4**.

## Quickstart (Windows MSYS2 UCRT64)

```powershell
# Configure, compile (-Dwerror=true), and run all 23 test suites
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
meson setup build -Dwerror=true
meson compile -C build
meson test -C build --print-errorlogs

# Package standalone Windows bundle + run isolated-PATH smoke test
.\scripts\package-win.ps1 -BuildDir build -OutDir dist\brainterm -SmokeTest

# Launch BrainTerm on a vault
.\dist\brainterm\bt.exe --vault path\to\your\vault
```

## Key Shortcuts
- `Ctrl+O` — Open Vault
- `Ctrl+S` — Save note atomically
- `Ctrl+P` — Quick Open note (fuzzy)
- `Ctrl+E` — Ask / Search / Command bar (`>` for commands, `?` for Ask evidence pane)
- ``Ctrl+` `` — Toggle terminal pane (`Ctrl+Tab` / `Ctrl+1..8` switch tabs, `Alt+Enter` fullscreen)
- `Ctrl+Shift+C` / `V` / `F` — Terminal copy, bracketed paste, scrollback search
- `Ctrl+Click` on `[[wiki-link]]` — Jump to or create note

## Standalone Read-Only MCP Bridge (`bt-mcp`)
Point Claude Desktop or Cursor to:
```json
{
  "command": "bt-mcp.exe",
  "args": ["--vault", "C:/path/to/your/vault"]
}
```
Exposes `search_memory`, `ask_memory`, `recent`, and `get_note` over stdio JSON-RPC 2.0 against `vault/.bt/index.db` opened strictly read-only.
