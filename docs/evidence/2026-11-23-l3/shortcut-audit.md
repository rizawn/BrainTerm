# L3 Shortcut Conflict Audit (`docs/evidence/2026-11-23-l3/shortcut-audit.md`)

## 1. Existing Application & Widget Bindings (G-9)

| Scope | Keybinding | Action / Behavior | Source Reference |
|---|---|---|---|
| `GApplication` | `Ctrl+S` (`<Primary>s`) | `app.save-active` | `src/app.c:3578` |
| `GApplication` | `Ctrl+N` (`<Primary>n`) | `app.new-note` | `src/app.c:3579` |
| `GApplication` | `F2` | `app.rename-active` | `src/app.c:3580` |
| `GApplication` | `Ctrl+P` (`<Primary>p`) | `app.quick-open` | `src/app.c:3581` |
| `GApplication` | `Ctrl+E` (`<Primary>e`) | `app.ask` | `src/app.c:3582` |
| `GApplication` | `Ctrl+O` (`<Primary>o`) | `app.open-vault` | `src/app.c:3583` |
| `GApplication` | `F5` | `app.rescan-vault` | `src/app.c:3584` |
| `GApplication` | `F9` | `app.toggle-sidebar` | `src/app.c:3585` |
| `GApplication` | `Ctrl+G` (`<Primary>g`) | `app.toggle-graph` | `src/app.c:3586` |
| `GApplication` | `Ctrl+Shift+F` (`<Primary><Shift>f`) | `app.toggle-graph-debug` | `src/app.c:3587` |
| `GApplication` | `Alt+Left` (`<Alt>Left`) | `app.nav-back` | `src/app.c:3588` |
| `GApplication` | `Alt+Right` (`<Alt>Right`) | `app.nav-forward` | `src/app.c:3589` |
| `GApplication` | ``Ctrl+` `` (`<Primary>grave`) | `app.toggle-terminal` | `src/app.c:3590` |
| `GApplication` | `Ctrl+Q` (`<Primary>q`) | `app.quit` | `src/app.c:3591` |
| `BtTerminal` | `Ctrl+Shift+C/V/F`, `Alt+Enter`, `Shift+PageUp/Down` | Terminal copy/paste/find, newline, scrollback; other keys forwarded to PTY | `src/terminal.c:528–560` |
| `BtTermTabs` | `Ctrl+Tab`, `Ctrl+1..8` | Cycle / select terminal tabs | `src/ui/termtabs.c:73–78` |
| `BtFileTree` | `F2`, `Delete` | Rename / delete selected item | `src/ui/filetree.c:257–264` |
| `BtGraphView` | `F` / `f`, `Escape` | Fit graph / clear selection | `src/graphview.c:1466–1470` |
| `BtQuickOpen` | `Escape`, `Up`, `Down` | Close / move quick-open selection | `src/ui/quickopen.c:101–113` |
| `BtAskBar` | `Escape` | Close Ask bar | `src/ui/askbar.c:170` |
| `BtEditorPane` | `Ctrl` press/release | Show pointer cursor on link hover | `src/ui/editorpane.c:1526–1527` |

## 2. Appendix B Candidate Conflict Matrix

| Action | Candidate | App Shortcut Conflict? | GTK / GtkSourceView Conflict? | Terminal Conflict? | Status & Choice |
|---|---|---|---|---|---|
| Open link at caret | **`Ctrl+Enter`** (`Preferred`) | **None.** Not in `app_actions`. | **Safe.** Default `GtkSourceView` `Return` inserts a newline; binding `Ctrl+Enter` in `GTK_PHASE_CAPTURE` on `pane->view` activates only when caret is inside a link span (`bt_link_span_at`), otherwise returns `FALSE` so newline/default handling is untouched (`CK02`). | **None.** Controller attaches only to `pane->view`, never captures keys when `BtTerminal` holds focus (`CK10`). | **SELECTED:** `Ctrl+Enter` (`GDK_CONTROL_MASK` + `Return`/`KP_Enter`) |
| Next link | **`Ctrl+Alt+Right`** (`Preferred`) | **None.** App uses `Alt+Right` only (no Ctrl). | **Safe.** `GtkTextView` binds `Ctrl+Right` (word) and `Alt+Right` (unconsumed/app nav-forward), not `Ctrl+Alt+Right`. Intercepted in `GTK_PHASE_CAPTURE` on `pane->view` (`CTRL | ALT`, `!SHIFT`, `!SUPER`). | **None.** Terminal handles its own focus controller. On Windows 11 desktop without virtual-desktop/display-rotation hotkeys intercepted, GTK receives `Ctrl+Alt+Right`. | **SELECTED:** `Ctrl+Alt+Right` (`GDK_CONTROL_MASK | GDK_ALT_MASK` + `Right`) |
| Previous link | **`Ctrl+Alt+Left`** (`Preferred`) | **None.** App uses `Alt+Left` only (no Ctrl). | **Safe.** Same as `Ctrl+Alt+Right`; handled in `GTK_PHASE_CAPTURE` on `pane->view`. | **None.** Scoped strictly to `pane->view` focus. | **SELECTED:** `Ctrl+Alt+Left` (`GDK_CONTROL_MASK | GDK_ALT_MASK` + `Left`) |
| Nav back / forward | `Alt+Left` / `Alt+Right` | Existing L2 app bindings (`app.nav-back`, `app.nav-forward`) | None | Forwarded only if `!nav-back/forward` | **KEPT UNCHANGED** (verified via `CK11`) |

## 3. Summary
- **Unresolved conflicts:** `0`. All preferred Appendix B bindings (`Ctrl+Enter`, `Ctrl+Alt+Right`, `Ctrl+Alt+Left`) bind cleanly to `pane->view` in `GTK_PHASE_CAPTURE` and never trigger while `BtTerminal` is focused.
- Recorded for ADR-025 (`L3-6b`).
