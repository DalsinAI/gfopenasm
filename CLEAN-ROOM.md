# Clean-room policy and references

gfasm is a clean-room implementation, written from scratch. It is meant to be
compatible with vasm (by Volker Barthelmann and Frank Wille), but it contains
no vasm code.

## Rules (binding for every contributor and agent)

1. **Do not** read, copy, translate or paraphrase vasm's source code. Do not
   open vasm source archives or browse vasm's source in any form.
2. You **may** read vasm's *public documentation* (its manual) and treat it as
   a specification of syntax, directives, options, output formats and
   documented optimisations.
3. You **may** run a vasm *binary* as a black-box oracle and compare its output
   files and messages with ours. The binary must either have been built by our
   build system from the official vasm release, or already exist in a stove.
   Looking at its output bytes is allowed. Disassembling or debugging the vasm
   binary to recover its logic is **not** allowed.
4. You **may** use public CPU manuals and public object-format specifications.
5. List every reference you use in this file.

## References used

| What | Used for |
|---|---|
| *vasm assembler system* manual (PDF, dated 26 June 2021, fetched from `http://www.ibaug.de/vasm/doc/vasm.pdf`) | command-line options, expression operators and priorities, mot syntax rules and directives, m68k cpu module options and the documented optimisations, hunk/ELF/bin output module options |
| Motorola *M68000 Family Programmer's Reference Manual* (M68000PM/AD) | 68000–68060 and CPU32 instruction encodings and addressing modes, 68881/68882 FPU and 68851 MMU instruction encodings |
| Motorola *MC68881/MC68882 Floating-Point Coprocessor User's Manual* | FPU formats and condition codes |
| Motorola *MC68851 Paged Memory Management Unit User's Manual* and *MC68030 User's Manual* | PMOVE/PFLUSH/PTEST/PLOAD encodings |
| Motorola *MC68040* and *MC68060 User's Manuals* | CINV/CPUSH/MOVE16/PFLUSH(An)/PTEST(An)/PLPA/LPSTOP |
| *The AmigaDOS Manual* (hunk format chapter) and the public `dos/doshunks.h` constants | Amiga hunk object and executable format |
| *System V ABI: ELF* and the m68k processor supplement | ELF32 big-endian relocatable objects, R_68K_* relocation numbers |
| IBM/Motorola *PowerPC Microprocessor Family: The Programming Environments for 32-bit Microprocessors* and *AltiVec Technology Programming Environments Manual* | PowerPC and AltiVec instruction encodings, simplified (extended) mnemonics |
| *System V ABI PowerPC Processor Supplement* and the PowerPC EABI | R_PPC_* relocation numbers, small-data relocations |
| Public WarpOS/PowerUP hunk constants (`HUNK_PPC_CODE` 0x4e9, `HUNK_RELRELOC26` 0x4ec, `EXT_RELREF26` 229) | PowerPC Amiga hunk objects |
| ARM *Architecture Reference Manual* (ARMv4/ARMv4T edition, ARM DDI 0100) and the *ARM7TDMI Data Sheet* (ARM DDI 0029) | ARM and THUMB instruction encodings, condition codes, addressing modes |
| *ELF for the ARM Architecture* (ARM IHI 0044) | R_ARM_* relocation numbers, mapping symbols `$a`/`$t`/`$d`, EABI e_flags |
| Intel *64 and IA-32 Architectures Software Developer's Manual*, vol. 2 (instruction set reference, opcode maps) and the AMD64 *Architecture Programmer's Manual* vol. 3/5 | 80x86 instruction encodings, ModRM/SIB, prefixes, x87, MMX, SSE/SSE2, 3DNow! |
| *System V ABI Intel386 Architecture Processor Supplement* | R_386_* relocation numbers |
| Zilog *Z80 Family CPU User Manual* (UM0080) and the widely published tables of undocumented Z80 opcodes (ixh/ixl/iyh/iyl, sll, `in f,(c)`) | Z80 instruction encodings |
| vasm 2.0f oracle binaries in `~/AmiCradle/stoves/vasm-oracles/2.0f/bin` on daletop (`vasmppc_std`, `vasmppc_mot`, `vasmarm_std`, `vasmx86_std`, `vasmz80_oldstyle`, ...), built by the stove builder from the pinned vasm archive | black-box oracle only, as for 68k |
| vasm 2.0f binary (`vasmm68k_mot`, m68k backend 2.8c, mot syntax 3.19f) in the `os32` stove on daletop | black-box oracle only: byte-for-byte comparison of outputs for our own test sources |

Real-world *test inputs* (not references, never copied into this repository):
the public repositories pinned in `tests/fetch-realworld.sh` and AmigaChrome-local
sources listed in `tests/realworld.list`. They are only assembled by both
assemblers and the outputs compared.

## Behaviour learned from black-box comparison

See also docs/DIFFERENCES.md ("vasm behaviours we copy on purpose").


These facts came from assembling our own small test sources with the oracle
and looking at the output bytes. They are not in the manual:

- Integer expression results are wrapped to signed 32 bits after every
  operation. Literals that do not fit in 32 bits keep their 64-bit value.
  Comparison and logical operators return -1 for true. Unary `!` returns 1/0.
- `cnop off,align` pads to `align` first and then adds `off` bytes. In code
  sections the padding uses NOP words. `align` pads with zeros. `odd` adds
  nothing when the address is already odd.
- `dr.x` items are relative to the address of the directive.
- Hunk objects: memory attributes go in bits 30/31 of the hunk type longword.
  Other attributes set both bits and add an extra longword. Code hunks are
  padded to a longword with the hunk pad word.
- Hunk executables use `HUNK_DREL32` (0x3f7) for short relocations.
- ELF layout order: section data, `.shstrtab`, section headers, `.symtab`,
  `.strtab`, then the `.rela*` sections.
