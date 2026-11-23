# BrainTerm — Mega Prompt: Clickable Links + LaTeX Math Rendering

> Version 1.0 · 2026-11-23
> Stack: C17, GTK4 + libadwaita + GtkSourceView 5, Meson. Windows-first (MSYS2 UCRT64). Linux must keep building.
> Audience: the coding agent working inside the BrainTerm repository.
> Read this entire file before writing any code.

---

## 0. How to use this document

- One brief, two features:
  - **Part A** — clickable links in the editor (`[[wikilinks]]`, Markdown links, URLs).
  - **Part B** — LaTeX / math rendering in the editor (`$...$` and `$$...$$`).
- Work in **phases**. Each phase ends with a **STOP GATE**: report, then wait for my go-ahead.
- **MUST** = required. **SHOULD** = do it unless you write down why not. **MAY** = optional.
- "I" / "the user" = Riza, the owner of the project. I test on Windows 11 by running `dist\brainterm\bt.exe`.
- Every "it works" needs evidence: a test, a measurement, a capture, or a transcript.
- If this document and the repo disagree (file names, APIs, versions), the repo wins. Report the difference.
- You MAY propose a better approach in an ADR. Do not deviate silently.
- Keep reports short and factual. No marketing language.

---

## 1. Ground truth from my screenshots

### 1.1 Screenshot 1 — links in the editor

- Vault: `life2`, 310 notes. Open note: `statistical-statistics-notes/MathStats.md`.
- The note contains Markdown tables whose cells hold wikilinks, for example:
  - `[[chapter-01-descriptive/01-data-types-collection.md]]`
  - `[[chapter-02-probability/04-moments.md]]`
- The link target is a **relative path with folders and the `.md` extension**, not just a title.
- Links are already drawn purple and underlined, so some link tagging exists in `editorpane.c`. **Nothing happens when I click them.**
- The caret is inside a link on line 13 and the brackets `[[ ]]` are visible there (reveal-on-caret behavior exists).
- Links sit inside table rows, so the `|` character appears next to links. Wikilink alias syntax also uses `|`. Handle this carefully (see A.2).

### 1.2 Screenshot 2 — math in the editor

The note `statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md` contains raw LaTeX that shows as plain text:

```
Resample with replacement from original data $B$ times:
1. For $b = 1, \ldots, B$: draw sample of size $n$ with replacement
2. Compute statistic $\hat{\theta}^{*(b)}$ for each sample
3. Use empirical distribution of $\hat{\theta}^*$ for inference
$$(\hat{\theta}^*_{(\alpha/2)}, \hat{\theta}^*_{(1-\alpha/2)})$$
- Propose $\theta^* \sim q(\theta^* | \theta^{(t-1)})$
- Compute acceptance ratio: $r = \frac{f(\theta^* | x) q(\theta^{(t-1)} | \theta^*)}{f(\theta^{(t-1)} | x) q(\theta^* | \theta^{(t-1)})}$
- Accept $\theta^{(t)} = \theta^*$ with probability $\min(1, r)$; else $\theta^{(t)} = \theta^{(t-1)}$
```

- **Existing bug visible in the screenshot:** parts of these formulas are drawn in *italic*. The `*` and `_` inside the math are being treated as Markdown emphasis by the highlighter or by the live-preview tags. Math spans must be tokenized **before** emphasis and must be excluded from every Markdown inline rule.
- The formulas appear inside ordered lists, nested bullet lists, and on their own line as display math.
- Pipes `|` appear inside math (conditional probability, absolute value). They are not table separators here.

### 1.3 What I asked for (my words, translated)

- "I want `[[file]]` to behave like a hyperlink: clickable, and clicking opens it directly."
- "I want to compile LaTeX / math code so the formulas are visible as real math."
- Interpretation you MUST follow unless I say otherwise:
  - Clicking a link to a note opens that note **inside BrainTerm**.
  - Clicking a link to a non-note file (PDF, image, etc.) opens it with the **default Windows application**, under the safety rules in A.9.
  - Clicking `https://...` opens the default browser.
  - Math is rendered **in place in the editor**, with the LaTeX source revealed when the caret enters it.

### 1.4 Lessons from earlier work on this project

- I once ran a **stale binary** because `package-win.ps1` failed to overwrite a locked exe. Always launch `dist\brainterm\bt.exe` and confirm the **build stamp** in the status bar matches your last commit.
- Screenshots do not prove interaction. You will add self-test CLIs and capture tools (A.15, B.22).
- Use **Dusk theme tokens only**. A new color needs a token in `theme.css`, `theme.h`, `theme.c`, and `test_theme.c` parity.
- Keep core logic GTK-free (`libbt_core`). New math code goes in its own library (B.5).
- Reports MUST include measured numbers, not impressions.

---

## 2. Goals and non-goals

### 2.1 Goals

- **G-A1** Clicking a wikilink to an existing note opens it in the editor, with the right resolution rules for paths, titles, and ambiguity.
- **G-A2** Links to missing notes are visibly different and offer a safe "create note" flow.
- **G-A3** Links to non-note files and URLs open in the right external app, safely.
- **G-A4** Back / forward navigation works across link jumps.
- **G-A5** Link behavior is discoverable: hover cursor, tooltip, legend in settings.
- **G-A6** One resolver in the GTK-free core, shared by the editor, the graph (ghost nodes), the index, and the MCP server.
- **G-B1** `$...$` (inline) and `$$...$$` (display) render as typeset math in the editor.
- **G-B2** The LaTeX source is revealed when the caret or selection touches the formula, and the file on disk is never changed by rendering.
- **G-B3** Invalid LaTeX never crashes, never hides the source, and shows a clear error.
- **G-B4** Typical statistics and machine-learning notation works: fractions, sums, integrals, hats, bars, Greek, blackboard bold, matrices, cases, aligned equations.
- **G-B5** Rendering is fast and cached. Scrolling stays smooth on notes with 200+ formulas.
- **G-B6** One bundled math font. No dependency on system fonts or an installed LaTeX.
- **G-B7** The Markdown emphasis bug on math content is fixed.

### 2.2 Non-goals (do not build these now)

- Rendering Markdown tables as real tables.
- Image / PDF embeds (`![[file.png]]`).
- Transclusion (`{{embed ...}}`), block references, or an outliner mode.
- A full TeX engine. No `\input`, `\include`, `\write`, `\catcode`, or arbitrary TeX programming.
- Exporting rendered math to HTML or PDF.
- MathML output, screen-reader math speech, or Braille.
- Editing math through a visual formula editor.
- Changing `graph.c` layout, theme palette, or the shell layout.
- Changing what agents may write into notes (agent rules stay exactly as they are).

---

## 3. Working agreements for the agent

1. **Recon first.** Phase 0 is read-only. Do not write production code before I approve the plan.
2. **Measure, do not guess.** Any claim about speed, correctness, or coverage needs a number or a test.
3. **Never write to my real vault.** `life2` (about 310 notes) is read-only for every test and self-test. Use fixtures under `tests/fixtures/` and temp copies.
4. **Commits:** one commit per sub-item, message format `area: what (phase id)`. Every commit builds and passes tests.
5. **Clean C17.** No leaks (`g_autoptr` / `g_autofree` or paired frees), no new warnings under the current flags, no undefined behavior. Run sanitizers (ASan/UBSan) on the unit tests at least once per phase when the toolchain allows it, and report if it does not.
6. **Threads.** The GTK main thread never blocks on file I/O, DB access, or long layout work. Use `GTask` / `GThreadPool`. Results swap in atomically.
7. **Security.** Note content is **data**, never instructions and never a command. See A.9 and B.17.
8. **Licensing and clean-room.**
   - Implement from public specifications (OpenType MATH table, MathML Core layout notes, TeX-style atom spacing rules, GTK docs).
   - Do not copy code from KaTeX, MathJax, MicroTeX, Logseq, Obsidian, or any other product.
   - Fonts MUST be under an open license that allows redistribution (SIL OFL or similar). Verify the license text from the font's official source and ship it in the package.
9. **Theme.** Dusk tokens only. New tokens are named by role, documented in `docs/project.md`, and checked by `test_theme.c`.
10. **Docs.** Each phase updates `docs/DECISIONS.md` (ADR), `docs/project.md`, and `CHANGELOG.md`.
11. **Packaging.** After each phase: build, test, run `scripts/package-win.ps1`, launch `dist\brainterm\bt.exe`, confirm the build stamp.
12. **No scope creep.** If you see a bug outside this brief, list it in your report. Do not fix it unless it blocks the work.
13. **Respect the guardrails of the product.** Human Lock, atomic writes through `vault.c`, path guard, checkpoint rules, and agent write limits are unchanged.
14. **Ask when blocked.** A blocker is reported at the stop gate with options. Do not invent a workaround that changes product behavior.

---

## 4. Phase map and stop gates

| Phase | Scope | Output | Gate |
|---|---|---|---|
| P0 | Recon + plan + ADRs, no production code | `docs/links-and-math-plan.md`, ADR drafts | **STOP 0** |
| L1 | Link resolver in core + tests + self-test CLI | `linkresolve.c/.h`, tests | — |
| L2 | Click, hover, cursor, tooltip, open targets, navigation history | UI + security layer | — |
| L3 | Broken-link create flow, ambiguity chooser, keyboard, settings, docs | UI polish | **STOP 1** |
| M1 | Math delimiter tokenizer + emphasis bug fix + highlighter context | `mathtok.c/.h`, lang spec | — |
| M2 | LaTeX parser to AST + tests | `mathparse.c/.h` | — |
| M3 | Font bundle + layout engine + metric tests | `mathfont.c`, `mathlayout.c` | — |
| M4 | Cairo renderer + gallery + golden images + preview popover | `mathrender.c`, popover | **STOP 2** |
| M5 | In-place rendering in the editor (spike first), reveal-on-caret, caching | `mathview.c` | — |
| M6 | Errors, macros, copy-as-source, settings, accessibility, evidence | polish + docs | **STOP 3** |

Rules for gates:

- At each STOP, send: what you built, measured numbers, what you skipped, decisions you made, open risks, and a manual test checklist for me.
- Do not start the next phase until I say go.
- If a phase is impossible as specified (for example an API is missing), stop early and report with options.

---

# PART A — CLICKABLE LINKS

## A.1 Link kinds in scope

| Kind | Example | Click result |
|---|---|---|
| Wikilink to note | `[[chapter-01-descriptive/01-data-types-collection.md]]` | Open note in the editor |
| Wikilink by title | `[[Bootstrap]]` | Resolve by filename or title, then open |
| Wikilink with alias | `[[02-bootstrap\|Bootstrap notes]]` | Open target, display alias if live-preview shows alias |
| Wikilink with heading | `[[02-bootstrap#Core Concepts]]` | Open note, scroll to the heading |
| Wikilink to same note | `[[#Core Concepts]]` | Scroll within the current note |
| Wikilink to file | `[[assets/paper.pdf]]` | Open with default app (A.9 rules) |
| Markdown link to note | `[text](projects/conpty.md)` | Open note |
| Markdown link with anchor | `[text](02-bootstrap.md#overview)` | Open note, scroll to anchor |
| Markdown link, spaces | `[t](my%20note.md)` | Percent-decode, then resolve |
| URL link | `[site](https://example.org)` | Default browser |
| Bare URL | `https://example.org/page` | Default browser |
| mailto | `[me](mailto:a@b.c)` | Default mail app |
| Frontmatter link | `links: ["[[welcome]]"]` | Same as wikilink |

