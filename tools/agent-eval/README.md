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

**Expect parts** (`e526391`, where each `&&` part of a top-level `expect` is its own deep comparison, and `== []`
is rejected elsewhere; the same 12 feature tasks × 3 runs, compared with the `5973225` runs above):

| model | build | passed | turns | cost | failed expects | `==`-in-`&&` failures (runs hit) |
|---|---|---|---|---|---|---|
| Sonnet | `5973225` | 36/36 | 15.7 | $0.333 | 27 | 20 (11) |
| Sonnet | `e526391` | 36/36 | 14.5 | $0.318 | 12 | 0 (0) |
| Haiku | `5973225` | 36/36 | 19.7 | $0.024 | 26 | 20 (8) |
| Haiku | `e526391` | 36/36 | 17.8 | $0.023 | 5 | 0 (0) |

The pitfall is gone, and turns fell by 1.6 per task on average, more on the tasks it used to hit (AF2: 22 → 16 on
Haiku, 19 → 14 on Sonnet; DF1: 14 → 10 on Sonnet). No run hit the new `[compare]` error.

**Contracts** (`A4c`–`D5c`: seven of the logic-bug controls with the doc comment's rule also written as
`requires`/`ensures`; D4 has none, since `parseTeam` throws on bad input; 3 runs per model; a pass means the planted bug
was fixed):

| build | Sonnet | Haiku | bugs `fut test` reports |
|---|---|---|---|
| `3257a03`, doc comment only | 0/21 | 0/21 | none |
| `3257a03`, with contracts | 6/21 | 2/21 | C4, C5 (property tests) |
| `prop-gen` `c6308aa`, with contracts | 12/21 | 8/21 | also A5 (a `Player` built by its init), B5 (500 g, a number in the function) |

- **A contract helps only when `fut test` reports it.** A clause nothing reported (A4, B4, D5, which need a filled bag,
  cart or project) was fixed in no run.
- **Reported, Sonnet fixed it every time.** Haiku sometimes left a failing test as "an existing failure", and on C5 it
  changed the contract to match the bug in 4 of 6 runs ("the body is already correct").
- **A note on a broken `ensures`** ("the contract states what the code is meant to do: fix the code, unless ...") left
  Sonnet at 12/12. On C5c, Haiku fixed the code in 3 of 6 runs (1 of 6 before), changed the contract in 2, and in 1 refused
  to touch `render.fut` without the user. C5c is a weak test of this: its doc comment says `seen` "remembers how often
  each base came up", which fits the bug (1 after the first) better than the contract's `seen[base] == 0`, and Haiku
  cited it.
