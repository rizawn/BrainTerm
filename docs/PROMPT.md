# BrainTerm — Agent Operating Instructions (PROMPT.md)

| Field | Value |
|---|---|
| Audience | The AI coding agent that will implement BrainTerm from `docs/project.md` |
| Version | 2.0 — 2026-11-21 |
| Companion | `docs/project.md` (the specification) — read it FIRST |
| Language of code | C17. Comments and identifiers in English. |

You are implementing **BrainTerm**: a local-first Markdown second-brain with an embedded real terminal, an auto-organizing memory engine, and a brain-shaped graph — as a pure **C17 + GTK4** native application, per ADR-001 in the specification.

---

## 1. Read order (mandatory, in this order)

1. `docs/project.md` §0–§3 (changes, definition, principles). **The seven principles P1–P7 are law.** They override everything else in the spec, every roadmap convenience, and any clever idea of your own.
2. `docs/project.md` §5 (runtime architecture) and §7 (data model). These define the contracts you may not improvise around: the terminal byte path (§5.4), the threading model (ADR-007), the SQLite schema + FTS triggers (§7.2), and one-way file→index reconciliation (§7.3).
3. The milestone you are currently on: §12's row for that milestone, plus the feature section it cites (§6.A–§6.D) with its acceptance criteria.
4. `docs/DECISIONS.md` — the running decision log. Append to it whenever you make a non-obvious choice.

Never implement from memory of a previous session: re-read the section you touch.

## 2. Environment & build

- Toolchain: **Meson + Ninja**; on Windows, **MSYS2 UCRT64**. Dependencies (all pinned via Meson wraps or system packages, never vendored ad hoc): `gtk-4`, `gtksourceview-5`, `libyaml`, `sqlite3`, `libgit2`, `libvterm`, `md4c`, `cjson`; test-only `cmocka`.
- Commands: `meson setup build && meson compile -C build && meson test -C build`. The build MUST be warning-clean under `-Wall -Wextra -Wpedantic` (treat warnings as errors in CI).
- CI: GitHub Actions, `windows-latest` + `ubuntu-latest`. The Windows job runs the packaging smoke test (§11.7) from M1 onward.

## 3. Working rules

1. **Milestone discipline.** Work strictly in M1→M8 order (§12). A milestone is done ONLY when every item in its "Done when" column passes, its tests exist, and its "Explicitly NOT in it" scope guard is respected. Do not start the next milestone while the current one is unfinished; do not back-fill features "while we're here" — that is scope creep, the risk §9 of the draft explicitly names.
2. **Tests ship with the feature, not after.** Every unit in §11 gets written in the same commit-range as the code it tests. A feature without its test does not exist.
3. **C style:** ISO C17, no GNU extensions in portable files (`pty_win.c` may use Win32 API). Naming: `bt_` prefix for public symbols, `module_action` naming (`vault_resolve`, `index_upsert_note`). Error handling: return `gboolean`/`BtStatus` with an out-param message, no longjmp, no exit() from library code. Every allocation has exactly one owner; check every return from allocation and IO (the app's own spec §6.A3 exists because half-written files are real). No global mutable state outside `BtApp`.
4. **Re-use before writing (ADR-001 mitigations):** never hand-roll YAML, Markdown, VT parsing, git plumbing, or JSON — use libyaml, MD4C, libvterm, libgit2, cJSON. What we DO write ourselves (terminal renderer, scrollback, graph layouts, path guard) must have the dedicated tests from §11.
5. **Security is a functional requirement.** The threat model (§8) is not a footnote: `vault_resolve()` is the single choke point for every path (§6.A4); organizer CLI calls never interpolate note text into shell strings (§8.1); OSC 52 stays off by default (§8.2). A change that weakens any of these is a bug, even if everything else passes.
6. **When blocked or spec-silent:** check §14 (open questions) — if your question is listed, the recorded default stands. If it is genuinely new: make the smallest decision consistent with P1–P7, log it in `docs/DECISIONS.md` with one paragraph of reasoning, and continue. Do not stop, and do not silently redesign.
7. **Checkpoints before agent-facing writes.** Anything that lets an external agent write to the vault (organizer batches, launcher) goes through the `checkpoint_take()` façade first (ADR-006). If the checkpoint fails, the launch is refused (R10) — that is correct behavior, do not "fix" it by skipping.

## 4. Per-milestone definition of done (condensed from §12)

- **M1:** vault open, tree, editor (GtkSourceView + custom markdown lang), atomic save, crash-rig R1 green, 1k-scan < 2 s.
- **M2:** one terminal tab; ConPTY + libvterm + custom grid renderer; vim/htop/claude TUI usable; keystroke→glyph < 16 ms; resize debounced 100 ms; zero orphan processes on close (sweep test); scrollback ring; VT corpus green.
- **M3:** multi-tab (cap 8) + cwd badges; open-terminal-here; Claude/Codex/Gemini launcher with context handoff (`TERM_PROGRAM=brainterm`, `BT_ACTIVE_NOTE`, AGENTS.md wiring); git checkpoint per launch.
- **M4:** SQLite schema §7.2 verbatim (incl. FTS triggers); ask bar with search/command modes; < 100 ms @ 1k notes; R2 rebuild.
- **M5:** watcher + conflict banner (never auto-overwrite); session registry + Resume.
- **M6:** organizer with three backends (CLI headless / local model / No-AI rules — No-AI is default and must always work); human lock enforced (fuzz test green); batch checkpointing.
- **M7:** graph 2D, brain layout (forced spine/brainstem + force-relaxed cortex), every `.md` file = one node (node-count == file-count assertion), ghost nodes, filters, growth animation, 60 fps @ 1k nodes.
- **M8:** importers; `bt-mcp` read-only MCP bridge; optional embeddings (disableable); hybrid search; 3D is a MAY — cut it first.

## 5. Anti-patterns (each of these has silently killed projects like this)

- Storing a durable fact only in SQLite "because it's easier" — violates P5. The golden rebuild test exists to catch you.
- Letting the organizer write to `tags` or anywhere in the body — violates P4. The human-lock fuzz test exists to catch you.
- Building a chat UI "for convenience" of the Ask mode — violates P6. Ask = evidence pane + "open in agent".
- Parsing VT output with ad-hoc string matching instead of libvterm state — the conformance corpus exists to catch you.
- Adding cloud sync, telemetry, or any network call from the core — violates P1 and §8.4 outright.
- Skipping the Windows clean-VM packaging smoke "until later" — GTK-on-Windows rot is discovered late exactly by teams that do this.
- Touching two milestones in one branch. Small commits, one concern, every commit green.

## 6. Deliverable per session

At the end of each work session: (1) `meson test` output for the current milestone; (2) a short note in `docs/DECISIONS.md` covering decisions made this session; (3) if a §10 budget is now measurable, its measured value. State plainly what is NOT done — never report a milestone complete on unverified code.

---

*The spec's closing rule applies to you twice over: §3 (P1–P7) overrides everything, including your own good ideas.*
