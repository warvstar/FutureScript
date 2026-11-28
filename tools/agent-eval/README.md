# agent-eval

Measures how a change to fut (a compile-time check, an error message, a hint) affects coding agents. The same tasks
run against two builds of fut, an old and a new one, and the runs are graded with hidden tests.

```
python3 build.py                               # in each checkout being compared
python3 tools/agent-eval/validate.py --old ../fut-old --new .
python3 tools/agent-eval/run.py --old ../fut-old --new . --models sonnet,haiku --reps 3 'AF*'
python3 tools/agent-eval/report.py build/agent-eval
```

Each run is a headless Claude Code agent (`claude -p`). It starts with a copy of the task's project, that compiler's
`docs/spec.md` as `SPEC.md`, its `fut` on `PATH`, and only fut, ls and cat allowed in the shell. Your settings, hooks
and MCP servers are left out. `run.py` reuses finished runs in `--out`, so rerunning it resumes. A run costs about
$0.20–0.40 on Sonnet and $0.03 on Haiku.

## Tasks

- `projects/A`–`D`: four small programs of about 500 lines each: an RPG engine, a shop backend, a Markdown processor
  and a task scheduler.
- `tasks/<id>/`: a request (`task.txt`), hidden tests (`hidden.fut`), a reference solution (`ref.patch`) and
  `meta.json`.
  - **Planted** tasks (`A1`–`D5`) also plant one bug with `bug.patch`, which the request never mentions. They measure
    whether agents end up fixing a latent bug. Twelve bugs are of kinds a compile-time check reports; eight are logic
    bugs that no check can catch (the controls).
  - **Feature** tasks (`AF1`–`DF3`) start from the clean project and ask for a larger feature. They measure what
    agents run into on ordinary work.

`AUTHORING.md` describes how to write a task; `validate.py` checks one.

## What a run records (`result.json`)

- **Outcome:** passed (every test passes with the hidden tests appended and `fut test` exits 0, so a stale `.api`
  fails), turns, cost and seconds.
- **`seen`:** every distinct error the agent's own `fut` commands printed, with its code, split into compile time and
  run time.
- **`latent`:** what the new compiler reports in the agent's final code.

## Caveats

- **Tasks written against the checks:** the planted bugs were chosen so that a check reports them, which shows what
  happens once such a bug exists, not how often one appears. The feature tasks were written without the check list.
- **Small samples:** reps of one task aren't independent, so read results per task (`report.py --by-task`).

## Results so far

**Planted bugs** (Sonnet 5.5, 20 tasks × 3 runs per compiler, before vs after the compile-time checks of `c0ee5ea`):

| planted bug | fixed, old compiler | fixed, new compiler |
|---|---|---|
| checkable, in code the task doesn't touch | 0/24 | 24/24 |
| checkable, in code the task uses | 9/12 | 12/12 |
| logic bug (control) | 0/24 | 0/24 |

Every failure was the feature done right with the planted bug left in. Agents fix what a tool reports and don't
audit code outside the task.

**Feature tasks** (`207b357` vs `5973225`, Sonnet and Haiku 5.5, 12 tasks × 3 runs, 144 runs, $26):

- **Pass rate:** all 144 runs passed. Turns and cost were the same on both compilers.
- **Errors a compile-time check reports were rare:** Sonnet met one in 4 of 72 runs (`l.has`, `s.lower()`, a nil
  result), Haiku in 5 of 72. Most of Haiku's were `var gold: num` in a class with no initial value.
- **Agents' own failed tests were mostly one pitfall:** 85 of 112 failed `expect`s were a list or map compared with
  `==` inside `&&`. There `==` is identity, so a correct result fails with only "... is false". It hit 42 of 144 runs
  and cost about 4 turns each, compared within the same task.
- **Other recurring errors:** Haiku's brace mistakes that leave a method outside its class (`'self' outside a
  method`, `'init' is already defined`), Sonnet's default arguments, and `s.slice(a)` with one argument.