Out of scope: image embeds, `file:` URLs, custom protocol handlers.

## A.2 Syntax and parsing rules

- **Wikilink grammar:** `[[` target (`|` alias)? `]]`
  - The target cannot contain `[`, `]`, or a newline.
  - Maximum target length: 1024 bytes. Longer is not a link.
  - Empty target `[[]]` is not a link.
  - `[[a]]b]]` ends at the first `]]`.
- **Escaped pipe:** inside table rows people write `[[note\|alias]]`. Treat `\|` as an alias separator **and** strip the backslash from the target/alias view.
- **Table pipes:** in a Markdown table row, a plain `|` outside `[[ ]]` is a cell separator. A `|` inside `[[ ]]` is an alias separator. Do not let a table pipe split a wikilink and do not let a wikilink swallow a cell boundary.
- **Anchors:** after the target, `#heading` selects a heading. A `^block-id` suffix is parsed and kept, but block navigation is a non-goal: open the note and ignore the block part.
- **Where links are recognized:** body text, list items, table cells, headings, blockquotes, frontmatter values.
- **Where links are NOT recognized:**
  - fenced code blocks and inline code spans
  - math spans (`$...$`, `$$...$$`) — once Part B exists
  - HTML comments
  - the target of a Markdown link (no nesting)
- **Markdown links:** `[text](dest "title")` with balanced parentheses in `dest`. `<dest with spaces>` form is accepted.
- **Bare URLs:** `http://` or `https://` up to whitespace, with trailing `.,;:!?)` trimmed. Balanced `)` stays.
- **Incremental updates:** on every buffer change, re-scan only the affected paragraph(s) (plus one neighbor for fences). Full-buffer rescans on every keystroke are not allowed.
- **Data model in the UI:** a sorted array of `BtLinkSpan`, looked up by binary search on byte offset. Do not store link data inside `GtkTextTag` objects.

## A.3 Resolver specification

Inputs: raw target `T`, source note path `S` (vault-relative), an index snapshot (file list with titles and aliases).

Steps:

1. Split alias at the first unescaped `|` (or `\|`).
2. Split anchor at the first `#` (heading) and at `^` (block id).
3. Trim whitespace. If the target is empty and an anchor exists, the link points to the current note (`SAME_NOTE`).
4. Normalize: `\` to `/`, collapse `//`, strip leading `./`, resolve `..` lexically. If the result leaves the vault root, status is `BLOCKED` with reason `ESCAPES_VAULT`.
5. Compare with Unicode NFC and case-folding per platform (Windows: case-insensitive; Linux: case-sensitive). Keep the original spelling for display.
6. Generate candidates and stop at the first stage that finds at least one:
   1. exact vault-relative path
   2. same path with `.md` appended when there is no extension
   3. path relative to the directory of `S`
   4. basename match (with and without extension) anywhere in the vault
   5. frontmatter `title` match, then `aliases` match (case-insensitive)
7. Outcome:
   - 0 candidates: `UNRESOLVED` (this is a ghost link)
   - 1 candidate: `RESOLVED`
   - 2 or more: `AMBIGUOUS`, ranked: same directory as `S`, then fewer path segments, then lexical order
8. If the resolved file is not a note (extension not in the note set `.md`, `.markdown`), `kind = FILE`.
9. A directory match gives `kind = DIR`.
10. Schemes: `http`, `https`, `mailto` give `EXTERNAL`. Any other scheme gives `BLOCKED` with reason `SCHEME_NOT_ALLOWED`.
11. Windows rejects (status `BLOCKED`, reason `UNSAFE_PATH`):
    - UNC paths (`\\server\share`), `\\?\` and `\\.\` prefixes
    - drive-letter absolute paths (`C:\...`) that are not inside the vault
    - alternate data streams (`name.txt:stream`)
    - reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`..`COM9`, `LPT1`..`LPT9`), with or without extension
    - trailing dots or spaces in a segment
    - embedded NUL or control characters
12. Symlinks and junctions are followed only through the existing `vault.c` path guard. If the real path leaves the vault, `BLOCKED` with reason `ESCAPES_VAULT`.
13. The resolver is **pure and deterministic**: same inputs give same outputs. It never touches the disk except through the index snapshot, and the final existence check goes through `platform.h`.

## A.4 Resolver API (suggested shape; adapt to repo style)

```c
typedef enum {
  BT_LINK_RESOLVED, BT_LINK_AMBIGUOUS, BT_LINK_UNRESOLVED,
  BT_LINK_EXTERNAL, BT_LINK_BLOCKED
} BtLinkStatus;

typedef enum {
  BT_LINK_KIND_NOTE, BT_LINK_KIND_FILE, BT_LINK_KIND_DIR,
  BT_LINK_KIND_URL,  BT_LINK_KIND_SAME_NOTE
} BtLinkKind;

typedef enum {
  BT_LINK_BLOCK_NONE, BT_LINK_BLOCK_ESCAPES_VAULT,
  BT_LINK_BLOCK_UNSAFE_PATH, BT_LINK_BLOCK_SCHEME_NOT_ALLOWED,
  BT_LINK_BLOCK_TOO_LONG
} BtLinkBlockReason;

typedef struct {
  BtLinkStatus status;
  BtLinkKind kind;
  BtLinkBlockReason block_reason;
  char  *path;          /* vault-relative, forward slashes, original case; NULL if none */
  char **candidates;    /* NULL-terminated, ranked, for AMBIGUOUS */
  char  *anchor;        /* heading text without '#', or NULL */
  char  *block_id;      /* without '^', or NULL */
  char  *display;       /* alias if present, else target text */
  char  *url;           /* for EXTERNAL */
} BtLinkResolution;

BtLinkResolution *bt_link_resolve(const BtIndexSnapshot *idx,
                                  const char *source_note,
                                  const char *raw_target);
void bt_link_resolution_free(BtLinkResolution *r);
```

- Put it in `src/linkresolve.c/.h` inside `libbt_core`. No GTK includes.
- `BtIndexSnapshot` is an immutable view (hash maps for path, basename, title, alias). Build it on the index thread and publish it atomically.
- Rebuilding the snapshot on every note create/rename/delete is acceptable. It must stay under 20 ms for 3,000 notes.

## A.5 Index and graph integration

- The graph's ghost nodes MUST come from this same resolver. After this work, a link is a ghost in the graph **if and only if** it is `UNRESOLVED` in the editor.
- The index stores, per link: source note, raw target, resolved path (nullable), status, line number. A migration `000N_links_resolution.sql` adds the columns; do not edit old migrations.
- Re-resolve affected links when a note is created, renamed, or deleted (a link that was a ghost may become resolved).
- Provide a core function `bt_link_find_backlinks(path)` only if it already exists in the plan from earlier work; otherwise skip it. Do not start the backlinks feature here.

## A.6 Click handling

- Attach a `GtkGestureClick` (button 1) and a `GtkEventControllerMotion` to the editor view. Verify behavior against the installed GTK 4 version.
- A press-release counts as a **click on a link** only if all are true:
  - exactly one press (`n_press == 1`)
  - press and release happen within the GTK drag threshold of each other
  - there is no selection (the buffer has no non-empty selection)
  - the pointer is over a link span
  - Alt is **not** held
- The caret-inside rule: if the caret is already inside that same link span, a click only moves the caret and does **not** open the link. This lets me edit link text (like in my screenshot, where the caret sits in a link).
- Double and triple clicks never open a link (they select a word / line).
- Modifier behavior:
  - `Click` opens (default mode)
  - `Alt+Click` places the caret without opening
  - `Ctrl+Click` behaves like `Click` in default mode, and is **required** when the setting is `ctrl_click`
  - Middle click opens, same as click
- Setting `links.open_on_click`: `click` (default) or `ctrl_click`. Persist in `state.ini` under `[editor]`.
- After a click opens a link, the click is consumed so it does not also move the caret in the old note.
- Unsaved changes: flush pending edits to disk through `vault.c` (atomic write) **before** switching notes. If the flush fails, do not navigate, and show a one-line error in the status bar.
- Opening a note goes through the existing "open note" function in `app.c` (the review mentioned `bt_app_open_note`). Do not add a second open path.
- On open with an anchor: scroll the heading to about one third of the view height and flash a subtle highlight for 600 ms. With Reduce motion on, no flash, only the scroll.

## A.7 Hover, cursor, tooltip, visual style

- Hovering a link: the mouse cursor becomes `pointer`. Moving off restores the text cursor. In `ctrl_click` mode, the pointer shows only while Ctrl is held.
- Hover style: underline stays, color shifts to the hover token (below). Transition 120 ms (skip with Reduce motion).
- Tooltip after the GTK default delay:
  - resolved note: vault-relative path (and heading if any)
  - resolved file: path plus "Opens in default app"
  - external: the full URL
  - ambiguous: "N notes match. Click to choose."
  - unresolved: "Note not found. Click to create."
  - blocked: the reason in plain words
- Tokens (reuse the existing palette; add only what is missing):

| Token | Role | Value guidance |
|---|---|---|
| `@bt_link` | normal link | existing accent family (current purple) |
| `@bt_link_hover` | hover | accent lighter, same family as `bt_graph_edge_active` |
| `@bt_link_broken` | unresolved | warning `#FBBF24` at about 70% |
| `@bt_link_blocked` | blocked | danger `#F87171` at about 70% |

- Broken links keep an underline but use the broken token, so they are visible at a glance.
- Do not rely on color alone: broken links also get a different underline style where Pango allows it, or a tiny glyph suffix in the tooltip only. Report what you chose.
- Links produced inside `## AI draft` sections look the same, and their tooltip adds "AI draft".

## A.8 Open targets

| Resolution | Action |
|---|---|
| `NOTE` | Open in the editor (same pane) |
| `SAME_NOTE` | Scroll to the anchor |
| `FILE` | Open with the default app, after the A.9 checks |
| `DIR` | Reveal in Windows Explorer / file manager, after a confirm popover |
| `EXTERNAL` http/https | Default browser |
| `EXTERNAL` mailto | Default mail client |
| `UNRESOLVED` | Popover "Create note?" (A.11) |
| `AMBIGUOUS` | Chooser popover (A.12) |
| `BLOCKED` | Do nothing except a status-bar message with the reason |

Implementation notes:

- Prefer `GtkFileLauncher` / `GtkUriLauncher` when the installed GTK is 4.10 or newer; otherwise `g_app_info_launch_default_for_uri()`. Verify what works on Windows with the MSYS2 build and report.
- Launch asynchronously. Never block the UI thread. Show a status-bar message "Opening ..." and clear it on completion or error.
- Errors from the OS (no associated app, access denied) show in the status bar, not in a modal dialog.

