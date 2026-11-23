# BrainTerm Phase L3 — Manual Verification Checklist for Riza

> **IMPORTANT:** Run this checklist on a **COPY** of `life2` (e.g., copy `life2` to `Desktop\life2-l3-copy` first). Never run create-note tests on the original `life2` folder.

1. [ ] **Build stamp check:** Launch `dist\brainterm\bt.exe`, verify the bottom-right status bar build stamp matches `git rev-parse --short HEAD` with **no `-dirty` suffix**.
2. [ ] **Unresolved note link → Create popover:** Open `statistical-statistics-notes/MathStats.md`, click `[[chapter-01-descriptive/02-graphical-representation.md]]`. Verify the `Create "02-graphical-representation"?` popover appears with `Will create: statistical-statistics-notes/chapter-01-descriptive/02-graphical-representation.md` and no file is created on disk yet.
3. [ ] **Cancel create:** Press `Escape` (or click `Cancel`). Verify the popover closes and the note is still not created on disk.
4. [ ] **Confirm create:** Click the same link again and press `Enter` (or click `Create`). Verify the new note opens with canonical 9-key YAML frontmatter and `# 02-graphical-representation`, and the status bar shows `Created statistical-statistics-notes/chapter-01-descriptive/02-graphical-representation.md`.
5. [ ] **Back navigation after create:** Press `Alt+Left` to return to `MathStats.md`. Verify the link is now styled as a normal resolved link (purple underline, no amber wavy underline) and `MathStats.md` was not modified on disk.
6. [ ] **Non-note unresolved link:** In a test note, type `[[missing-image.png]]` and click it outside the caret. Verify no create popover opens and the status bar displays `Not a note: .png files cannot be created here`.
7. [ ] **Ambiguous wikilink → Chooser popover:** Open `claude-obsidian/wiki/index.md` and click `[[claude-obsidian-ecosystem-research]]`. Verify the chooser popover lists both `claude-obsidian/wiki/sources` and `claude-obsidian/.raw` candidates with candidate 1 pre-selected.
8. [ ] **Copy full path link from chooser:** Press `Down` then `Ctrl+C` (or click `Copy full path link`). Verify the chooser stays open, the status bar shows `Copied link for claude-obsidian-ecosystem-research`, and the clipboard contains `[[claude-obsidian/.raw/claude-obsidian-ecosystem-research]]`.
9. [ ] **Open candidate from chooser:** Press `Enter`. Verify the selected candidate opens and `claude-obsidian/wiki/index.md` is unmodified on disk.
10. [ ] **Keyboard link navigation (`Ctrl+Alt+Right` / `Ctrl+Alt+Left` & `Ctrl+Enter`):** Press `Ctrl+Alt+Right` repeatedly to jump the caret from link to link (wrapping from last to first), then press `Ctrl+Enter` on a link to activate it.
11. [ ] **Confirm external URLs toggle:** Open the primary menu (`☰`), toggle **Confirm external URLs** off, and click an `https://` link. Verify it opens without a confirmation popover, while a `javascript:alert(1)` link remains blocked.
