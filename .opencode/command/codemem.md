---
description: Manage project memory (status, show, save, clear-session, clear-project, compact, init, auto, auto-refresh, compact-status, compact-now, compact-reset, test-history, lesson, tui, dashboard, exit, ai status, ai auto-timeout, ai triage)
---

You are executing the memory utility subcommand: "$ARGUMENTS".

## FIRST: deterministic result (precomputed by the plugin)
If the file `.opencode/memory/command_result.txt` exists and its FIRST line begins with `# /codemem` or `# /context` or `# /regression` AND the command token after `# ` matches "$ARGUMENTS" (or contains this subcommand), then output the ENTIRE file contents verbatim (from line 2 onward) as your whole reply and STOP. Do not add commentary.

Otherwise (plugin absent or no matching precomputed result), follow the manual rules below.

## HARD RULES (never break these)
1. **NEVER run `git commit`, `git add`, `git push`, `git reset`, `git clean`, `git checkout <sha> -- .`, `git stash`, amend, or force.**
2. **NEVER edit, create, or delete any file outside of project folder.
3. If "$ARGUMENTS" is not in the list below, or you cannot determine its meaning, ASK for clarification — do not guess.

## Manual subcommand map (used only when there is no precomputed file)
- `status` — READ ONLY. Print a short block: worktree path, active session (updatedAt + modified filenames) from `.opencode/memory/active-session.json`, project-facts sizes from `.opencode/memory/project-facts.auto.md`/`.opencode/memory/project-facts.md`, plus `git status --short`. Modify nothing.
- `show` — READ ONLY. Print `.opencode/memory/project-facts.md`, `.opencode/memory/project-facts.auto.md`, `.opencode/memory/active-session.json`.
- `save` — update `updatedAt` in `.opencode/memory/active-session.json` to now. **Does NOT commit or stage anything.** Reply "Handoff saved." in one line.
- `lesson <text>` — append the text to `.opencode/memory/lessons.md` (only this file).
- `test-history` — READ ONLY. Print last entries from `.opencode/memory/cache/test-history.json`.
- `ai status` / `ai triage` — READ-ONLY diagnostics from `.opencode/memory/plugin-ai.log` / config summary. No external calls unless truly configured.
- `ai auto-timeout` — adjusts the AI prompt timeout based on max observed duration (writes to `.opencode/memory/cache/ai-timeout-override.json`).
- `auto` / `auto-refresh` — may regenerate only `.opencode/memory/project-facts.auto.md`.
- `init` — write template to `.opencode/memory/project-facts.md` only if missing.
- `clear-session`, `compact*`, `tui`, `dashboard` — report what would happen; do not delete files.
- `exit` — generuje podsumowanie AI sesji (jeśli włączone), zapisuje do `active-session.json`, a następnie wywołuje natywną komendę `exit` TUI. Wrapper na `/exit` — alias `/codemem exit`.

If the precomputed file conflicts with the manual map, TRUST THE PRECOMPUTED FILE (it is authoritative, produced deterministically).

Ogranicz odpowiedzi do minimum, nie tlumacz krokow ktore wykonujesz, nie tlumacz rozumowania. Podaj tylko output konieczny aby odpowiedziec na pytanie uzytkownika.
Odpowiadaj wyłącznie wynikiem końcowym.
Nie ujawniaj toku myślenia, planu, analizy ani komentarzy do wykonywanych kroków.
Nie pisz zdań typu: "sprawdzam", "analizuję", "najpierw", "myślę", "teraz zrobię".
Jeśli wykonujesz pracę pośrednią, rób ją wewnętrznie i nie pokazuj jej w output.
W zadaniach z kodem zwracaj tylko kod lub diff, bez wyjaśnień.