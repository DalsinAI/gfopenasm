# Roadmap

gfopenasm is one assembler binary: a shared core (expressions, symbols,
sections, macros, conditional assembly) with CPU, syntax and output
modules. Run under a vasm name (`vasm<cpu>_<syntax>`), or as
`gfasm -cpu=<cpu> -syntax=<syntax>`, it behaves like the matching vasm
program. Every target is checked byte for byte against vasm's output on
the test corpus and on real-world sources (see
[tests/README.md](../tests/README.md)).

## Done

| Target | vasm equivalent | Outputs |
|---|---|---|
| Motorola 68000-68060, CPU32, 68881/2, 68851; `mot` syntax | `vasmm68k_mot` | hunk, hunkexe, ELF, bin |
| PowerPC 32-bit with AltiVec; `std` and `mot` syntax | `vasmppc_std`, `vasmppc_mot` | ELF, hunk (WarpOS/PowerUP), bin |
| ARM, ARMv2-v4T with THUMB; `std` syntax | `vasmarm_std` | ELF, bin |
| 80x86, 16/32-bit, x87, MMX, SSE/SSE2; `std` syntax | `vasmx86_std` | ELF, bin |
| Z80; `oldstyle` syntax | `vasmz80_oldstyle` | bin |
| 6502, 65C02, WDC 65C02; `oldstyle` syntax | `vasm6502_oldstyle` | bin, C64 PRG, Apple II |

## Next

- Output modules: S-record, Intel hex, `vobj`, a.out, TOS.
- Listing files (`-L`) in vasm's formats.
- Full `-devpac` and `-phxass` compatibility modes.
- 68k: ColdFire and 68080.
- PowerPC: 64-bit instructions (`-mppc64`), POWER/POWER2, 40x/44x/e500.
- x86: x86-64 (`-m64`, `.code64`), SSE3 and later, CPU selection.
- Z80: 8080, Game Boy, HD64180 and Rabbit modes.
- 6502: illegal opcodes, DTV, 65CE02, HuC6280 and MEGA65 modes.
- More CPUs that vasm supports.

The details of what is missing in each target are in
[DIFFERENCES.md](DIFFERENCES.md).
