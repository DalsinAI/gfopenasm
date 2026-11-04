# gfopenasm (`gfasm`)

gfopenasm is a free, open-source, multi-target assembler under the MIT
licence. It assembles for Motorola 68k, PowerPC, ARM, x86, Z80 and
6502, and it is compatible with vasm: it takes the same command-line
options and the same source syntax, and it writes the same object files,
byte for byte.

It is one small program in portable C11 with no dependencies. Ready-made
binaries are there for Linux, Windows, AmigaOS 3.x (68k), AmigaOS 4
(PowerPC) and AROS (x86-64), and it builds anywhere a C compiler runs.
It writes the same bytes on every host: float constants are computed in
software, so even the FPU and the C library of the host make no
difference.

## CPUs, syntaxes and output formats

| CPU | Syntax | Run as (vasm name) | Output formats |
|---|---|---|---|
| Motorola 68000-68060, CPU32, 68881/68882 FPU, 68851 MMU | `mot` | `vasmm68k_mot` | Amiga hunk object (`-Fhunk`) and executable (`-Fhunkexe`), ELF, raw binary |
| PowerPC, 32-bit, with AltiVec | `std`, `mot` | `vasmppc_std`, `vasmppc_mot` | ELF, hunk (WarpOS/PowerUP PPC hunks), raw binary |
| ARM, ARMv2-ARMv4T, with THUMB | `std` | `vasmarm_std` | ELF, raw binary |
| 80x86, 16/32-bit, x87, MMX, SSE/SSE2 | `std` | `vasmx86_std` | ELF (i386), raw binary |
| Zilog Z80 | `oldstyle` | `vasmz80_oldstyle` | raw binary |
| MOS 6502, 65C02, WDC 65C02 | `oldstyle` | `vasm6502_oldstyle` | raw binary, with C64 PRG and Apple II headers |

What is not there yet (x86-64, ColdFire, a.out, S-record and Intel hex
output, and so on) is listed in [docs/DIFFERENCES.md](docs/DIFFERENCES.md)
and [docs/ROADMAP.md](docs/ROADMAP.md).

## Compatible with vasm