## A.9 Security rules for opening links

Notes can be written by agents or imported from the internet. Treat every link as untrusted input.

- **Never execute programs.** The following extensions are always blocked from "open with default app", case-insensitive, including after trailing-dot or space tricks:
  `exe com bat cmd ps1 psm1 psd1 vbs vbe js jse wsf wsh msi msp scr pif lnk url reg hta cpl msc jar dll sys inf appx msix ps1xml gadget application`
- Only **allow-listed** extensions open without a prompt: `pdf png jpg jpeg gif webp svg txt csv tsv json md markdown mp3 wav ogg mp4 webm`.
- **Any other extension** shows a confirm popover with the full path and an "Open anyway" button. "Always allow this extension" is not offered.
- The resolved real path MUST be inside the vault (via the existing path guard, including symlink / junction escape checks). Otherwise: blocked.
- External URLs:
  - allowed schemes: `http`, `https`, `mailto`
  - `javascript:`, `data:`, `file:`, `vbscript:`, `ms-*:`, `search-ms:`, and any custom scheme are blocked
  - the first external URL opened in a session shows a one-time confirm popover showing the full URL; I can tick "Don't ask again this session"
  - URLs longer than 2048 bytes are blocked
  - strip userinfo (`user:pass@`) from displayed URLs and warn if present
- Never pass link text through a shell. Use API calls only, no `system()`, no `cmd /c`, no `Start-Process` string building.
- Never log full URLs or paths to files outside the vault log. No telemetry.
- A link click is a **user action**. Opening a link never triggers an agent action and never writes to the vault, except the explicit "Create note" popover (A.11).
- Add a unit test list for every rule in this section (A.16, group U).

## A.10 Navigation history

- Keep a back / forward stack of `(note path, caret offset, scroll position)`.
- Push an entry when a link opens a different note. Do not push for same-note scrolls shorter than one screen.
- Shortcuts: `Alt+Left` back, `Alt+Right` forward, and mouse back/forward buttons.
- Add back / forward buttons only if there is an obvious place in the header or editor toolbar that fits the Dusk style. Otherwise shortcuts only.
- Maximum stack size 100. Entries for deleted notes are skipped silently.
- History is per session, not persisted.
- Opening from the sidebar or Ctrl+P also pushes an entry, for consistency.

## A.11 Broken link: create-note flow

- Click on an `UNRESOLVED` note link opens a small popover anchored to the link:
  - title: `Create "<name>"?`
  - shows the exact vault-relative path that will be created
  - buttons: `Create` and `Cancel`
- Path rule: if the target contains folders, create them (through `vault.c`, path guard on every segment). If it has no folder, create next to the current note.
- Add `.md` when missing. Reject names with characters illegal on Windows (`< > : " / \ | ? *`) except the folder separator, and show why.
- New note content follows the existing new-note template, including the frontmatter fields I already use (`id`, `title`, `created`, `updated`, `source: human`, `agent: null`, `tags`, `tags_ai`, `links`).
- Creation is atomic (temp file then rename). After creation, update the index, re-resolve links, and open the new note.
- Creating a note never modifies the note that contained the link.
- Cancel leaves everything untouched.

## A.12 Ambiguity chooser

- For `AMBIGUOUS`, open a popover listing the candidates:
  - each row shows filename and the folder path in muted text
  - the best-ranked candidate is preselected
  - `Enter` opens the selected row, `Esc` cancels, arrows move
- Choosing does **not** rewrite the link in the file. It opens the note only.
- Offer a second button `Copy full path link` that puts `[[folder/file]]` on the clipboard so I can disambiguate manually.

## A.13 Keyboard

- `Ctrl+Enter` with the caret inside a link opens it, in both click modes.
- `Alt+Left` / `Alt+Right` navigate history.
- `Ctrl+Alt+Right` jumps the caret to the next link in the note, `Ctrl+Alt+Left` to the previous one.
- Links are reachable without a mouse. Document these keys in the shortcuts list if the app has one.

## A.14 Settings and menu

- App menu entries: `Open links with single click` (toggle). Persist in `state.ini`.
- Optional second toggle: `Confirm external URLs` (default on).
- No other settings. Do not add a settings window.

## A.15 Self-test CLI for links

Add `bt.exe --selftest-links <vault_dir>` (read-only, headless, no GTK window):

- scans every note, extracts every link with the same parser as the editor, resolves it with the same resolver
- prints: total links by kind, counts of `RESOLVED / AMBIGUOUS / UNRESOLVED / EXTERNAL / BLOCKED`
- prints the first 50 unresolved and all ambiguous links as `file:line target`
- prints timing (total ms, ms per 1,000 links)
- exit code 0 if it completed (unresolved links are not an error), nonzero on internal error
- I will run it on `life2`. Include its real output in your phase report.

## A.16 Tests for Part A

### Group R — resolver table tests (CMocka, table-driven)

Fixture vault listing used by the tests:

```
welcome.md
projects/conpty.md
projects/notes/conpty.md
statistical-statistics-notes/MathStats.md
statistical-statistics-notes/chapter-01-descriptive/01-data-types-collection.md
statistical-statistics-notes/chapter-13-empirical/02-bootstrap.md
Daily/2026-11-23.md
Résumé.md                  (stored NFC; test also with NFD input)
assets/diagram.png
assets/paper.pdf
scripts/run.ps1
```

| # | Source | Raw target | Expected |
|---|---|---|---|
| R01 | welcome.md | `welcome` | RESOLVED welcome.md |
| R02 | welcome.md | `Welcome` | RESOLVED on Windows, UNRESOLVED on Linux |
| R03 | welcome.md | `welcome.md` | RESOLVED |
| R04 | MathStats.md | `chapter-01-descriptive/01-data-types-collection.md` | RESOLVED via relative-to-source |
| R05 | welcome.md | `statistical-statistics-notes/chapter-13-empirical/02-bootstrap` | RESOLVED (adds .md) |
| R06 | welcome.md | `02-bootstrap` | RESOLVED (basename, unique) |
| R07 | welcome.md | `conpty` | AMBIGUOUS, candidates ranked |
| R08 | projects/conpty.md | `conpty` | AMBIGUOUS, same-dir candidate first |
| R09 | welcome.md | `projects/conpty` | RESOLVED exact |
| R10 | welcome.md | `missing-note` | UNRESOLVED |
| R11 | welcome.md | `02-bootstrap#Overview` | RESOLVED, anchor `Overview` |
| R12 | 02-bootstrap.md | `#Core Concepts` | SAME_NOTE, anchor |
| R13 | welcome.md | `02-bootstrap\|Bootstrap` | RESOLVED, display `Bootstrap` |
| R14 | welcome.md | `02-bootstrap\\|Bootstrap` (escaped pipe) | RESOLVED, display `Bootstrap` |
| R15 | welcome.md | `02-bootstrap^abc123` | RESOLVED, block_id `abc123` |
| R16 | welcome.md | `../outside` | BLOCKED ESCAPES_VAULT |
| R17 | projects/conpty.md | `../welcome` | RESOLVED |
| R18 | welcome.md | `assets/paper.pdf` | RESOLVED kind FILE |
| R19 | welcome.md | `scripts/run.ps1` | RESOLVED kind FILE (open is blocked later by A.9) |
| R20 | welcome.md | `assets` | RESOLVED kind DIR |
| R21 | welcome.md | `Résumé` (NFD input) | RESOLVED to NFC file |
| R22 | welcome.md | `C:\Windows\system32\calc.exe` | BLOCKED UNSAFE_PATH |
| R23 | welcome.md | `\\server\share\a.md` | BLOCKED UNSAFE_PATH |
| R24 | welcome.md | `a.md:stream` | BLOCKED UNSAFE_PATH |
| R25 | welcome.md | `NUL` | BLOCKED UNSAFE_PATH |
| R26 | welcome.md | `name. ` (trailing dot/space) | BLOCKED UNSAFE_PATH |
| R27 | welcome.md | `https://example.org/x` | EXTERNAL |
| R28 | welcome.md | `javascript:alert(1)` | BLOCKED SCHEME_NOT_ALLOWED |
| R29 | welcome.md | `file:///C:/x` | BLOCKED SCHEME_NOT_ALLOWED |
| R30 | welcome.md | `mailto:a@b.c` | EXTERNAL |
| R31 | welcome.md | (empty) | not a link |
| R32 | welcome.md | 1,100-byte target | BLOCKED TOO_LONG |
| R33 | welcome.md | `my%20note.md` from a Markdown link | percent-decoded, then resolved |
| R34 | welcome.md | `projects\conpty.md` | RESOLVED (backslash normalized) |
| R35 | welcome.md | `projects//conpty.md` | RESOLVED (collapsed) |
| R36 | welcome.md | title match, note has `title: "ConPTY Notes"` | RESOLVED by title |
| R37 | welcome.md | alias match from frontmatter `aliases` | RESOLVED by alias |
| R38 | symlink to outside the vault | any | BLOCKED ESCAPES_VAULT |

### Group P — parser tests

- wikilink inside a table cell with plain `|` cell separators on both sides
- `[[a\|b]]` in a table cell
- two links on one line, adjacent
- link inside a list item, a heading, a blockquote
- link in frontmatter YAML string
- link inside fenced code: not a link
- link inside inline code: not a link
- nested brackets `[[a [b] c]]`: not a link
- Markdown link with balanced parentheses in the URL
- bare URL with trailing punctuation
- Unicode (emoji, CJK, RTL) inside the target
- CRLF line endings
- 10,000 links in one note: parse under 50 ms
- incremental rescan equals full rescan after random edits (property test with a fixed seed)

### Group U — security tests

- every blocked extension, with mixed case, trailing dot, trailing space, and double extension (`a.pdf.exe`)
- allow-listed extension opens without prompt (mock the launcher)
- unknown extension requires confirm
- URL scheme allow-list and block-list
- URL with userinfo is flagged
- link that escapes through a symlink or junction is blocked
- launcher is never called for a blocked link (mock and assert zero calls)

### Group I — UI interaction tests (headless where possible, otherwise a scripted manual checklist)

- click opens, Alt+Click does not
- click inside the caret's own link does not open
- drag-select across a link does not open
- double click does not open
- `ctrl_click` mode requires Ctrl
- unsaved edits are flushed before navigation, and a failed flush blocks navigation
- back / forward restore note, caret, and scroll
- ambiguity chooser keyboard flow
- create-note popover creates exactly one file, in the right folder, atomically
- cancel creates nothing
- tooltip text for each status

## A.17 Acceptance criteria for Part A

