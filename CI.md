# CI

gfopenasm has no GitHub Actions job. New DalsinAI repositories can't use the self-hosted
runners yet, so Team 5 runs CI on demand from daletop instead.

- **The job:** [`ci/check-m68k.yml`](ci/check-m68k.yml). It uses the workflow format and is
  read from the PR head. It builds gfasm with gcc and with clang (`-Werror`), runs `make test`
  (the committed corpus, compared byte for byte with the stored vasm oracle output, so no vasm
  is needed) and syntax-checks the test scripts.
- **Where it runs:** daletop fetches the PR head and rsyncs the tree to amigachromebuild. The
  steps run there in `docker run --rm -v ...:/w -w /w ubuntu:24.04`.
- **The result:** a commit status on the PR head, with context `m68k-daletop`.

## Running it

On daletop, after a PR is opened or pushed to:

```
~/WorkingSpace/AmigaChrome/team5/ci/repo-ci.sh --repo gfopenasm --pr N
```

When `.github/workflows/check-m68k.yml` is missing, `repo-ci.sh` falls back to
`ci/check-m68k.yml`. `--no-status` posts nothing and `--keep` keeps the work dir.
Nothing runs by itself: there is no timer and no service.

The live oracle comparisons need the vasm binaries on daletop, so they are run by hand, not
in this job:

```
VASM=~/AmiCradle/stoves/os32/prefix/bin/vasmm68k_mot sh tests/gen-expected.sh   # regenerate
sh tests/fetch-realworld.sh && VASM=... sh tests/realworld.sh tests/realworld-public.list
```

## Logs

On daletop, in `~/WorkingSpace/AmigaChrome/team5/ci/logs/gfopenasm-pr<N>-<sha7>-<time>/`:
`summary.txt` has every step's result, and `amigachromebuild.log` has the full output.
