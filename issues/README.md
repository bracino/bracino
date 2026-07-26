# issues/ — lab notebook

Thin, repo-local bug/feature trail for **Bracino**. Not Jira.

Same workflow spirit as other lab projects (e.g. asciiWiring): numbered markdown files, open → closed with **Fix** + **Verify**, no deletion of closed lore.

## Workflow

1. Human posts a numbered scribble list (`1. … 2. …`) in chat, or a single clear ask.
2. Agent files each item as `open/NNN-slug.md` (next free `NNN`, zero-padded).
3. Work the issue; when done, **move** to `closed/NNN-slug.md`, fill **Fix** + **Verify**, set `Status: closed`.
4. Update `docs/STATUS.md` / `docs/ROADMAP.md` when capability, public contracts, or phase posture change.

**Do not** delete closed issues — they hold root-cause lore (wrong fix vs right fix), including plant-safety near misses.

## Numbering

- Monotonic integers: `001`, `002`, …
- Next id = max existing id in `open/` + `closed/` + 1 (ignore `fixtures/` names except when they share the issue id prefix)
- Filename: `NNN-short-kebab-slug.md`
- One concern per file when practical

## Template

```markdown
# NNN — short title

- **Status:** open | closed
- **Type:** bug | enhancement | task | design
- **Opened:** YYYY-MM-DD
- **Closed:** YYYY-MM-DD   # if closed
- **Refs:** paths, docs, related NNN, chat batch if any

## Context
## Repro          # bugs; optional for design/task
## Expected
## Actual         # bugs
## Proposal       # design/task: options + recommendation
## Fix            # closed: root cause + key files/decisions
## Verify         # closed: bench / build / compose / manual check
```

Design-heavy work (MQTT schema, BOM) may lean on **Context / Proposal / Fix** more than Repro/Actual.

## Where captures live

| Location | Git? | Use for |
|----------|------|---------|
| `ephemera/` (repo root) | **No** (gitignored) | Serial logs, scope shots notes, throwaway captures |
| `issues/fixtures/` | **Yes** | Minimal logs/payload samples an issue needs after clone |
| `docs/` | **Yes** | Settled schemas and runbooks (promote out of issues when stable) |
| Inline in the issue `.md` | **Yes** | Short freezes of topics, struct layouts, failing asserts |

**Policy:** day-to-day dumps may sit in `ephemera/`; before relying on them in a closed issue, promote inline or into `issues/fixtures/NNN-…` or `docs/`.

## Agents

See root `AGENTS.md` → **Issues notebook**. New sessions: skim `issues/open/` before firmware/protocol/server work.