- [ ] On `life2`, clicking `[[chapter-01-descriptive/01-data-types-collection.md]]` in `MathStats.md` opens that note in BrainTerm.
- [ ] `--selftest-links` runs on `life2` and reports counts; every unresolved link is listed with `file:line`.
- [ ] A link is a graph ghost node if and only if the editor shows it as unresolved (test proves it).
- [ ] All R, P, U tests pass. Sanitizer run is clean or reported.
- [ ] No blocked-extension file can be launched by any click path (test proves it).
- [ ] Back / forward work across at least 5 consecutive jumps.
- [ ] No new warnings, no per-keystroke full-buffer rescans, UI never blocks (frame-time check while clicking links in a large note).
- [ ] Packaged exe launched, build stamp matches, short manual checklist passed.
- [ ] Docs, ADRs, CHANGELOG updated.

---

# PART B — LaTeX / MATH RENDERING

## B.1 Requirements derived from the screenshot

| ID | Requirement |
|---|---|
| B-REQ-01 | `$ ... $` renders as inline math, baseline-aligned with the surrounding text. |
| B-REQ-02 | `$$ ... $$` renders as display math: centered, larger operators, own vertical space. |
| B-REQ-03 | Display math may span multiple lines. |
| B-REQ-04 | Math inside ordered lists, nested bullets, blockquotes, headings, and table cells renders correctly. |
| B-REQ-05 | The caret or a selection touching a formula reveals the LaTeX source. Moving away re-renders. |
| B-REQ-06 | Rendering never changes the saved file. The buffer text on disk equals what I typed. |
| B-REQ-07 | `*` and `_` inside math never become Markdown emphasis. |
| B-REQ-08 | Invalid LaTeX shows the source with an error style and a tooltip message. No crash. |
| B-REQ-09 | Copying text that contains rendered math copies the LaTeX source. |
| B-REQ-10 | Undo / redo behave exactly as without rendering. |
| B-REQ-11 | Typical stats / ML notation works (see B.6 Tier 1 and Tier 2). |
| B-REQ-12 | Math color follows the theme and updates on theme change. |
| B-REQ-13 | HiDPI is crisp (render at device pixels, not scaled bitmaps). |
| B-REQ-14 | A note with 300 formulas scrolls at 60 FPS on my machine (see B.19). |
| B-REQ-15 | Works offline, in a single packaged folder, with no installed LaTeX. |

## B.2 Delimiters and tokenization rules

Math spans are found by a dedicated tokenizer **before** any Markdown inline processing.

### B.2.1 Recognized forms

| Form | Mode | Notes |
|---|---|---|
| `$...$` | inline | pandoc-style rules below |
| `$$...$$` | display | may span lines |
| `\(...\)` | inline | optional, Tier 2 |
| `\[...\]` | display | optional, Tier 2 |
| `\begin{equation}...\end{equation}` | display | also `equation*`, `align`, `align*`, `gather`, `gather*` at top level |

### B.2.2 Inline `$` rules (to avoid treating money as math)

- The opening `$` MUST be followed immediately by a non-space character.
- The closing `$` MUST be preceded immediately by a non-space character.
- The closing `$` MUST NOT be followed immediately by an ASCII digit.
- `\$` is a literal dollar sign and never opens or closes math.
- `$$` is never read as an empty inline span. It starts display math.
- Inline math does not cross a blank line. It MAY cross a single newline when it is inside one paragraph, but the tokenizer MUST stop at the end of the paragraph.
- If no valid closer is found, the opener is plain text.
- `$5 and $10` is **not** math. `$x$ and $y$` is two math spans.

### B.2.3 Display `$$` rules

- Opens at `$$` and closes at the next unescaped `$$`.
- May span multiple lines, up to 200 lines or 16 KB, whichever comes first. Beyond that, treat as text and mark with a warning in the self-test.
- May start after a list marker or blockquote prefix (`> `, `- `, `1. `). The prefix is not part of the math. Strip leading `> ` quote markers from continuation lines inside a blockquote.
- Display math written inline in a paragraph (text before or after the `$$ ... $$`) renders as a display block and splits the visual line. Report in the ADR how you handle it.

### B.2.4 Where math is NOT recognized

- fenced code blocks, indented code blocks, inline code spans
- YAML frontmatter
- the inside of wikilinks and Markdown link destinations
- URLs
- HTML comments

### B.2.5 Precedence and the emphasis bug

- Order of recognition inside a paragraph: code spans, then math spans, then wikilinks / Markdown links, then emphasis and the rest.
- Once a range is claimed as math, **no other rule may style or tag characters inside it** (no italic, no bold, no strikethrough, no link, no list-marker handling).
- Fix this in both places where emphasis can come from:
  - the GtkSourceView language spec `data/language-specs/bt-markdown.lang` — add `bt:math-inline` and `bt:math-display` contexts placed **before** emphasis contexts, with `extend-parent` off and no child contexts
  - the live-preview tag pass in `editorpane.c` / `mdparse.c` — skip math ranges when applying emphasis and link tags
- Add a regression test that reproduces my screenshot: the line `$$(\hat{\theta}^*_{(\alpha/2)}, \hat{\theta}^*_{(1-\alpha/2)})$$` MUST get zero emphasis tags.
- Add a second regression test for `**bold** and $a*b*c$ and *italic*` (bold and italic outside math remain, none inside).

### B.2.6 Tokenizer API (core, GTK-free)

```c
typedef enum { BT_MATH_INLINE, BT_MATH_DISPLAY } BtMathMode;

typedef struct {
  gsize start, end;          /* byte offsets of the full span including delimiters */
  gsize src_start, src_end;  /* byte offsets of the LaTeX source only */
  BtMathMode mode;
  guint32 hash;              /* hash of normalized source + mode, for caching */
  guint line_first, line_last;
} BtMathSpan;

/* Scan a UTF-8 buffer. Spans are sorted, non-overlapping. */
GArray *bt_math_scan(const char *text, gsize len, const BtMathScanOpts *opts);
```

- Handles CRLF, tabs, and a UTF-8 BOM at the start.
- Never reads past `len`. Fuzz it (B.21).
- Incremental use: `bt_math_scan_range()` rescans a paragraph range and returns replacements. Full-buffer scanning on every keystroke is not allowed.

## B.3 Engine decision (write ADR before coding)

Evaluate these options in the ADR, with a short table and a decision:

| Option | Summary | Main risks |
|---|---|---|
| 1. Own engine in C on HarfBuzz OpenType MATH | Parse a LaTeX subset, lay out with MATH-table constants, draw glyphs with Cairo | Effort; must restrict to a subset |
| 2. MicroTeX | Existing C++ LaTeX renderer with a Cairo backend | C++ toolchain on MSYS2, license check, build size, subset gaps |
| 3. KaTeX / MathJax in a WebView | Full fidelity | Heavy dependency, startup time, not a native widget |
| 4. External LaTeX (tectonic / pdflatex) | Real LaTeX | Installation burden, latency, offline packaging |
| 5. Unicode substitution only | Replace `\alpha` with α etc. | No fractions, sums, matrices; not "real math" |

Decision guidance:

- My default expectation is **Option 1** because it keeps a single native exe, works offline, and fits C + GTK + Cairo.
- Before committing, run a **time-boxed spike** (target one day of work) that proves:
  - `hb-ot-math.h` is available in the installed HarfBuzz (version printed in the report)
  - the bundled math font loads and `hb_ot_math_has_data()` is true
  - you can draw `\frac{a+b}{c}` with correct shift-ups from MATH constants onto a Cairo surface
- If the spike fails or Option 1 is clearly unreasonable, stop at STOP 0 with the evidence and recommend Option 2. Do not silently switch.
- Verify licenses from primary sources for any library you consider. Record the exact version and license in the ADR.

## B.4 Math font

- Bundle one OpenType font that has a **MATH table**. Candidate: STIX Two Math (SIL OFL). Alternatives: Libertinus Math (OFL), Latin Modern Math (GUST Font License). Verify the license text from the official distribution and ship it as `data/fonts/LICENSE-<font>.txt`.
- Embed via GResource and load from memory. Do not depend on system fonts.
- Show the font name and license in the About dialog or a credits file.
- On load, verify with `hb_ot_math_has_data()`. If missing, fail loudly at startup self-check and fall back to "raw source with a notice". Never crash.
- Missing glyph policy: draw the `.notdef` box in the error color and flag the expression as having a warning. Report glyph coverage of the Tier 1 and Tier 2 command lists in the evidence folder.
- `\text{...}` uses the math font's upright Latin letters. Non-Latin text in `\text` is a known limitation; report it in docs.

## B.5 Modules and files

Create a separate static library `libbt_math` that depends on **HarfBuzz only** (no GTK, no Cairo). Drawing happens in the UI layer.

| File | Role |
|---|---|
| `src/math/mathtok.c/.h` | Delimiter scanner (B.2) |
| `src/math/mathparse.c/.h` | LaTeX subset to AST (B.7, B.8) |
| `src/math/mathmacros.c/.h` | Optional per-vault macros (B.16) |
| `src/math/mathfont.c/.h` | Font blob, `hb_font_t`, MATH constants cache |
| `src/math/mathlayout.c/.h` | AST to box tree to draw list (B.9, B.10) |
| `src/math/meson.build` | `libbt_math` target |
| `src/ui/mathrender.c/.h` | Replays a draw list with Cairo; texture cache |
| `src/ui/mathview.c/.h` | Editor integration (B.12 to B.14) |
| `tests/unit/test_math_tok.c` | tokenizer tests |
| `tests/unit/test_math_parse.c` | parser tests |
| `tests/unit/test_math_layout.c` | metric tests |
| `tests/unit/test_math_fuzz.c` | fuzz harness |
| `tests/bench/bench_math.c` | performance |
| `tests/golden/math/` | golden PNGs + manifest |
| `vault-template/examples/math-gallery.md` | gallery note used by captures |
| `docs/math.md` | user and developer documentation |

- Register everything in Meson. `bt.exe` and `bt-mcp.exe` both MAY link `libbt_math`; `bt-mcp.exe` does not need rendering.
- Font loading and Cairo drawing live in the UI layer. The layout library only needs glyph IDs, advances, and extents from HarfBuzz.

## B.6 Supported LaTeX subset

Priority tiers: **Tier 1** = must ship in M3/M4. **Tier 2** = ship in M5/M6 if time allows, else report. **Tier 3** = optional.

### B.6.1 Structure and layout

| Construct | Tier |
|---|---|
| Groups `{...}` | 1 |
| Superscript `^`, subscript `_`, both together, nested | 1 |
| Primes `'` and `''` | 1 |
| `\frac{a}{b}`, `\dfrac`, `\tfrac` | 1 |
| `\binom{n}{k}`, `\dbinom`, `\tbinom` | 1 |
| `\sqrt{x}`, `\sqrt[n]{x}` | 1 |
| `\left ... \right` with stretchy delimiters, `\middle` | 1 |
| `\big \Big \bigg \Bigg` (and `l`, `r`, `m` variants) | 2 |
| `\displaystyle \textstyle \scriptstyle \scriptscriptstyle` | 2 |
| `\limits`, `\nolimits` | 1 |
| `\substack{a \\ b}` | 2 |
| `\overset{a}{b}`, `\underset{a}{b}`, `\stackrel{a}{b}` | 2 |
| `\xrightarrow[below]{above}`, `\xleftarrow` | 2 |
| `\overbrace{x}^{y}`, `\underbrace{x}_{y}` | 2 |
| `\cfrac` | 3 |
| `\phantom`, `\hphantom`, `\vphantom` | 3 |
| `\boxed{x}` | 3 |
| `\cancel`, `\bcancel`, `\xcancel` | 3 |

