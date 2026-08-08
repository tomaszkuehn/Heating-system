---
description: Detect and revert regressions (last-good, suspect, revert <file|all|stash>, feature <add|list|mark|check>)
---

You are executing the regression utility subcommand: "$ARGUMENTS".

## FIRST: deterministic result (precomputed by the plugin)
If `.opencode/memory/command_result.txt` exists and its FIRST line contains `/regression` and the command token matches this subcommand, output the file contents verbatim (from line 2) as your whole reply and STOP.

## HARD RULES
1. **NEVER run `git commit`, `git push`, `git reset --hard`, `git clean`, `git stash drop`, or `git checkout <sha> -- .` (whole-tree).**
2. Read-only subcommands (`last-good`, `suspect`, `check`) must NOT modify anything.
3. `feature` subcommands write ONLY to `.opencode/memory/cache/features.json` and may run `git notes append` on HEAD **for `mark` only**. Never commit.
4. `revert` is the ONLY destructive subcommand. It may run `git checkout <sha> -- <file>` for a SPECIFIC file, or `git stash push`. It must always ask for explicit confirmation first and only touch the named file(s) — never whole-tree.
5. If the subcommand is not recognized, ASK for clarification.

## Manual map (no precomputed file)
- `last-good` / `suspect` — READ ONLY. Print the regression window from `.opencode/memory/cache/test-history.json` (lastGood/firstRed) and suspect files.
- `feature add|list|mark|check` — manage features in `.opencode/memory/cache/features.json`. `mark` may append a `git notes` on HEAD.
- `revert <file>` — require confirmation, then `git checkout <sha> -- <file>` for that file only.
- `revert stash` — `git stash push -m "..."` (reversible).

End with: "**Uwaga:** <command> nie commitowało. Wszelkie modyfikacje są ograniczone do wskazanego pliku / pamięci projektu."