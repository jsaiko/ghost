# Contributing

Start with [ARCHITECTURE.md](ARCHITECTURE.md) for how the pieces fit and
the rules the design depends on, and [TODO.md](TODO.md) for known gaps.

## Build and test

Prerequisites and the build commands are in the
[README](README.md#build-and-test).

- `make test` runs ctest (libgdp, wraith and spectre) and `cargo test
  --workspace` runs the Rust side, from `host/`.
- CI builds everything on Ubuntu 26.04, plainly and under
  `-DGHOST_ENABLE_ASAN=ON` (AddressSanitizer and UBSan), and runs
  libgdp's libFuzzer targets. Run the sanitizer build locally for changes
  to parsers or buffer handling.
- A change to a daemon needs a real run. Unit tests don't cover login,
  session start or capture; [wraith's README](host/wraith/README.md#run)
  and [spectre's](client/spectre/README.md) have recipes that need no
  ghostd.

## Style

- C++ is formatted by `.clang-format` (clang-format 21: tabs, attached
  braces, one-tab continuations); run `clang-format -i` on what you change.
  A table laid out by hand goes between `// clang-format off` and
  `// clang-format on`. Rust is formatted by `cargo fmt`.
<!-- REUSE-IgnoreStart -->
- A new file starts with an SPDX header: `SPDX-FileCopyrightText: 2026
  Joseph Saiko <https://saiko.dev>` and
  `SPDX-License-Identifier: GPL-3.0-only`. libgdp and `docs/spec/` are
  MIT. Files that can't carry a header are covered by `REUSE.toml`; check
  with `reuse lint`.
<!-- REUSE-IgnoreEnd -->
- No compatibility shims for old wire formats or config: ghost has not
  been released, so change every end together.

## Comments

A comment says what the code does or why it must be that way, in as few
words as that takes. Don't record how a bug was found or what was tried
first; that belongs in the commit message. Where a decision needs more
room, write it up in [docs/design/](docs/design/) or an
[ADR](docs/adr/README.md) and cite it: `docs/design/<file>.md#<heading>`,
or `gdp-spec.md §N`. Cite headings and section numbers that exist; a
renamed heading breaks every comment that points at it.

## Documentation

| Where | What goes there |
|---|---|
| `docs/install/` | Steps to do something on a host. |
| `docs/reference/` | Lookup tables: settings, flags, paths. |
| `docs/design/` | How an area works and why, ending in its Limitations. |
| `docs/adr/` | One decision each: context, decision, rejected alternatives. Numbered in sequence and not rewritten; supersede an old one with a new one. |
| `docs/spec/` | The GDP wire protocol. Changing the wire changes this. |
| `TODO.md` | Open gaps and unverified paths. Accepted design limits go in the design doc's Limitations instead. |

Update the doc in the same change as the code it describes.

## Commits

Small, one topic each, with a message that says why. Work lands on
`main`.
