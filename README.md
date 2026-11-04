# gfopenasm (`gfasm`)

An MIT-licensed, clean-room assembler that accepts the same sources and
command line as vasm, so it can replace vasm as a build tool.

**Status:** Motorola 68k (`mot` syntax), PowerPC (32-bit + AltiVec, `std`
and `mot` syntax), ARM (ARMv2–v4T with THUMB) and 80x86 (32/16-bit, x87,
MMX, SSE; `std` syntax) produce output that is byte-identical to vasm's.
Z80 and 6502 are next; see
[docs/ROADMAP.md](docs/ROADMAP.md).

## Why

vasm is free only for non-commercial use and is not open source. We use it as
a build tool (for example, Team 4's Chocolate Doom port uses `vasmm68k_mot`),
and we need an assembler we can ship and use freely. gfasm is written from
scratch. The rules it is developed under are in [CLEAN-ROOM.md](CLEAN-ROOM.md).

## Build

Portable C11 with no dependencies. It builds on Linux x86-64 and ARM64.

    make            # builds ./gfasm
    make test       # runs the regression corpus (see tests/README.md)

## Usage

    gfasm -cpu=68k -syntax=mot [vasm options] source.s
    gfasm -cpu=ppc -syntax=std [vasm options] source.s
    gfasm -cpu=arm -syntax=std [vasm options] source.s
    gfasm -cpu=x86 -syntax=std [vasm options] source.s

The binary also works under vasm's names. If you install or symlink it as a vasm name
(`vasmm68k_mot`, `vasmppc_std`, `vasmppc_mot`, `vasmarm_std`, `vasmx86_std`), it selects the CPU and syntax
from its own name:

    vasmm68k_mot -Fhunk -o foo.o foo.s
    vasmm68k_mot -Fhunkexe -nosym -o foo foo.s
    vasmm68k_mot -Felf -m68020 -o foo.o foo.s
    vasmm68k_mot -Fbin -o foo.bin foo.s

It supports these vasm options: `-F<fmt>` (`hunk`, `hunkexe`, `elf`, `bin`),
`-o`, `-I`, `-D`, `-L`, `-nosym`, `-quiet`, `-x`, `-w`, `-maxerrors=`,
`-esc`/`-noesc`, `-nocase`, `-pad=`, `-uspc=`, `-unnamed-sections`,
`-ignore-mult-inc`, `-noialign`, the `-m680x0`/`-m68881`/`-m68882`/`-m68851`/`-mcpu32`
CPU options, the ARM options (`-a2`...`-a4t`, `-m2`...`-mstrongarm1100`,
`-big`, `-little`, `-thumb`, `-opt-adr`), the `-opt-*` options, `-no-opt`, `-sc`, `-sd`, `-sdreg=`, `-devpac`
(partial) and `-phxass` (partial). It also takes the mot syntax options
`-spaces`, `-ldots`, `-localu`, `-align`, `-allmp` and `-cnop=`.

## License

MIT. See [LICENSE](LICENSE).
