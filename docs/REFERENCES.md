# References

gfopenasm is written from public documentation: CPU manuals, object file
format specifications and the vasm manual. Where these leave something
open (an optimisation, a padding rule, the order of tables in a file),
the behaviour was settled by assembling test sources with vasm and
comparing the output; those cases are listed in
[DIFFERENCES.md](DIFFERENCES.md).

| Document | Used for |
|---|---|
| *vasm assembler system* manual (Volker Barthelmann, Frank Wille; the edition of 26 June 2021) | command-line options, expression operators and priorities, the mot, std and oldstyle syntax modules and their directives, CPU module options and documented optimisations, output module options |
| Motorola *M68000 Family Programmer's Reference Manual* (M68000PM/AD) | 68000-68060 and CPU32 instruction encodings and addressing modes, 68881/68882 FPU and 68851 MMU instructions |
| Motorola *MC68881/MC68882 Floating-Point Coprocessor User's Manual* | FPU data formats and condition codes |
| Motorola *MC68851 Paged Memory Management Unit User's Manual* and *MC68030 User's Manual* | PMOVE, PFLUSH, PTEST and PLOAD encodings |
| Motorola *MC68040* and *MC68060 User's Manuals* | CINV, CPUSH, MOVE16, PFLUSH(An), PTEST(An), PLPA, LPSTOP |
| *The AmigaDOS Manual* (hunk format chapter) and the public `dos/doshunks.h` constants | Amiga hunk object and executable format |
| Public WarpOS/PowerUP hunk constants (`HUNK_PPC_CODE`, `HUNK_RELRELOC26`, `EXT_RELREF26`) | PowerPC hunk objects |
| *System V ABI*, ELF chapters, and the m68k processor supplement | ELF32 relocatable objects, `R_68K_*` relocations |
| IBM/Motorola *PowerPC Microprocessor Family: The Programming Environments for 32-bit Microprocessors* and the *AltiVec Technology Programming Environments Manual* | PowerPC and AltiVec instruction encodings and simplified mnemonics |
| *System V ABI PowerPC Processor Supplement* and the PowerPC EABI | `R_PPC_*` relocations, small-data relocations |
| ARM *Architecture Reference Manual* (ARMv4/ARMv4T, ARM DDI 0100) and the *ARM7TDMI Data Sheet* (ARM DDI 0029) | ARM and THUMB instruction encodings, condition codes, addressing modes |
| *ELF for the ARM Architecture* (ARM IHI 0044) | `R_ARM_*` relocations, the `$a`/`$t`/`$d` mapping symbols, EABI flags |
| Intel *64 and IA-32 Architectures Software Developer's Manual*, volume 2, and the AMD64 *Architecture Programmer's Manual*, volumes 3 and 5 | 80x86 instruction encodings, ModRM/SIB, prefixes, x87, MMX, SSE/SSE2, 3DNow! |
| *System V ABI Intel386 Architecture Processor Supplement* | `R_386_*` relocations |
| Zilog *Z80 Family CPU User Manual* (UM0080) and the widely published tables of undocumented Z80 instructions | Z80 instruction encodings |
| MOS *MCS6500 Microcomputer Family Programming Manual* and the Rockwell/WDC *W65C02S* data sheets | 6502 and 65C02 encodings, including the WDC RMB/SMB/BBR/BBS/STP/WAI instructions |
