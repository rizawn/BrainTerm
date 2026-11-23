# PF-2: Placeholder Count Reconciliation (83 → 71)

## Summary
- **P-2 Baseline (`unresolved-categories.txt`):** `83` total `UNRESOLVED` links, of which `16` were manually grouped under `a) TEMPLATE_PLACEHOLDER`.
- **P-3 / L2 Classifier (`--selftest-links life2`):** `71` `UNRESOLVED` + `12` `PLACEHOLDER` (`83 - 12 = 71`).
- **Root Cause:** The P-3 rule in `src/linkresolve.c` classifies only mustache/brace template placeholders matching `{{...}}` as `BT_LINK_PLACEHOLDER` (`12` links). Four bare example link stems in `claude-obsidian/skills/wiki-mode/templates/lyt/moc-template.md` do not use `{{...}}` delimiters, so they intentionally remain `BT_LINK_UNRESOLVED` (`4` links) rather than risking false suppression of ordinary kebab-case wikilinks like `[[note-1]]`.

## Item-by-Item Breakdown (All 16 Rows from P-2)

| # | File & Line | Target | P-3 Classification | Reason |
|---:|---|---|---|---|
| 1 | `claude-obsidian/skills/wiki-mode/templates/lyt/atomic-template.md:17` | `{{source-1}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 2 | `claude-obsidian/skills/wiki-mode/templates/lyt/atomic-template.md:22` | `{{related-note}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 3 | `claude-obsidian/skills/wiki-mode/templates/lyt/moc-template.md:20` | `note-1` | `UNRESOLVED` | Non-brace token; kept unresolved so kebab-case wikilinks still resolve/offer creation |
| 4 | `claude-obsidian/skills/wiki-mode/templates/lyt/moc-template.md:21` | `note-2` | `UNRESOLVED` | Non-brace token; intentionally remains `UNRESOLVED` |
| 5 | `claude-obsidian/skills/wiki-mode/templates/lyt/moc-template.md:22` | `note-3` | `UNRESOLVED` | Non-brace token; intentionally remains `UNRESOLVED` |
| 6 | `claude-obsidian/skills/wiki-mode/templates/lyt/moc-template.md:26` | `adjacent-moc-1` | `UNRESOLVED` | Non-brace token; intentionally remains `UNRESOLVED` |
| 7 | `claude-obsidian/skills/wiki-mode/templates/para/area-template.md:23` | `{{project-1}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 8 | `claude-obsidian/skills/wiki-mode/templates/para/area-template.md:24` | `{{project-2}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 9 | `claude-obsidian/skills/wiki-mode/templates/para/area-template.md:28` | `{{resource-1}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 10 | `claude-obsidian/skills/wiki-mode/templates/para/project-template.md:31` | `{{related-area}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 11 | `claude-obsidian/skills/wiki-mode/templates/para/project-template.md:32` | `{{related-resource}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 12 | `claude-obsidian/skills/wiki-mode/templates/para/resource-template.md:27` | `{{area-1}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 13 | `claude-obsidian/skills/wiki-mode/templates/para/resource-template.md:28` | `{{project-1}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 14 | `claude-obsidian/skills/wiki-mode/templates/zettel/atomic-template.md:24` | `{{parent-id}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 15 | `claude-obsidian/skills/wiki-mode/templates/zettel/atomic-template.md:29` | `{{related-zettel-1}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |
| 16 | `claude-obsidian/skills/wiki-mode/templates/zettel/atomic-template.md:30` | `{{related-zettel-2}}` | `PLACEHOLDER` | Matches `{{...}}` delimiter rule |

## Conclusion
- `12` `{{...}}` placeholders + `4` intentional non-brace template tokens = `16` P-2 `TEMPLATE_PLACEHOLDER` entries.
- Arithmetic matches exactly: `83 - 12 = 71` `UNRESOLVED` in `--selftest-links life2`.
