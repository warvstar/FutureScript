# Writing agent-eval tasks

A task is a request a user might give a coding agent working on one of the projects in `projects/`, plus hidden tests
that decide whether the agent got it right. `run.py` gives the agent a copy of the project, the task text and the fut
reference (`docs/spec.md`), then grades the result with the hidden tests appended to `main.fut`.

## Feature tasks (`"kind": "feature"`)

These measure how agents do on ordinary work, so they must look like ordinary work:

- **Substantial:** about 40–120 lines of new or changed fut. The work should span at least two files or classes,
  use the project's existing classes and functions (constructors, methods, typed params), and add at least one new
  class, method, field or enum variant where that fits the request. Several pieces that must work together beat one
  big function.
- **Written as a user would ask:** what they want and why, the exact names and signatures of anything the hidden
  tests call, the file each goes in, and every rule the hidden tests rely on (rounding, order, ties, empty input,
  errors). Mention nothing about likely mistakes, and give no hints about how to implement it.
- **Ending with:** "Add tests for it. `fut test main.fut` must pass when you finish."
- **Graded only on what the text specifies.** Hidden tests may call only the names the task gives, and check only the
  behavior it states. Any correct implementation must pass.
- **New functions** should be non-`pub` (or in `main.fut`), so the `.api` files don't change. If a task must change a
  `pub` interface, say in the task that `fut api main.fut` updates the `.api` files.

## Files: `tasks/<id>/`

- `task.txt`: the request.
- `hidden.fut`: `test "..." { ... }` blocks, appended to `main.fut` for grading.
- `meta.json`: `{"project": "A", "kind": "feature", "description": "one line"}`.
- `ref.patch`: a correct solution, as `diff -ruN` from the project to the solved tree, with `a/` and `b/` path
  prefixes so that `patch -p1` applies it inside a copy of the project. It includes the tests the task asks for.

Planted-bug tasks (`"kind": "planted"`) also have `bug.patch`, applied before the agent starts, and meta fields
`bug` and `path`. See the existing A1–D5 tasks.

## Validation

`python3 tools/agent-eval/validate.py --old OLD --new NEW [TASK ...]` must print `ok` for each task. For both
compilers it checks that:

- the starting tree's own tests pass;
- the starting tree fails the hidden tests;
- `ref.patch` applies, and the solved tree passes every test, hidden ones included;
- for a planted task, the solved tree minus the fix (the feature alone) still fails.

`--old` and `--new` are built fut checkouts.