### B.6.2 Spacing

| Command | Meaning | Tier |
|---|---|---|
| `\,` | thin space (3/18 em) | 1 |
| `\:` or `\>` | medium space (4/18 em) | 1 |
| `\;` | thick space (5/18 em) | 1 |
| `\!` | negative thin space | 1 |
| `\ ` (backslash space) | normal space | 1 |
| `\quad`, `\qquad` | 1 em, 2 em | 1 |
| `\hspace{len}` | fixed space, units `em`, `pt` only | 3 |
| `\mathbin \mathrel \mathord \mathop \mathopen \mathclose \mathpunct` | force atom class | 2 |

### B.6.3 Fonts and text

| Command | Tier |
|---|---|
| `\mathrm \mathbf \mathit \mathsf \mathtt` | 1 |
| `\mathbb` (R, N, Z, Q, C, E, P, 1) | 1 |
| `\mathcal` | 1 |
| `\mathscr`, `\mathfrak` | 2 |
| `\boldsymbol`, `\bm` | 2 |
| `\text{...}`, `\textrm`, `\textbf`, `\textit` | 1 |
| `\operatorname{name}`, `\operatorname*{name}` | 1 |
| `\mathnormal` | 3 |

### B.6.4 Accents and decorations

| Command | Tier |
|---|---|
| `\hat`, `\widehat` | 1 |
| `\bar`, `\overline`, `\underline` | 1 |
| `\tilde`, `\widetilde` | 1 |
| `\vec`, `\overrightarrow`, `\overleftarrow` | 1 |
| `\dot`, `\ddot` | 1 |
| `\check`, `\breve`, `\acute`, `\grave`, `\mathring` | 3 |

### B.6.5 Greek letters

| Group | Commands | Tier |
|---|---|---|
| Lowercase | `\alpha \beta \gamma \delta \epsilon \varepsilon \zeta \eta \theta \vartheta \iota \kappa \lambda \mu \nu \xi \pi \varpi \rho \varrho \sigma \varsigma \tau \upsilon \phi \varphi \chi \psi \omega` | 1 |
| Uppercase | `\Gamma \Delta \Theta \Lambda \Xi \Pi \Sigma \Upsilon \Phi \Psi \Omega` | 1 |

### B.6.6 Operators and relations

| Group | Commands | Tier |
|---|---|---|
| Binary | `+ - \pm \mp \times \div \cdot \ast \star \circ \bullet \cup \cap \setminus \oplus \otimes \wedge \vee \land \lor` | 1 |
| Relations | `= \ne \neq < > \le \leq \ge \geq \ll \gg \approx \sim \simeq \cong \equiv \propto \in \notin \ni \subset \subseteq \supset \supseteq \perp \parallel \mid \nmid \not` | 1 |
| Arrows | `\to \rightarrow \leftarrow \gets \leftrightarrow \Rightarrow \Leftarrow \Leftrightarrow \mapsto \implies \iff \uparrow \downarrow \longrightarrow \longleftarrow` | 1 |
| Big operators | `\sum \prod \coprod \int \iint \iiint \oint \bigcup \bigcap \bigoplus \bigotimes \bigvee \bigwedge` | 1 |
| Named operators | `\lim \limsup \liminf \max \min \sup \inf \arg \det \dim \exp \log \ln \lg \sin \cos \tan \cot \sec \csc \sinh \cosh \tanh \arcsin \arccos \arctan \Pr \gcd \deg \ker \hom \mod \bmod \pmod` | 1 |
| Misc symbols | `\infty \partial \nabla \forall \exists \nexists \emptyset \varnothing \ell \hbar \Re \Im \aleph \prime \angle \triangle \neg \lnot \top \bot` | 1 |
| Dots | `\ldots \cdots \vdots \ddots \dots \therefore \because` | 1 |
| Escapes | `\% \$ \& \# \_ \{ \} \backslash` | 1 |

### B.6.7 Delimiters

| Delimiter | Source | Tier |
|---|---|---|
| Parentheses, brackets | `( ) [ ]` | 1 |
| Braces | `\{ \} \lbrace \rbrace` | 1 |
| Angle | `\langle \rangle` | 1 |
| Bars | `\| \lvert \rvert \lVert \rVert` and plain `\|` | 1 |
| Floor / ceiling | `\lfloor \rfloor \lceil \rceil` | 1 |
| Slash | `/ \backslash` | 1 |
| Null delimiter | `\left. \right.` | 1 |

### B.6.8 Environments

| Environment | Tier |
|---|---|
| `matrix pmatrix bmatrix Bmatrix vmatrix Vmatrix` | 1 |
| `cases` | 1 |
| `aligned`, `align`, `align*`, `split` (alignment at `&`) | 1 |
| `gather`, `gather*` | 2 |
| `array{lcr}` with optional vertical bars | 2 |
| `smallmatrix` | 3 |
| `equation`, `equation*` (wrapper only) | 1 |

Rules: `&` separates columns, `\\` separates rows, `\hline` in `array` only, `\tag{...}` shows a right-aligned label (Tier 2), `\label`, `\nonumber`, `\notag` are accepted and ignored.

### B.6.9 Stats / ML notation that MUST work (Tier 1 acceptance list)

Each item must parse, lay out, and render in the gallery:

- `\bar{X}_n`, `\hat{\theta}_{MLE}`, `\hat{\beta} = (X^\top X)^{-1} X^\top y`
- `\mathbb{E}[X]`, `\mathbb{P}(A \mid B)`, `\operatorname{Var}(X)`, `\operatorname{Cov}(X,Y)`
- `X \sim \mathcal{N}(\mu, \sigma^2)`, `X_1, \ldots, X_n \overset{\text{iid}}{\sim} F`
- `\sum_{i=1}^{n} (x_i - \bar{x})^2`, `\prod_{i=1}^{n} f(x_i; \theta)`
- `\int_{-\infty}^{\infty} e^{-x^2/2}\, dx`
- `\frac{\partial L}{\partial \theta}`, `\nabla_\theta J(\theta)`
- `\arg\max_{\theta} \log p(x \mid \theta)`, `\operatorname*{arg\,min}_{w}`
- `\sqrt{\frac{\sum_i (x_i-\bar x)^2}{n-1}}`
- `\xrightarrow{d}`, `\xrightarrow{p}`, `\overset{d}{\to}`
- `\begin{pmatrix} a & b \\ c & d \end{pmatrix}`
- `f(x) = \begin{cases} 1 & x \ge 0 \\ 0 & \text{otherwise} \end{cases}`
- `\begin{aligned} y &= ax + b \\ &= \ldots \end{aligned}`
- `X \perp\!\!\!\perp Y`
- `\theta^{(t)}`, `\hat{\theta}^{*(b)}`, `\hat{\theta}^*_{(\alpha/2)}`
- the acceptance ratio `r = \frac{f(\theta^* | x) q(\theta^{(t-1)} | \theta^*)}{f(\theta^{(t-1)} | x) q(\theta^* | \theta^{(t-1)})}`

### B.6.10 Explicitly unsupported (must fail gracefully)

`\input \include \write \read \openin \catcode \def \let \csname \expandafter \newenvironment \usepackage \documentclass \begin{tikzpicture}` and any other TeX programming. Unknown commands render as `\name` in the error style (B.15) and the rest of the formula still renders.

## B.7 Parser grammar (informal)

```
math        := expr
expr        := (item)*
item        := atom script*
script      := ('^' | '_') arg | "'"+
atom        := symbol | command | group | env | delimited | fraction-like
group       := '{' expr '}'
arg         := group | single-token
command     := '\' name ( '*' )? args...      ; arity table-driven
delimited   := '\left' delim expr ( '\middle' delim expr )* '\right' delim
env         := '\begin' '{' name '}' body '\end' '{' name '}'
body        := row ( '\\' row )*
row         := cell ( '&' cell )*
```

Parser rules:

- Table-driven command registry: name, arity (required args, optional first arg), kind (symbol, operator, accent, font switch, layout), atom class, and a replacement code point or glyph name.
- Whitespace in math mode is ignored, except after a control word where it terminates the word.
- `\\` outside an environment is a line break only in display `aligned`-like contexts; otherwise report a warning and ignore.
- Double superscript `x^a^b` is an error with a precise column.
- `{}` empty group is valid and acts as an empty atom (used for spacing and prime tricks).
- Unbalanced braces: error with column and expected closer.
- Mismatched `\left` / `\right`: error with column.
- Mismatched `\begin` / `\end`: error with both names.
- Position tracking: every AST node keeps `src_start/src_end` byte offsets for error highlights.
- The parser MUST be total: for any byte string it returns either an AST or a list of errors plus a best-effort partial AST. It never aborts the process.
- Allocate nodes from an arena. Free the whole tree in one call.

## B.8 AST node types

| Node | Fields |
|---|---|
| `ROW` | children |
| `ATOM` | code point or glyph id, atom class, font style |
| `OPNAME` | name string (upright), limits flag |
| `SCRIPTS` | base, sub, sup, prime count |
| `FRAC` | numerator, denominator, rule thickness (default or none for `\binom`), style override |
| `SQRT` | radicand, optional index |
| `DELIM` | open, close, list of middle delimiters, body, size (auto or fixed) |
| `ACCENT` | base, accent kind, wide flag |
| `OVER` / `UNDER` | base, annotation, brace/arrow/plain |
| `STACKREL` | top, base |
| `XARROW` | arrow kind, above, below |
| `MATRIX` | rows, columns, per-column alignment, delimiter pair |
| `CASES` | rows of (value, condition) |
| `ALIGN` | rows of columns, alignment pairs |
| `TEXT` | UTF-8 string, style |
| `SPACE` | em width (can be negative) |
| `STYLE` | display / text / script / scriptscript |
| `ERROR` | message, column, original source slice |

## B.9 Layout algorithm

Implement layout from public specs: the OpenType MATH table, the TeX atom-spacing model, and MathML Core's layout notes. Do not copy code from other engines.

### B.9.1 Units and sizes

- Lay out in **em units** as floats. Convert to pixels only at render time. The layout result is cached per expression and is independent of font size and DPI.
- Math styles: `D` (display), `T` (text), `S` (script), `SS` (scriptscript), each with a **cramped** variant.
- Script scaling: use `ScriptPercentScaleDown` and `ScriptScriptPercentScaleDown` from the MATH constants.
- Start style: `D` for `$$`, `T` for `$`.

### B.9.2 Box model

