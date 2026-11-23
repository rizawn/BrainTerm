# L2 Interactive Manual Checklist (For Riza)

> Record your pass/fail notes in `docs/evidence/2026-11-23-l2/manual-checklist-results.md`.
> Open the `life2` vault in read-only / normal mode without mutating research notes unless testing an edit inside a disposable scratch note.

## 1. Click & Caret Rules (ADR-016)
- [ ] Open `statistical-statistics-notes/MathStats.md`.
- [ ] **Single-click open:** With the caret elsewhere in the note, single-click any blue resolved wikilink (e.g., `[[chapter-01-descriptive/01-data-types-collection.md]]`). Verify it opens the target note immediately.
- [ ] **Drag safety:** Press and drag across text overlapping a link to create a selection. Release mouse and verify the link does **not** activate.
- [ ] **Caret-inside rule:** Click to place the caret using `Alt+Click` inside a wikilink (or move with arrow keys), then single-click inside that same link span. Verify it places/moves the caret for editing instead of opening the link.
- [ ] **Override shortcut:** With the caret inside a link, press `Ctrl+Click` on the link. Verify it opens the target despite the caret being inside.
- [ ] **Alt+Click:** Hold `Alt` and click any link. Verify it places the caret inside the link without navigating.

## 2. Hover Tooltips & Visual Link Styles
- [ ] Hover over a resolved wikilink (`[[...]]`): verify muted subtle styling on `[[`/`]]` delimiters, crisp accent underline/color on target text, and tooltip showing resolved relative path / title.
- [ ] Hover over a Markdown link inside the vault (`[label](relpath)`).
- [ ] Hover over a code block or inline `` `[[not_a_link]]` `` span or `$...$` math formula containing brackets: verify it is **not** styled or clickable as a link.
- [ ] Hover over an unresolved wikilink: verify distinct unresolved style and tooltip (`Note does not exist yet` / unresolved indicator).
- [ ] Hover over an external URL containing `user:pass@` (or create one temporarily in a scratch note): verify `user:pass@` is stripped in the tooltip and confirm popover.

## 3. Navigation History (`Alt+Left` / `Alt+Right`)
- [ ] Navigate forward across 3 notes via link clicks.
- [ ] Press `Alt+Left` twice and verify both prior notes re-open at the previous caret/scroll position.
- [ ] Press `Alt+Right` and verify forward navigation returns cleanly.

## 4. Security Policy & Settings Toggle (ADR-017)
- [ ] Toggle `Editor -> Open links on click` off in preferences / menu (or `[editor] links_open_on_click = false` in `state.ini`), verify single-click places caret while `Ctrl+Click` opens links. Restore toggle to `true`.
- [ ] Verify blocked unsafe targets (e.g. `.bat`, `.exe`, `file:///`, `javascript:`) never launch and display a blocked reason notice.
