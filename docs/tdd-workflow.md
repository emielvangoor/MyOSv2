# MyOSv2 TDD workflow

MyOSv2 is developed test-first. The loop:

1. **RED** — add a failing test in `src/tests.c` (a new `test_*` function plus an
   entry in the `tests[]` array). Run `make test`; watch it fail.
2. **GREEN** — write the minimum code to make it pass. Run `make test`; watch it pass.
3. **REFACTOR** — clean up with the test still green.

## Running the tests

- `make run` — boots normally; the suite runs first and the kernel HALTS if any
  test fails (so you never demo a broken build).
- `make test` — builds a test kernel, runs the suite in QEMU, and returns a shell
  exit code (0 = all passed). Use this in scripts/CI.

## The commit gate

A pre-commit hook (`.githooks/pre-commit`, enabled with
`git config core.hooksPath .githooks`) runs `make test` and blocks the commit if
anything is red. Do not bypass it with `--no-verify`.

## Adding a test

```c
static void test_my_thing(void)
{
    KASSERT(some_condition);
}
// ...then add it to the tests[] array:
{ "my thing", test_my_thing },
```

New contributors must run `git config core.hooksPath .githooks` once after cloning
to activate the gate.