```c
typedef struct {
  float width, height, depth;   /* height = above baseline, depth = below */
  float italic_corr;            /* italic correction */
  float accent_attach;          /* top accent attachment x, if any */
  BtMathAtomClass cls;          /* Ord, Op, Bin, Rel, Open, Close, Punct, Inner */
  /* children and draw ops are accumulated into the draw list during layout */
} BtMathBox;
```

### B.9.3 Atom spacing (between adjacent atoms, non-script styles)

Unit: 0 = none, 1 = thin (3/18 em), 2 = medium (4/18 em), 3 = thick (5/18 em). A star means the space applies only in `D` and `T`, never in `S` or `SS`.

| left \ right | Ord | Op | Bin | Rel | Open | Close | Punct | Inner |
|---|---|---|---|---|---|---|---|---|
| **Ord** | 0 | 1 | 2* | 3* | 0 | 0 | 0 | 1* |
| **Op** | 1 | 1 | – | 3* | 0 | 0 | 0 | 1* |
| **Bin** | 2* | 2* | – | – | 2* | – | – | 2* |
| **Rel** | 3* | 3* | – | 0 | 3* | 0 | 0 | 3* |
| **Open** | 0 | 0 | – | 0 | 0 | 0 | 0 | 0 |
| **Close** | 0 | 1 | 2* | 3* | 0 | 0 | 0 | 1* |
| **Punct** | 1* | 1* | – | 1* | 1* | 1* | 1* | 1* |
| **Inner** | 1* | 1 | 2* | 3* | 1* | 0 | 1* | 1* |

Rules:

- A `Bin` atom at the start of a row, after another `Bin`, `Op`, `Rel`, `Open`, or `Punct`, or before a `Close`, `Rel`, or `Punct`, becomes `Ord` (unary minus case).
- Explicit spacing commands add to the table spacing; they do not replace it.
- Cover these rules with metric tests.

### B.9.4 Constants to read from the MATH table

Use `hb_ot_math_get_constant()` (verify names in `hb-ot-math.h`). At minimum:

- `AxisHeight`, `ScriptPercentScaleDown`, `ScriptScriptPercentScaleDown`
- `FractionRuleThickness`, `FractionNumeratorShiftUp`, `FractionNumeratorDisplayStyleShiftUp`
- `FractionDenominatorShiftDown`, `FractionDenominatorDisplayStyleShiftDown`
- `FractionNumeratorGapMin`, `FractionNumDisplayStyleGapMin`, `FractionDenominatorGapMin`, `FractionDenomDisplayStyleGapMin`
- `SuperscriptShiftUp`, `SuperscriptShiftUpCramped`, `SubscriptShiftDown`
- `SuperscriptBaselineDropMax`, `SubscriptBaselineDropMin`
- `SubSuperscriptGapMin`, `SuperscriptBottomMaxWithSubscript`, `SpaceAfterScript`
- `UpperLimitGapMin`, `UpperLimitBaselineRiseMin`, `LowerLimitGapMin`, `LowerLimitBaselineDropMin`
- `RadicalVerticalGap`, `RadicalDisplayStyleVerticalGap`, `RadicalRuleThickness`, `RadicalExtraAscender`, `RadicalKernBeforeDegree`, `RadicalKernAfterDegree`, `RadicalDegreeBottomRaisePercent`
- `AccentBaseHeight`, `FlattenedAccentBaseHeight`
- `DelimitedSubFormulaMinHeight`, `DisplayOperatorMinHeight`
- `OverbarVerticalGap`, `OverbarRuleThickness`, `OverbarExtraAscender`, `UnderbarVerticalGap`, `UnderbarRuleThickness`, `UnderbarExtraDescender`
- `StackTopShiftUp`, `StackBottomShiftDown` (and the display-style variants) for `\binom`-like stacks
- Also use: `hb_ot_math_get_glyph_italics_correction`, `hb_ot_math_get_glyph_top_accent_attachment`, `hb_ot_math_get_glyph_kernings`, `hb_ot_math_get_glyph_variants`, `hb_ot_math_get_glyph_assembly`, `hb_ot_math_get_min_connector_overlap`.

### B.9.5 Construct rules (summary; follow the spec for details)

- **Scripts:** place sup/sub relative to the base using the shift constants and the cramped state; apply italic correction for sup; enforce gap minimums; apply math kerning if the font provides it.
- **Fractions:** numerator and denominator use the next smaller style; center on the rule; rule sits on the axis; use display or text constants depending on style; bar thickness from the font. `\binom` has no bar and uses stack constants with delimiters sized to the stack.
- **Radicals:** pick a glyph variant tall enough for the radicand plus gaps; use glyph assembly when no variant fits; draw the overbar as a rule; place the index with the kern and raise constants.
- **Big operators:** in `D` style use the display-size variant (at least `DisplayOperatorMinHeight`), limits above and below by default for `\sum`, `\prod`, `\lim`-like operators, and to the side for `\int`. `\limits` / `\nolimits` override. In `T` style, limits go to the side.
- **Delimiters:** `\left ... \right` size = max(height above axis, depth below axis) with the `DelimitedSubFormulaMinHeight` rule; use variants and assemblies; center on the axis. `\big` family uses fixed ratios.
- **Accents:** base accent height from the constants; wide accents pick a variant that fits the base width; attachment uses the top accent attachment point of the base.
- **Matrices and cases:** columns are sized to their widest cell, rows to their tallest ascent and deepest descent plus a baseline skip of about 1.2 em and a row gap; column gap 1 em; delimiters stretch to the whole block and center on the axis.
- **Aligned:** pairs of columns `rl`, with the `&` column aligned; inter-pair space 2 em.
- **Text:** upright, normal text spacing, a space after a control word is not rendered.
- **Operator names:** upright, with thin spaces around per TeX (`\sin x`, `\max_{\theta}`).

### B.9.6 Output: draw list

```c
typedef enum { BT_DRAW_GLYPH, BT_DRAW_RULE, BT_DRAW_ERRMARK } BtDrawKind;
typedef struct {
  BtDrawKind kind;
  float x, y;            /* em units, origin at box left on the baseline */
  union {
    struct { guint32 glyph; float scale; guint8 style; } g;  /* style: normal/bold/etc. */
    struct { float w, h; } rule;
    struct { float w, h; guint32 src_start, src_end; } err;
  };
} BtDrawOp;

typedef struct {
  float width, height, depth;      /* em */
  BtDrawOp *ops; gsize n_ops;
  gboolean has_error, has_warning;
  GPtrArray *errors;               /* message + column + src slice */
} BtMathLayout;
```

- The draw list is flat and immutable. Replaying it is O(n_ops) with no allocation.
- Glyph scale is the style scale (`T` 1.0, `S` per constant, `SS` per constant).
- Style variants (bold, calligraphic, blackboard, fraktur, script) map to Unicode math alphanumeric code points and the glyphs the font has for them. Report any missing variants.

## B.10 Layout tests (metrics, no pixels)

Use relative expectations (ratios and orderings) so tests survive font changes within the same family:

- a fraction in `T` style is taller than a single letter and its numerator baseline is above the axis
- a subscript's baseline is lower than the base baseline, a superscript's is higher
- `x^2_i` places sub and sup with at least the minimum gap
- `\sum_{i=1}^n` in `D` style stacks limits above and below, and in `T` style puts them at the side
- `\left( \frac{a}{b} \right)` delimiters are taller than the fraction
- `\sqrt{x}` radical bar spans the radicand width
- matrix column widths equal the widest cell in each column
- `a+b` has medium spaces around `+`, `-a` has none (unary)
- `a=b` has thick spaces around `=`
- `\sin x` has a thin space between `\sin` and `x`
- empty input has zero width and a defined baseline
- width of `\quad` is 1 em, `\qquad` 2 em, `\,` 3/18 em, `\!` negative 3/18 em

## B.11 Rendering and caching

- `mathrender.c` replays a draw list into a `cairo_t`, using a Cairo FreeType font face made from the same font blob. Glyph ids from HarfBuzz map to the same FreeType face. Verify on Windows MSYS2 that Cairo has FreeType support and report.
- Render to a **device-pixel texture** at the widget scale factor, then hand it to GTK as a `GdkTexture` / `GdkPaintable`. No bitmap scaling at draw time.
- Subpixel positioning on, hinting off (math looks better unhinted). Antialiasing on.
- Color comes from the theme token `@bt_math_fg` (default text color). Re-render on theme change and on scale-factor change.
- Caches:
  - **Layout cache:** key = `(hash, mode)`; LRU, at most 2,000 entries.
  - **Texture cache:** key = `(hash, mode, px_size, scale, rgba)`; LRU with a total budget of 32 MB.
  - Both caches are cleared when the font blob changes (never at runtime) and trimmed on memory pressure.
- Layout and texture creation happen lazily for formulas in or near the visible range, in idle callbacks, budgeted to at most 4 ms per idle tick. Formulas still pending show the source text in the dim style (never a blank hole).
- No allocation in the replay path. No Pango layout per frame.
- Baseline: the texture carries its baseline offset so inline math aligns with text.

## B.12 Editor integration (spike first)

The hard problem: show a rendered formula in place of the source text without changing the buffer content, undo history, or the saved file.

### B.12.1 Candidate approaches

| ID | Approach | Idea | Main risks |
|---|---|---|---|
| E1 | Invisible source + inline paintable | Tag the source range `invisible`, insert a `GdkPaintable` (or child anchor) next to it | Buffer gets an extra U+FFFC character; offsets shift; undo manager may record it; file save must exclude it |
| E2 | Invisible source + overlay drawing | Tag source invisible, reserve space with a custom mechanism, draw formulas in a `snapshot_layer` override of a `GtkTextView` subclass | Reserving inline width is hard; verify what the installed GTK exposes |
| E3 | Child anchor + widget | Same as E1 but with a widget in the anchor | Widgets are heavy for hundreds of formulas |
| E4 | Popover / side preview only | Never replace text; show rendered math in a popover when the caret is inside, or in a preview strip | Not in place, but the safest |

### B.12.2 Mandatory spike (time-boxed, before any M5 code)

Build a throwaway prototype on a copy of `editorpane.c` that tests E1, E2, and E4 against these **hard criteria**:

1. **File integrity:** after typing, deleting, undo, redo, paste, and save, the file bytes equal the buffer text. No U+FFFC or other artifacts, ever. A randomized test (fixed seed, 10,000 operations) proves it.
2. **Undo/redo:** `Ctrl+Z` / `Ctrl+Y` restore text exactly. Rendering state does not create undo steps and does not clear the undo stack.
3. **Offsets:** all of my existing features that use buffer offsets (links, tags, AI draft overlay, search, checkpoint diffs) keep working. If E1 shifts offsets, the ADR must show how every consumer maps offsets.
4. **Selection and copy:** selecting across a formula and copying yields the LaTeX source with delimiters. Verify what the clipboard contains for hidden text and fix it if needed.
5. **Caret behavior:** arrow keys move across a rendered formula as one unit or reveal the source; Home/End work; no dead caret positions.
6. **Accessibility:** the accessible text of the buffer remains the source.
7. **Performance:** 300 formulas in a note, scrolling at 60 FPS (B.19).
8. **Layout stability:** revealing or hiding a formula does not make the viewport jump more than one line.

