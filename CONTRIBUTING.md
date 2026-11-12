# Contributing

This repository is a research project: three implementations of the
Hood–Melville persistent queue (C, Rust, Zig) and a measured comparison of
them. The git history is part of the research record — every commit says
what was changed, why, and what the measurements showed. Keep it that way.

## Commit messages

Commits follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/)
and are written in English.

```
<type>(<scope>)!: <summary>

<body>

BREAKING CHANGE: <what breaks for users>
Refs: #<issue>

Co-Authored-By: ...
```

**Summary line.** Imperative mood, lowercase after the colon, no trailing
period, about 72 characters at most.

**Types.**

| type | use for |
|---|---|
| `feat` | new API or capability (a new variant, operation, option) |
| `fix` | a bug or a divergence from the specification |
| `perf` | a change made for speed or memory, with measurements |
| `refactor` | a code change with no behavior or performance intent |
| `test` | tests only |
| `bench` | benchmark workloads, measurement scripts and recipes |
| `docs` | README, `docs/`, the research journal, comments |
| `build` | CMake, Cargo, `build.zig`, `justfile` outside benchmarks |
| `ci` | `.github/workflows` |
| `style` | formatting and lints with no code change |
| `chore` | anything else (license, repository housekeeping) |

`bench` is a project-specific type: benchmark infrastructure is a large
part of this repository and is neither a feature nor a test.

**Scopes** (optional, comma-separated if several): the implementation —
`c`, `rust`, `zig`; or the component — `pool`, `sync`, `bench`, `journal`,
`readme`, `proof`. Omit the scope when a change spans the whole project.

**Body.** Explain what changed and why. For `perf` and for anything that
moves numbers, state the measurement: the method (for example, A/B over 4
code layouts, N rounds), the ratios and what was rejected. Reference the
journal section in `docs/benchmarks.md` when there is one.

**Footers.**
- `BREAKING CHANGE:` together with `!` after the type/scope for any change
  of a public API (C headers, the Rust crate, the Zig module), even before
  1.0.
- `Refs: #N` for related issues (several: `Refs: #10, #14`). Do not use
  closing keywords; issues are closed by hand with a summary.
- `Co-Authored-By:` trailers stay last.

Examples:

```
perf(zig): allocate the rear before the pool, inline clone

...

Refs: #1
```

```
feat(c,zig)!: handle refcount overflow idiomatically — saturation in C, illegal behavior in Zig

...

BREAKING CHANGE: C NAME_clone and NAME_iter_init return void.
Refs: #3
```

## History

- `main` is linear: no merge commits.
- Each finished step is one commit with its tests, measurements and the
  journal entry.
- History is not rewritten. The only exception so far was the one-time
  conversion of all commit messages to this convention. That conversion
  kept the content of every message.

## Before committing

Run locally what CI runs. Every CI job calls the same `just` recipes
(`.github/workflows/ci.yml`), so a green local run means a green CI run.

- **Formatting and linters:** `just lint`. It covers every source:
  libraries, tests, benchmark workloads and scripts. `just fmt` applies
  the formatting.

  | language | formatter | linter |
  |---|---|---|
  | C/C++ | clang-format (`.clang-format`) | clang-tidy (`.clang-tidy`); a build of every CMake target with `-Wall -Wextra -Wpedantic -Werror`, including the pooled workloads and the macOS allocator shims in `bench/` |
  | Rust | rustfmt | clippy pedantic on all targets, with and without `pool`, and `bench/snapshot/rd.rs` |
  | Zig | `zig fmt` | `zig ast-check` of every file |
  | Python (`bench/*.py`) | ruff format (`ruff.toml`) | ruff check |
  | Shell, `justfile`, workflow | `just --fmt` | shellcheck, actionlint |

  The versions of clang-format, clang-tidy, ruff, shellcheck and
  actionlint are pinned in the `justfile` and run through `uvx`, so local
  runs and CI use the same tools.
- **Tests:** `just test`. For the implementation you touched, also:
  - C: `just c-asan`, `just c-tsan`, `just c-leaks` (macOS),
    `just c-install-check`. C builds use `-Werror`. CI runs C on Linux
    (GCC) and on macOS (clang).
  - Rust: `just rust-miri`.
- **Performance:** `just bench-quick` against the saved baseline. Any
  claim about speed needs:
  - an A/B over the 4 code layouts (`bench-layouts*`, `bench/compare.py`);
  - an entry in `docs/benchmarks.md`.

  A change to a benchmark workload must not change the measured code;
  compare its machine code, or measure it.
- **Fairness rules** for comparing languages are in `docs/plan.md`:
  - final implementations are idiomatic for their language, even at a
    cost;
  - a non-idiomatic variant may exist only as a labeled demonstration;
  - any difference between the implementations must be justified.

## Language

Code comments and documentation are currently in Russian. Moving them to
English is tracked in #21. New commit messages are English only.
