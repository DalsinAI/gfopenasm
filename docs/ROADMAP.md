# gfasm roadmap

The goal is one assembler binary with a shared core plus CPU, syntax and
output modules. Run under a vasm name (`vasm<cpu>_<syntax>`), or as
`gfasm -cpu=<cpu> -syntax=<syntax>`, it should behave like the matching vasm
build. Every phase is checked byte-for-byte against the black-box vasm oracle
on a test corpus (see CLEAN-ROOM.md).

| Phase | Target | vasm equivalent | Outputs | Estimate | Order rationale |
|---|---|---|---|---|---|
| 1 | Motorola 68000–68060, CPU32, 68881/2, 68851; `mot` syntax | `vasmm68k_mot` | hunk, hunkexe, ELF, bin | done (corpus + real-world pass) | our Amiga build tools use it today |
| 2 | PowerPC (32-bit), `mot` and `std` syntax | `vasmppc_mot`, `vasmppc_std` | ELF, hunk (WarpOS/PPC hunks), bin | done for 32-bit PPC + AltiVec | AmigaOS 4 / WarpOS / MorphOS targets; the `std` syntax module is reused by ARM and x86 |
| 3 | ARM (A32/Thumb, ARMv2–v4T, as in vasm) | `vasmarm_std` | ELF, bin | done (corpus + real-world pass) | Archimedes/GBA/NDS sources; Raspberry Pi bring-up stubs |
| 4 | x86 (i386–PentiumPro, x87, MMX, SSE/SSE2; x86-64 still to do) | `vasmx86_std` | ELF, bin | done for 32/16-bit (corpus + real-world pass) | host-side helper code |
| 5 | Z80 (plus 8080/GBZ80 variants) | `vasmz80_oldstyle` | bin, ihex, srec | ~1 agent-day | small ISA, needs the `oldstyle` syntax module |
| 6 | 6502 (plus 65C02/65816 options) | `vasm6502_oldstyle` | bin (+ C64 PRG/Atari COM headers) | ~1 agent-day | reuses `oldstyle` from phase 5 |

Shared work that the phases pick up along the way:

- Syntax modules: `mot` (phase 1), `std` (phase 2), `oldstyle` (phase 5).
- Output modules: `srec`, `ihex` and `vobj` (phase 5). Listing-file parity is
  a nice-to-have.
- CI: a GitHub Actions workflow on `[self-hosted, amicradle-validate]` once
  new repos are allowed to use that runner. Until then, run `make test`
  locally on daletop.

## Phase 1 checklist

- [x] core: expressions, symbols, sections, macros, REPT/IRP, IF*, include/incbin
- [x] mot syntax directives (manual chapter 4)
- [x] 68k encoder: integer ISA 68000–68060, FPU, MMU, 040/060 extras
- [x] vasm default optimisations (manual chapter 19.6)
- [x] outputs: hunk, hunkexe, ELF, bin
- [x] corpus passes byte-for-byte against the oracle (30/30)
- [x] real-world sources pass (103/103: AmiWind engine asm, the SDL2 c2p used by Chocolate Doom, 10 public demo/tool repos)
- [ ] listing file (`-L`), with format parity as a stretch goal
- [ ] `-devpac`/`-phxass` compatibility modes (partial so far)
