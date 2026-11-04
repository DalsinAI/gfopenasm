# Contributing to gfopenasm

Thank you for helping. Bug reports, test cases, fixes and new features
are all welcome.

## Reporting a bug

The best bug report is a small source file that gfasm and vasm assemble
differently. Please include:

- the source (as small as you can make it) and the exact command line;
- the gfasm version (`gfasm` prints it unless you pass `-quiet`) and the
  vasm version you compared with;
- what differs: the output bytes, an error that one of them reports, or
  a crash.

Every difference from vasm's output is treated as a bug, unless it is
listed as intentional in [docs/DIFFERENCES.md](docs/DIFFERENCES.md).

## Pull requests

- Keep each pull request to one subject, and describe what it changes
  and how you tested it.
- `make test` must pass. If you change behaviour, add a test to
  `tests/corpus` (see [tests/README.md](tests/README.md)).
- If you fix a difference from vasm, add the case to the corpus with
  vasm's output as the expectation. If you add a difference on purpose,
  list it in `docs/DIFFERENCES.md`.
- Third-party sources used for testing stay outside the repository: add
  them to `tests/fetch-realworld.sh` (pinned by commit) and to a
  `tests/realworld-*.list` file instead of copying them in.

## Code

- Portable C11, with no dependencies beyond the C standard library and
  `libm`.
- It must build without warnings with `-Wall -Wextra` under gcc and
  clang.
- gfasm runs on many hosts, small ones included, so keep it portable:
  no POSIX-only or Windows-only calls in the core; read and write files
  in binary mode; never depend on the host's byte order (write output
  byte by byte); and do not assume a large stack.
- Follow the style of the file you are editing.

## Continuous integration

[`ci/check.yml`](ci/check.yml) is the check that every pull request has
to pass. It is written in the GitHub Actions workflow format and runs in
an `ubuntu:24.04` container: it builds gfasm with gcc and with clang
with warnings as errors, runs `make test`, and checks that the test
scripts parse. You can run the same steps by hand:

    docker run --rm -v "$PWD":/w -w /w ubuntu:24.04 sh -c '
      apt-get update && apt-get install -y build-essential clang python3 &&
      make CFLAGS="-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror" &&
      make test'

## Licence

gfopenasm is under the MIT licence (see [LICENSE](LICENSE)). By sending
a contribution you agree that it is your own work and that it is
published under the same licence.