### B.12.3 Decision rule

- Pick the **simplest** approach that passes all eight criteria. Write the ADR with evidence (test output, numbers).
- If none pass, ship **E4** first (popover on the caret line and hover) at STOP 2, and report what blocks in-place rendering with options.
- Never ship an approach that can corrupt the saved file. Integrity beats looks.

### B.12.4 Reveal-on-caret behavior

- Reveal the source when the caret is inside the span **or** the selection intersects it. Rendered otherwise.
- Hysteresis: moving the caret out of a span re-renders after 150 ms of no typing (so typing inside a formula does not flicker).
- While the source is revealed, style it with `@bt_math_source` (dim monospace) and show a small live preview popover under the caret line with the rendered formula (reuses the E4 popover). Update the preview on each pause in typing (debounce 120 ms).
- Clicking on a rendered formula places the caret at its end and reveals the source.
- Reveal and re-render never move the viewport more than one line.

## B.13 Display math presentation

- Display math is centered in the text column, with 8 px above and below (Dusk spacing scale), and a subtle block background only when the source is revealed.
- If a display formula is wider than the column, it scrolls horizontally inside its own block (overflow-x), and the page never scrolls sideways.
- Multi-line display math collapses to a single block when rendered.
- Equation numbers (`\tag{}`) are right-aligned and are Tier 2.
- A display span inside a list item keeps the list indent.

## B.14 Inline math presentation

- Inline math uses `T` style, so fractions and big operators are compact.
- Baseline-aligned with the text. If an inline formula is taller than 2.2 times the line height, accept the taller line but report the case in the self-test.
- Font size follows the editor font size. The editor zoom scales math with it.
- Inline math inside headings follows the heading size.
- Inline math inside a link text keeps the link underline off (only the link style applies outside the math).

## B.15 Error handling and display

- Every parse or layout problem produces an error record: message, column, source slice.
- Display: show the **source text** in `@bt_math_error` color with a wavy or dashed underline where Pango allows it, and put the message in the tooltip: `Unknown command \foo at column 12`.
- Partial rendering: if the error is recoverable (unknown command), render the rest and draw the failing command as `\name` in the error color.
- Hard errors (unbalanced braces) show the whole formula as source in the error style.
- Never show a blank hole, and never show a modal dialog.
- Warnings (missing glyph, tall inline formula) are collected for the self-test but do not change the display.
- The status bar may show `Math: N errors in this note` as a muted item. No toast spam.

## B.16 Macros (Tier 2)

- Optional per-vault macros in `.bt/macros.json`:

```json
{ "\\R": "\\mathbb{R}", "\\E": "\\mathbb{E}", "\\argmax": "\\operatorname*{arg\\,max}" }
```

- Support only simple argument-less and `#1`..`#9` argument macros defined by me in that file. No `\def` inside notes.
- Expansion limits: depth 32, total expansion size 8 KB per formula, no recursion (detect cycles). On violation: error with the macro name.
- The file is read once on vault open and on change (file watcher). A bad file shows one status-bar message and macros stay off.
- The macro file is data. Never execute anything from it. Agents may not write it (it is outside the allowed agent write set).

## B.17 Resource limits and safety for math

Notes are untrusted. Enforce all of these, fail with a normal error, and test each one:

| Limit | Value |
|---|---|
| Max source length, inline | 4,096 bytes |
| Max source length, display | 16,384 bytes |
| Max AST nesting depth | 64 |
| Max AST nodes | 5,000 |
| Max matrix size | 20 rows by 20 columns |
| Max macro expansion | see B.16 |
| Max draw ops per formula | 20,000 |
| Layout time per formula | 50 ms soft cap; beyond that, abort and show source with an error |
| Max formulas laid out per idle tick | budgeted by time (B.11) |

- No file access, no network, no process spawn from math code.
- No recursion without a depth counter. No unbounded loops.
- Fuzzing (B.21) must run clean under ASan/UBSan.
- Memory growth is bounded by the cache budgets.

## B.18 Theme tokens for math

| Token | Role | Guidance |
|---|---|---|
| `@bt_math_fg` | rendered formula color | same as `text-primary` |
| `@bt_math_source` | revealed source text | `text-secondary`, monospace |
| `@bt_math_error` | error style | danger `#F87171` |
| `@bt_math_block_bg` | display block background when revealed | `surface-2` at about 60% |
| `@bt_math_preview_bg` | preview popover background | `surface-2` |

- Add the tokens to `theme.css`, `theme.h`, `theme.c`, `test_theme.c`, and `docs/project.md`.
- No hard-coded colors in `mathrender.c` or `mathview.c`.

## B.19 Performance budgets (measure and report)

| Metric | Budget |
|---|---|
| Parse + layout, typical inline formula | under 0.3 ms |
| Parse + layout, each formula in the B.6.9 acceptance list | under 3 ms each |
| Render to texture, typical formula at 2x scale | under 1.5 ms |
| Open a note with 300 formulas, first paint | under 150 ms extra |
| Scroll through that note | 60 FPS, p95 frame under 16.6 ms |
| Typing latency inside a formula | under 8 ms per keystroke at p95 |
| Memory for 300 cached formulas | under 16 MB |
| Idle redraws with nothing changing | zero |

- Add `tests/bench/bench_math.c` and write results to `docs/bench/<date>.json`.
- Every number comes with the machine and GPU/driver string.

## B.20 Accessibility and clipboard

- The buffer text remains the source of truth, so assistive tech reads LaTeX source.
- Tooltip on a rendered formula shows its source on hover (after the default delay).
- Context menu on a rendered formula: `Copy LaTeX source`, `Copy with $ delimiters`, `Show source`.
- Copy of any selection containing formulas yields source text (B.12.2 criterion 4).
- Respect `gtk-enable-animations`: no fade transitions when disabled.
- Keyboard: `Ctrl+Shift+M` toggles math rendering on/off for the whole editor. The setting persists.
- Contrast of math color on the editor background meets WCAG AA (the token test already checks `text-primary`).

## B.21 Tests for Part B

### Group T — tokenizer

- `$x$`, `$ x$` (not math), `$x $` (not math), `$x$1` (not math), `\$5`, `$5 and $10`
- `$$x$$`, `$$` multi-line, `$$` unterminated, `$` unterminated
- math in list items, blockquotes (`> $$`), headings, table cells
- math inside a code span or fence is ignored
- math inside a wikilink or URL is ignored
- frontmatter containing `$` is ignored
- CRLF and BOM
- escaped `\$` inside math
- 5,000 spans in one buffer scan under 20 ms
- incremental rescan equals full rescan after random edits (fixed seed)
- **regression:** the exact lines from my screenshot produce the exact expected spans

### Group H — highlighter and tags

- zero emphasis tags inside any math span (the screenshot line and `a*b*c`)
- emphasis outside math still works
- links outside math still work, link-like text inside math is not a link
- language-spec contexts are in the right order (test by highlighting a fixture and comparing tags)

### Group P — parser

- every item in B.6.9 parses with zero errors
- each error class gives the right column: unbalanced brace, mismatched `\left/\right`, mismatched `\begin/\end`, double superscript, unknown command, missing argument
- partial AST for recoverable errors
- limits from B.17 (depth, nodes, matrix size, length)
- deterministic output for the same input
- arena frees cleanly (no leaks under ASan)

### Group L — layout (metrics)

- the B.10 list, plus: `\binom`, `\overset`, `\xrightarrow`, `\underbrace`, accents, `\text`, `\operatorname*`

### Group G — golden images

- render the gallery at 1x and 2x with the bundled font and compare to PNGs in `tests/golden/math/`
- tolerance: per-pixel RGB distance threshold plus a maximum differing pixel percentage (write both numbers in the manifest, start strict, loosen only with an ADR)
- a script `scripts/update-golden.ps1` regenerates goldens and prints a diff summary; goldens change only in reviewed commits

### Group F — fuzz and property tests

- fuzz the tokenizer and the parser with random bytes, mutated corpus, deeply nested braces, huge inputs; run at least 1,000,000 iterations in CI mode, shorter locally; zero crashes, zero sanitizer reports, every run finishes within the time cap
- property: `layout(parse(x))` never produces NaN or infinity
- property (editor spike result): file bytes equal buffer text after random edits with rendering on (B.12.2 criterion 1)

### Group I — interaction

- caret enters a formula: source revealed; leaves: rendered after hysteresis
- click on a formula reveals the source
- select across formulas and copy gives source
- toggle `Ctrl+Shift+M` restores raw text view exactly
- undo/redo across formula edits
- theme switch re-renders in the new color
- scale factor change re-renders crisp

## B.22 Self-test CLI for math

Add `bt.exe --selftest-math <vault_dir>` (read-only, headless, no window):

- scans every note, extracts every math span with the same tokenizer as the editor, parses and lays out each with the real engine and font
- prints: number of notes with math, inline count, display count
- prints: number of formulas with errors, and the first 50 with `file:line` and message
- prints: histogram of **unknown commands** (command name and count). This tells us what to add next.
- prints: timing (total ms, p50 and p95 ms per formula) and the largest formula by op count
- exit code 0 on completion, nonzero on internal error
- I will run it on `life2`. Include the real output in your phase report.

## B.23 Acceptance criteria for Part B

- [ ] In `02-bootstrap.md`, every formula from my screenshot is rendered as math, in place (or, if E4 is the fallback, in the popover) and the source is revealed on caret entry.
- [ ] No emphasis styling appears inside math (regression tests prove it).
- [ ] File bytes always equal buffer text (randomized test proves it).
- [ ] Undo/redo unaffected (test proves it).
- [ ] All Tier 1 items in B.6.9 render with no errors.
- [ ] `--selftest-math` runs on `life2` and reports a histogram of unknown commands.
- [ ] Golden image tests pass. Fuzz runs are clean.
- [ ] Performance budgets in B.19 are met, or misses are reported with numbers and reasons.
- [ ] Math toggle, copy-as-source, and tooltips work.
- [ ] Packaged exe launched, build stamp matches, manual checklist passed.
- [ ] Docs (`docs/math.md`, `docs/project.md`), ADRs, and CHANGELOG updated.

---

# PART C — SHARED WORK

## C.1 Settings and menu

- Settings stored in `state.ini`:

```ini
[editor]
links_open_on_click = true      ; false means Ctrl+Click required
confirm_external_urls = true
math_render = true
```

- App menu entries: `Open links with single click`, `Confirm external URLs`, `Render math`.
- Keyboard: `Ctrl+Shift+M` math toggle (B.20). Document all new shortcuts in the shortcuts list if one exists.
- No settings window. No other options.

## C.2 Documentation to write or update