[vasm](http://sun.hasenbraten.de/vasm/) is a portable multi-CPU assembler
by Volker Barthelmann and Frank Wille. It is a separate project with its
own licence. gfopenasm is not part of vasm and is not affiliated with its
authors; it is written to be compatible with it, so that projects that
build with vasm can build with gfopenasm too.

- **Command line.** gfasm accepts vasm's options. Run it as
  `gfasm -cpu=<cpu> -syntax=<syntax>`, or install or link it under a vasm
  name and it picks the CPU and syntax from its own name, so existing
  makefiles work unchanged.
- **Syntax modules.** The `mot` (Motorola/Devpac style), `std` (GNU as
  style) and `oldstyle` (8-bit) syntaxes, with their directives, macros,
  conditional assembly and local labels.
- **Output.** The same bytes as vasm 2.0f, including vasm's default
  optimisations, section layout, symbol tables and relocations.

### How compatibility is tested

- **Test corpus** (`make test`): 67 source files in `tests/corpus`, about
  40,000 lines covering all six CPUs plus directive, macro and output
  format tests. The expected output of each file was made with vasm 2.0f
  and is committed in `tests/expected`, so `make test` needs no vasm.
  gfasm must produce the same bytes, or fail where vasm fails.
- **Real-world sources**: 270 files from 43 public repositories (Amiga
  demos and tools, WarpOS code, Archimedes and GBA games, i386 code, Z80
  and 6502 home-computer projects) are assembled by gfasm and by vasm
  2.0f, and the outputs are compared. All 270 are byte-identical. The
  repositories are pinned by commit in `tests/fetch-realworld.sh`; they
  are only test input and are not part of this repository.

- **Every platform**: each release binary runs the corpus on its own
  system (or under qemu or wine) before it ships, and the real-world set
  too where the host can run it; see [Download](#download).

See [tests/README.md](tests/README.md) for how to run both. The
differences that remain are listed in
[docs/DIFFERENCES.md](docs/DIFFERENCES.md). Anything else that differs
from vasm is a bug: please report it.

## Download

Release binaries are on the
[releases page](https://github.com/DalsinAI/gfopenasm/releases), with a
`SHA256SUMS` file. Each archive holds `gfasm`, the licence, this README
and an `INSTALL.txt` for its system.

| System | File | Tested |
|---|---|---|
| Linux x86-64 (static, any distribution) | `gfopenasm-<version>-linux-x86_64.tar.xz` | corpus and real-world sources |
| Linux arm64 (static) | `gfopenasm-<version>-linux-arm64.tar.xz` | corpus and real-world sources, under qemu |
| Windows x86-64 | `gfopenasm-<version>-windows-x86_64.zip` | corpus (also with Windows paths) and real-world sources, under wine |
| AmigaOS 3.x, 68000 or better | `gfopenasm-<version>-amigaos-m68k.lha` | corpus and real-world sources on AmigaOS 3.2 (an emulated A1200), with a 4 KB shell stack |
| AmigaOS 4.1, PowerPC | `gfopenasm-<version>-amigaos4-ppc.lha` | not run on AmigaOS 4 yet (see below) |
| AROS x86-64 (ABI v11) | `gfopenasm-<version>-aros-x86_64.lha` | built on AROS with its own gcc; corpus and real-world sources on AROS 2026.09 (Linux-hosted) |

The AmigaOS 4 build is made with the AmigaOS 4 SDK's gcc and newlib, but
we have no AmigaOS 4 system to run it on yet. The same source passes the
corpus and the real-world sources on big-endian PowerPC Linux, so it is
expected to work; reports are welcome.

There is no macOS or MorphOS binary, because we cannot test one; build
from source there (below). On any other system, build from source too: it
takes a few seconds.

## Usage

    gfasm -cpu=68k  -syntax=mot      [options] source.s
    gfasm -cpu=ppc  -syntax=std      [options] source.s
    gfasm -cpu=arm  -syntax=std      [options] source.s
    gfasm -cpu=x86  -syntax=std      [options] source.s
    gfasm -cpu=z80  -syntax=oldstyle [options] source.s
    gfasm -cpu=6502 -syntax=oldstyle [options] source.s

Or under a vasm name (a copy, a symlink or a hard link):

    vasmm68k_mot -Fhunk -o foo.o foo.s
    vasmm68k_mot -Fhunkexe -nosym -o foo foo.s
    vasmm68k_mot -Felf -m68020 -o foo.o foo.s
    vasmppc_std -Felf -o bar.o bar.s

Supported options: `-F<fmt>` (`hunk`, `hunkexe`, `elf`, `bin`), `-o`,
`-I`, `-D`, `-L`, `-nosym`, `-quiet`, `-x`, `-w`, `-maxerrors=`,
`-esc`/`-noesc`, `-nocase`, `-pad=`, `-uspc=`, `-unnamed-sections`,
`-ignore-mult-inc`, `-noialign`, `-no-opt`, the `-opt-*` options, `-sc`,
`-sd`, `-sdreg=`, `-devpac` and `-phxass` (partial); the 68k CPU options
`-m68000`...`-m68060`, `-m68020up`, `-mcpu32`, `-m68881`, `-m68882` and
`-m68851`; the mot syntax options `-spaces`, `-ldots`, `-localu`,
`-align`, `-allmp` and `-cnop=`; and the ARM options `-a2`...`-a4t`,
`-m2`...`-mstrongarm1100`, `-big`, `-little`, `-thumb` and `-opt-adr`.
The vasm manual describes what each option does.

## Build from source

You need a C11 compiler and `make`; there are no other dependencies.

    make            # builds ./gfasm
    make test       # assembles the test corpus and compares the output

Then copy `gfasm` to a directory on your `PATH`, and add links under the
vasm names you need. This works on Linux and the BSDs; on macOS (with the
Xcode command line tools) it should work the same way, but we have not
tested it.

The release binaries are cross-compiled with the scripts in `ci/`:

    sh ci/build.sh TARGET OUTDIR     # one target, e.g. windows-x86_64
    sh ci/test.sh TARGET OUTDIR      # its corpus run (under qemu or wine)
    sh ci/package.sh VERSION TARGET OUTDIR DISTDIR

| Target | Compiler |
|---|---|
| `linux-x86_64`, `linux-arm64` | gcc, `aarch64-linux-gnu-gcc` (static) |
| `windows-x86_64` | `x86_64-w64-mingw32-gcc` (mingw-w64) |
| `amigaos-m68k` | `m68k-amigaos-gcc` with libnix (for example from [amiga-gcc](https://github.com/bebbo/amiga-gcc)); only the C library's headers are needed, no NDK |
| `amigaos4-ppc` | `ppc-amigaos-gcc` with the AmigaOS 4 SDK (for example from [adtools](https://github.com/sba1/adtools)) |
| `aros-x86_64` | built on AROS itself: `sh ci/aros-hosted.sh OUTDIR` boots Linux-hosted AROS with its SDK under Xvfb, compiles there and runs the corpus |

For MorphOS, `ppc-morphos-gcc` from the MorphOS SDK should build it with
`make CC=ppc-morphos-gcc`; we have not tried it.

Two builds of the same source with the same compiler give the same bytes
(the release builds are made twice and compared).

## Licence

gfopenasm is under the [MIT licence](LICENSE). In short: you can use it
for anything, commercial or not; you can ship it with your own SDK,
toolchain, game or operating system; you can embed its source in an open
or a closed project; and you can change it. Keep the copyright notice
and the licence text with it, and that is all. There is no warranty.

## Contributing

Bug reports, test cases and pull requests are welcome. A source file
that vasm and gfasm assemble differently is the most useful bug report
there is. See [CONTRIBUTING.md](CONTRIBUTING.md).
