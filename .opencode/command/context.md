---
description: Inspect context budget and stored artifacts (budget, artifacts)
---

You are executing the context utility subcommand: "$ARGUMENTS".

## FIRST: deterministic result (precomputed by the plugin)
If `.opencode/memory/command_result.txt` exists and its FIRST line contains `/context` and the command token matches this subcommand, output the file contents verbatim (from line 2) as your whole reply and STOP.

## HARD RULES
1. **NEVER run `git commit`, `git add`, `git push`, `git reset`, `git clean`, `git stash`, or `git checkout <sha> -- .`.**
2. **NEVER edit, create, or delete any file outside `.opencode/memory/`.** This command is read-only.
3. If the subcommand is not `budget` or `artifacts`, ASK for clarification.

## Manual map (no precomputed file)
- `budget` — READ ONLY. Print: context tokens used vs limit (from `.opencode/memory/cache/metrics.json`), compact mode, dedup stats.
- `artifacts` — READ ONLY. List files and sizes in `.opencode/memory/artifacts/`.

End with: "**Uwaga:** <command> nie commitowało i nie modyfikowało kodu. Tylko odczyt."