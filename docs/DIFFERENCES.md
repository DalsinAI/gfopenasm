# Known differences from vasm (phase 1, 68k / mot)

gfasm aims to produce the same object bytes as vasm. In the 68k phase, the
committed corpus and every real-world source we test match byte-for-byte (see
tests/README.md). The differences we know about are listed below.

## Intentional or not yet implemented

| Area | gfasm | vasm |
|---|---|---|
| Messages | Same wording for common errors and warnings, but no message numbers, and the set of warnings differs (for example, no "label name conflicts with mnemonic") | numbered errors/warnings |
| Default output format | `bin` | `test` (vasm's debug output module) |
| Listing file (`-L`) | simple address/bytes/source listing | vasm "wide"/"old" formats |
| `-devpac`, `-phxass` | partially implemented: the options are accepted and change the main defaults, but not every Devpac/PhxAss detail | full compatibility modes |
| `-depend`, `-dwarf`, `-linedebug` | accepted and ignored | dependency output, DWARF, SAS/C LINE DEBUG hunks |
| Output modules | hunk, hunkexe, ELF, bin | also a.out, TOS, Xfile, vobj, srec, ihex, ... |
| ColdFire, 68080, CPU32-only extras | not implemented | supported |
| `opt` directive | the vasm-specific `o?` flags and `a` are honoured, Devpac `o1`–`o12` are accepted but not individually switched | full |

## vasm behaviours we copy on purpose

The black-box comparison found some behaviours that the manual does not
describe. gfasm copies them so that the output bytes match:

- `asl Dn` (one operand, which means `#1`) on 68000–68030 is optimised into
  `add Dn,D0` rather than `add Dn,Dn`.
- `bsr` to a label four bytes ahead is kept as `bsr.w`.
- An explicitly short branch to the next instruction (`bra.s *+2`) becomes
  `lea (a6),a6`.
- An unsized branch to the next instruction is deleted. A `jmp` to the next
  instruction is deleted too.
- `B<cc>.L` on 68000/010 is assembled as `B<cc>.W`.
- `add/sub/and/or #imm,Dn` (not the `addi`/`andi`... forms) use the
  `<ea>,Dn` encoding with an immediate source.
- `and #-1` is kept as written, but `and.w #$ffff` becomes `tst`; `eor`
  compares the masked value.
- `movea.? #x,<non-An>` assembles like `move` without moveq optimisation.
- `movem.l (An)+,Am/An` becomes two `movea` instructions.
- FPU: unsized instructions with a memory or immediate source default to
  `.w`. Integer immediates with `.s/.d/.x` are optimised to `.w`/`.l`.
  `#$...`, `#%...` and `#@...` immediates in float formats are raw bit
  patterns. Float immediates with `.b/.w/.l` keep the format but store
  single-precision bits. `fdiv #2^n` becomes `fmul #2^-n`.
- `rs` and `so` share one offset counter. `fo` decrements before it assigns.
- `dcb.s` with a float fill value produces zeros.
- `\@` numbers also count `rept` blocks and include files.
- `__VASM` has the FPU/MMU bits only for an explicit `-m68881/2/-m68851`.
