# Tests

gfasm is tested against vasm 2.0f in two ways: a committed corpus that
needs no vasm, and real-world sources that are compared with a live vasm.

## Committed corpus (`make test`)

- `corpus/*.s` are the test sources. The `; gfasm-args:` line in each
  file gives its options (default `-Fhunk`), and a `; gfasm-cpu:` line
  gives the CPU and syntax (default `m68k mot`).
- `expected/<name>.out` holds the bytes that vasm 2.0f produced for the
  same source and options. `expected/<name>.fail` means vasm rejected the
  source, so gfasm must reject it too.
- `run.sh` assembles everything and compares the output byte for byte.
  Each source is assembled three times: from its own directory, by a
  relative path and by its absolute path. It does not need vasm.

How the corpus files are made:

| Files | Made by | Covers |
|---|---|---|
| `m000_*`, `m020_*`, `m030_*`, `m040_*`, `m060_*`, `m851_*`, `m881_*` | `gen/gen_insn.py` | 68k instructions: mnemonic x size x addressing mode |
| `d_*` | `gen/mk_directives.sh` | mot syntax directives, macros, sections and the output formats |
| `ppc_insn*` | `gen/gen_ppc.py` | PowerPC and AltiVec instructions, simplified mnemonics |
| `p_*` | `gen/mk_ppc.sh` | std/mot syntax on PowerPC; ELF, hunk and bin output |
| `arm_insn*`, `arm_lit0`, `arm_thumb0` | `gen/gen_arm.py` | ARM and THUMB instructions, literal pools |
| `a_*` | `gen/mk_arm.sh` | ARM directives, ELF, bin, big-endian and THUMB output |
| `x86_insn*` | `gen/gen_x86.py` | 80x86 integer, system, x87, MMX and SSE instructions |
| `x_*` | `gen/mk_x86.sh` | x86 directives, ELF, bin and `.code16` |
| `z80_insn0`, `o_z80_dirs` | `gen/gen_z80.py`, by hand | Z80 instructions, oldstyle directives |
| `m6502_*`, `o_6502_dirs` | `gen/gen_6502.py`, by hand | 6502 and WDC 65C02 instructions, oldstyle features, `-cbm-prg` |

The `gen_*.py` generators build candidate lines and keep only the ones
that vasm accepts, so the corpus holds valid code only. The generated
files are committed; you only need to run the generators again when you
change their candidate lists.

To make the expectations again you need the vasm binaries
(`vasmm68k_mot`, `vasmppc_std`, ... built from the vasm release) in one
directory:

    VASM_DIR=/path/to/vasm/bin sh tests/gen-expected.sh [name...]

To find the lines of an instruction file that differ from vasm:

    python3 tests/linecmp.py ./gfasm $VASM_DIR/vasmm68k_mot tests/corpus/m000_two.s -Fhunk

## Real-world sources

These are third-party sources from public repositories. They are test
input only and are never copied into this repository.
`fetch-realworld.sh` clones each one at a pinned commit into `tests/rw`
(or `$RW`):

    sh tests/fetch-realworld.sh

Each `realworld-*.list` file has one test per line,
`<options> | <source>`, where `@RW@` stands for the clone directory and
`<dir>//<path>` means that the source is assembled from `<dir>`.
`realworld.sh` assembles each source with vasm and with gfasm and
compares the outputs. Run it with gfasm under the matching vasm name:

    mkdir -p names && for n in vasmm68k_mot vasmppc_std vasmarm_std vasmx86_std vasmz80_oldstyle vasm6502_oldstyle; do ln -sf ../gfasm names/$n; done
    GFASM=names/vasmppc_std VASM=$VASM_DIR/vasmppc_std sh tests/realworld.sh tests/realworld-ppc.list

The 68k list also needs the assembler include files (`Include_I`) of an
AmigaOS NDK, which is not included here; set `NDK` to that directory:

    NDK=/path/to/NDK/Include_I GFASM=names/vasmm68k_mot VASM=$VASM_DIR/vasmm68k_mot sh tests/realworld.sh tests/realworld-m68k.list

`rw_discover.py` makes a list from a directory tree: for each file it
keeps the first set of options that vasm accepts.

## Results (vasm 2.0f)

| Set | Result |
|---|---|
| 68k corpus | 30/30 files match (about 18,000 instruction lines plus 16 directive and output tests) |
| 68k real-world (FPGAArcade amiga_code, KONEY, StefanKubsch AmigaCoding, Hamazing, hukkax, amiga-boing, bumbershoot, rosetta-pi-spigot, amiga_asmdev_workflow, amiga-font-luett) | 93/93 match |
| PowerPC corpus | 10/10 files match (about 7,400 instruction lines with AltiVec and simplified mnemonics, plus std/mot, ELF, hunk and bin tests) |
| PowerPC real-world (SonnetAmiga WarpOS sources, EEC `ppcifuncs.asm`) | 4/4 match |
| ARM corpus | 10/10 files match (about 2,300 ARM lines with all condition codes and suffixes, 340 THUMB lines, literal pools, plus ELF, bin, big-endian and THUMB tests) |
| ARM real-world (Archimedes: kieranhj proto-arc, Mark Moxon's Lander; GBA Frogger; an NDS template) | 5/5 match |
| x86 corpus | 8/8 files match (about 7,600 instruction lines: integer, system, x87, MMX, SSE/SSE2, jumps; plus ELF, bin, `.code16` and macro tests) |
| x86 real-world (robfinch rf80386: compiler-generated i386 code and hand-written CPU test macros) | 17/17 match |
| Z80 corpus | 2/2 files match (about 1,600 instruction lines with the undocumented forms, plus an oldstyle directive test) |
| Z80 real-world (13 repositories: ChibiAkumas GrimeZ80, MSX, CPC, TRS-80, a Z180 BIOS, ZOS, MECB, an arcade sound test...) | 88/88 match |
| 6502 corpus | 3/3 files match (about 1,200 instruction lines for 6502 and WDC 65C02, plus a directive and PRG test) |
| 6502 real-world (13 repositories: Ben Eater style computers, GameTank, X16 tests, SMON, BeebEater, blake3, Primes...) | 63/63 match |
