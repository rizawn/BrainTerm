---
id: bt-20261123-000900
title: "Links Demo"
created: 2026-11-23T08:00:00Z
updated: 2026-11-23T08:00:00Z
source: human
agent: null
tags: [example, links]
tags_ai: []
links: ["[[link-target]]"]
---

# Links Demo

This template note demonstrates every link status and resolution path in BrainTerm:

1. **Resolved sibling wikilink:** [[link-target]]
2. **Wikilink with alias:** [[link-target|Sibling Target Alias]]
3. **Wikilink with heading anchor:** [[link-target#Overview]]
4. **Unresolved wikilink (click to test Create popover):** [[missing-demo-note]]
5. **Ambiguous wikilink (click to test Chooser popover):** [[duplicate]]
6. **Markdown link with percent-encoded space:** [Spaced Note](spaced%20note.md)
7. **External HTTPS link:** [Example Reference](https://example.org/brainterm)
8. **Mailto link:** [Contact Author](mailto:author@example.org)
9. **Blocked link (blocked on purpose by ADR-017 security policy):** [Blocked Script](javascript:alert(1))
10. **Template placeholder (plain text on purpose, not a broken link):** [[{{template_placeholder}}]]