- `docs/links-and-math-plan.md` (Phase 0 output: findings, plan, risks)
- `docs/DECISIONS.md`: one ADR each for
  1. link resolution rules and the single shared resolver
  2. click behavior defaults and the caret-inside rule
  3. open-target security policy (block list, allow list, confirm rules)
  4. math tokenizer rules (pandoc-style `$`) and the emphasis fix
  5. math engine choice (B.3) with spike evidence
  6. math font choice and license
  7. editor integration approach (B.12) with the eight criteria evidence
  8. caching and budgets
- `docs/math.md`: supported commands, unsupported list, limits, macros, known limitations, troubleshooting
- `docs/project.md`: new modules, tokens, settings
- `CHANGELOG.md`: user-facing entries per phase
- `vault-template/examples/math-gallery.md` and a short `links-demo.md` so a fresh vault shows both features working

## C.3 Interaction with the AI-agent guardrails

- Agents still write only frontmatter `tags_ai` / `links` and `## AI draft` sections. Nothing in this work widens that.
- Links and math that agents write inside `## AI draft` are rendered and clickable like any others.
- Clicking a link or rendering math never starts an agent, never runs a command, and never writes to the vault (except the explicit create-note popover).
- `bt-mcp.exe`: optional, low priority: expose `resolve_link(target, source)` read-only. Skip unless it is trivial, and say so in the report.
- Rendering code never reads outside the vault and never makes network requests.
- The self-test CLIs are read-only against any vault you point them at.

## C.4 Verification protocol (every phase)

1. Build with the existing Meson setup. No new warnings.
2. Run all unit tests, plus sanitizer builds when available.
3. Run relevant benches and write results to `docs/bench/<date>.json`.
4. Run `scripts/package-win.ps1`. If it reports a locked file, stop and tell me; do not write to another folder.
5. Launch `dist\brainterm\bt.exe` and confirm the build stamp matches the commit.
6. Run the relevant self-test on `life2` and paste the real output into the report.
7. Capture evidence into `docs/evidence/<date>-<phase>/`:
   - PNGs from a deterministic capture mode, if you add one (`--editor-capture <dir> --editor-state <name>`), with a JSON sidecar (window size, scale, build stamp)
   - bench JSON
   - self-test text output
8. Write the manual checklist for me: 10 to 15 short steps with expected results.
9. I will also record a screen video of the interaction. Tell me which actions to record.

## C.5 Report format at every STOP GATE

1. **Built:** bullet list, with commit hashes.
2. **Measured:** table of numbers (tests passed, bench results, self-test counts), each with the machine and GPU/driver.
3. **Evidence paths:** where the files are.
4. **Skipped / deferred:** each item with the reason.
5. **Decisions made without asking:** each with the reason.
6. **Risks and open questions:** ranked.
7. **Manual checklist for Riza.**
8. **Next phase plan** in 5 lines or fewer.

Keep the whole report under two pages.

## C.6 Risk register (update in each report)

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| HarfBuzz MATH API missing or incomplete in the MSYS2 build | Low | High | Spike in P0; fallback option 2 in the ADR |
| Inline rendering corrupts buffer offsets or undo | Medium | High | Spike criteria; E4 fallback; randomized integrity test |
| Emphasis/highlighter conflicts persist | Medium | Medium | Context order test; tag pass skips math ranges |
| Math font glyph gaps for some commands | Medium | Low | Coverage report; error style for missing glyphs |
| Performance on notes with hundreds of formulas | Medium | Medium | Lazy layout, caches, budgets, bench |
| Link open path launches something dangerous | Low | High | Block list, allow list, mocks, tests, no shell |
| Resolver disagrees with the graph's ghost nodes | Medium | Medium | Single shared resolver; equivalence test |
| Locked exe causes stale-binary confusion again | Medium | Medium | Build stamp check in every report |

---

# APPENDICES

## Appendix 1 — Math test corpus (from my notes and typical stats/ML)

Use these in tokenizer, parser, layout, golden, and fuzz-seed tests. All must parse without errors unless marked.

```
$B$
$n$
$b = 1, \ldots, B$
$\hat{\theta}^{*(b)}$
$\hat{\theta}^*$
$$(\hat{\theta}^*_{(\alpha/2)}, \hat{\theta}^*_{(1-\alpha/2)})$$
$\theta^* \sim q(\theta^* | \theta^{(t-1)})$
$r = \frac{f(\theta^* | x) q(\theta^{(t-1)} | \theta^*)}{f(\theta^{(t-1)} | x) q(\theta^* | \theta^{(t-1)})}$
$\theta^{(t)} = \theta^*$
$\min(1, r)$
$\bar{x} = \frac{1}{n}\sum_{i=1}^{n} x_i$
$s^2 = \frac{1}{n-1}\sum_{i=1}^{n}(x_i-\bar{x})^2$
$\sigma^2$
$\mu_0$
$H_0 : \mu = \mu_0$
$H_1 : \mu \ne \mu_0$
$t = \frac{\bar{x}-\mu_0}{s/\sqrt{n}}$
$p(\theta \mid x) \propto p(x \mid \theta)\, p(\theta)$
$\hat{\beta} = (X^\top X)^{-1} X^\top y$
$\mathbb{E}[X] = \int_{-\infty}^{\infty} x f(x)\,dx$
$\operatorname{Var}(X) = \mathbb{E}[X^2] - (\mathbb{E}[X])^2$
$X \sim \mathcal{N}(\mu, \sigma^2)$
$X_1, \ldots, X_n \overset{\text{iid}}{\sim} F$
$\sqrt{n}(\bar{X}_n - \mu) \xrightarrow{d} \mathcal{N}(0, \sigma^2)$
$\hat{\theta}_n \xrightarrow{p} \theta$
$\ell(\theta) = \sum_{i=1}^n \log f(x_i; \theta)$
$\hat{\theta}_{MLE} = \arg\max_{\theta} \ell(\theta)$
$\nabla_\theta J(\theta) = \frac{\partial J}{\partial \theta}$
$\theta_{t+1} = \theta_t - \eta \nabla_\theta J(\theta_t)$
$\mathrm{softmax}(z)_i = \frac{e^{z_i}}{\sum_j e^{z_j}}$
$\mathcal{L} = -\sum_{i} y_i \log \hat{y}_i$
$\lVert w \rVert_2^2$
$\lvert x \rvert$
$\binom{n}{k} = \frac{n!}{k!(n-k)!}$
$$\int_a^b f(x)\,dx = F(b) - F(a)$$
$$\sum_{k=0}^{\infty} \frac{x^k}{k!} = e^x$$
$$\begin{pmatrix} a & b \\ c & d \end{pmatrix}\begin{pmatrix} x \\ y \end{pmatrix}$$
$$f(x) = \begin{cases} 1 & x \ge 0 \\ 0 & \text{otherwise} \end{cases}$$
$$\begin{aligned} y &= ax + b \\ &= a(x + c) \end{aligned}$$
$$X \perp\!\!\!\perp Y \mid Z$$
$$\prod_{i=1}^{n} f(x_i; \theta)$$
$\mathbf{x} \in \mathbb{R}^d$
$\mathcal{O}(n \log n)$
$\alpha, \beta, \gamma, \Gamma, \Sigma$
$\forall \epsilon > 0, \exists \delta > 0$
$A \subseteq B \cup C$
$\overline{A \cap B}$
$\underbrace{x + \cdots + x}_{n}$
$\overbrace{a, b, c}^{3}$
$\tilde{x}, \vec{v}, \dot{x}, \ddot{x}$
```

Must-fail-gracefully samples (error style, no crash):

```
$\frac{a}{$
$\foo{x}$
$x^a^b$
$\left( x \right]$
$\begin{pmatrix} a & b \end{matrix}$
$\input{secret}$
$\def\x{\x}\x$
$ \sqrt[ $
```

Not math (money and escapes):

```
It costs $5 and $10 in total.
Use \$ for a literal dollar.
$ not math $
```

## Appendix 2 — Hostile link samples (all must be safe)

```
[[..\..\Windows\System32\calc.exe]]
[[C:\Windows\System32\cmd.exe]]
[[\\evil\share\payload.exe]]
[[assets/paper.pdf.exe]]
[[scripts/run.ps1]]
[x](javascript:alert(1))
[x](file:///C:/Windows/System32/calc.exe)
[x](ms-msdt:/id PCWDiagnostic)
[x](search-ms:query=a)
[x](https://user:pass@example.org/)
[x](data:text/html;base64,PHNjcmlwdD4=)
[[CON]]
[[a.txt:hidden]]
[[note.md ]]
```

Expected: none launches anything. Each shows a clear blocked message or a confirm popover, and the tests assert the launcher mock was not called.

## Appendix 3 — Manual checklist template (fill in per phase)

1. Launch `dist\brainterm\bt.exe`. Build stamp matches `<hash>`.
2. Open `life2`, open `MathStats.md`.
3. Click `[[chapter-01-descriptive/01-data-types-collection.md]]`. It opens in the editor.
4. Press `Alt+Left`. You return to `MathStats.md` at the same scroll position.
5. Hover a broken link. The tooltip says it can be created. Click it, cancel. No file is created.
6. Hover an `https://` link. Tooltip shows the URL. Click it. A confirm popover shows once.
7. Put the caret inside a link. Click inside the same link. It does not open.
8. Open `02-bootstrap.md`. Formulas render as math.
9. Move the caret into a formula. Source appears with a live preview.
10. Move the caret out. The formula renders again.
11. Select a paragraph with formulas and copy. Paste into a plain text editor. LaTeX source with `$` appears.
12. Type a broken formula. The source shows in the error style with a tooltip.
13. Press `Ctrl+Z` a few times. Text undoes correctly, no odd characters.
14. Press `Ctrl+Shift+M`. Raw text view appears. Press again. Math returns.
15. Save and close. Open the file in another editor. The content is exactly what I typed.

## Appendix 4 — Glossary

- **Atom class:** TeX category (Ord, Op, Bin, Rel, Open, Close, Punct, Inner) that decides spacing.
- **Axis height:** the height above the baseline where the middle of `+`, `=`, and fraction bars sit.
- **Cramped style:** a style where superscripts are lowered, used under radicals and in denominators.
- **Display / text style:** large (own line) versus compact (inline) math layout.
- **Draw list:** flat array of glyph and rule operations produced by layout.
- **Ghost link:** a link whose target note does not exist.
- **Italic correction:** extra width after a slanted glyph so a following script does not collide.
- **MATH table:** OpenType table with math layout constants, glyph variants, and assemblies.
- **Reveal-on-caret:** showing the raw source when the caret touches a rendered element.
- **Stop gate:** a point where you stop, report, and wait for my approval.

## Appendix 5 — Definition of done (whole brief)

- All six acceptance lists (A.17, B.23, and per-phase checklists) are ticked with evidence.
- `--selftest-links` and `--selftest-math` outputs on `life2` are attached.
- Golden, fuzz, security, and integrity tests pass in CI.
- Documentation, ADRs, and changelog are complete.
- `dist\brainterm\bt.exe` is the file I launch, and its build stamp matches the final commit.
- A list of known limitations is published in `docs/math.md` and `docs/project.md`.

*End of brief.*
