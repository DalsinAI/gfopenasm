# Known differences from vasm

gfasm aims to produce the same object bytes as vasm 2.0f. The committed
test corpus and every real-world source in the test lists match byte for
byte (see [tests/README.md](../tests/README.md)). The differences that
are known are listed below. Anything else that differs is a bug.

## Intentional or not yet implemented

| Area | gfasm | vasm |
|---|---|---|
| Float expressions | some that vasm rejects are accepted: a shift of a float, a packed (`.p`) infinity; `oldstyle` syntax has no float constants (vasm reads `1.5` as `1` with a warning, gfasm reports an error) | errors |
| Messages | Same wording for common errors and warnings, but no message numbers, and the set of warnings differs (for example, no "label name conflicts with mnemonic") | numbered errors/warnings |
| Default output format | `bin` | `test` (vasm's debug output module) |
| Listing file (`-L`) | simple address/bytes/source listing | vasm "wide"/"old" formats |
| `-devpac`, `-phxass` | partially implemented: the options are accepted and change the main defaults, but not every Devpac/PhxAss detail | full compatibility modes |
| `-depend`, `-dwarf`, `-linedebug` | accepted and ignored | dependency output, DWARF, SAS/C LINE DEBUG hunks |
| Output modules | hunk, hunkexe, ELF, bin | also a.out, TOS, Xfile, vobj, srec, ihex, ... |
| ColdFire, 68080, CPU32-only extras | not implemented | supported |
| `opt` directive | the vasm-specific `o?` flags and `a` are honoured, Devpac `o1`–`o12` are accepted but not individually switched | full |

## General behaviours that gfasm matches

Comparing outputs with vasm showed some behaviours that the vasm manual
does not describe. gfasm does the same, so that the output bytes match:

- Float expressions are evaluated in double precision. A `.x` constant
  is the double's value in extended format (also for infinities, NaNs and
  denormals, whose exponent is rebiased as for any other value). A float
  in byte or word data is an error, except as a `dcb` fill value.
- Integer expression results are wrapped to signed 32 bits after every
  operation. Literals that do not fit in 32 bits keep their 64-bit value.
  Comparison and logical operators return -1 for true; unary `!` returns
  1 or 0.
- Hunk objects: memory attributes go in bits 30/31 of the hunk type
  longword; other attributes set both bits and add an extra longword.
  Code hunks are padded to a longword with the hunk pad word. Hunk
  executables use `HUNK_DREL32` for short relocations.
- ELF files are laid out as section data, `.shstrtab`, section headers,
  `.symtab`, `.strtab`, then the `.rela*` sections.

## 68k and mot syntax behaviours that gfasm matches

- `cnop off,align` pads to `align` first and then adds `off` bytes. In
  code sections the padding uses NOP words; `align` pads with zeros;
  `odd` adds nothing when the address is already odd.
- `dr.x` items are relative to the address of the directive.

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
  `.w`. Integer immediates with `.s/.d/.x` are optimised to `.w`/`.l`, and
  so are float immediates with a whole value that fits in a word (but not
  `-0.0`); other float immediates get the smallest of `.s` and `.d` that
  holds them exactly. `ftst` and `fsincos` keep the size as written.
  `#$...`, `#%...` and `#@...` immediates in float formats are raw bit
  patterns. A float immediate with `.l` stores single-precision bits; with
  `.b`, `.w` or no size it is an error. `fdiv #2^n` becomes
  `fmul #2^-n`.
- `rs` and `so` share one offset counter. `fo` decrements before it assigns.
- `dcb.s` with a float fill value produces zeros.
- `\@` numbers also count `rept` blocks and include files.
- `__VASM` has the FPU/MMU bits only for an explicit `-m68881/2/-m68851`.

## PowerPC behaviours that gfasm matches

- `vmaddfp` takes its operands in the order vD,vA,vB,vC. `vnmsubfp` uses the
  standard vD,vA,vC,vB order.
- `subi rD,rA,label` relocates `+label` but stores `-label` in the
  instruction.
- `.lcomm sym,size,align`: the alignment is in bytes, not bits.
- `.string`/`.asciz` with several strings concatenates them and adds a single
  terminating zero.
- A float constant in `.long` or `.quad` data stores the IEEE single or double
  bits.
- Conditional-branch hints follow the old `y` bit rule: `+` sets it for
  forward branches, `-` sets it for backward ones.
- A `d(r2)` or `d(r13)` operand with a relocatable displacement becomes a
  base-relative (small data) relocation: `HUNK_DREL16`/`EXT_DEXT16` in hunk
  objects.
- std syntax: local labels are digits only (`.1`, `1$`). `$` is the current
  pc. `\@` expands to a plain number.
- `extsw` is accepted without `-mppc64`. Other 64-bit instructions are not.

## PowerPC not implemented yet

- 64-bit instructions (`-mppc64`), POWER/POWER2, 40x/44x/Book-E/e500 extras,
  `-opt-branch`.
- `-little` is accepted, but it is not covered by the corpus.

## ARM behaviours that gfasm matches

`vasmarm_std` 2.0f (ARM backend 0.6) has some surprising behaviours. gfasm
does the same, so that the bytes match:

- Mnemonics are parsed as base + optional condition + optional suffix. When
  the rest does not form a valid suffix it is ignored, so `addseq` is a plain
  `add`, `movs.b` is `mov`, `ldrbeq` is an unconditional word `ldr` and
  `adrleq` is `adrle`. ADRL is written `adr{cond}l` or `adrl`.
- `ldr rd,=expr`: constants that fit use `mov`/`mvn`. Other values go to the
  literal pool, but the load offset is computed as *entry offset within the
  pool* - (pc + 8), so it is only in range in the first 4 KB of a section.
  `ldrh rd,=expr` uses 4-byte pool entries and a base register field of 0.
  A label in the current section of a relocatable object becomes
  `add/sub r0,pc,#...` (the destination register is lost). Pool
  relocations are written in reverse order. Pools are auto-aligned to 4
  (warning) and flushed at `.ltorg` and at the end of the section.
- Immediates that are not an 8-bit rotated value use the complementary
  instruction (`mov`/`mvn`, `and`/`bic`, `add`/`sub`, `cmp`/`cmn`,
  `adc`/`sbc`). `mov`, `mvn`, `add` and `sub` are otherwise split in two
  instructions (`mov`+`orr`, `mvn`+`eor`, `add`+`add`, `sub`+`sub`); the
  second one takes the lowest 8-bit rotated window that leaves an encodable
  rest. With `-opt-adr`, `adr` becomes `adrl` the same way.
- `ldr rd,label` with a halfword/signed size encodes base register 0.
  `[Rn]` without offset sets U=0 for halfword loads and `ldc`/`stc`. The
  `l` (long) suffix of `ldc`/`stc` is accepted but not encoded.
  Post-indexed `ldc`/`stc` do not set the W bit.
- `lsr #0`/`asr #0` encode as `#32`; `ror #0` is `rrx`.
- THUMB: `nop` assembles the 4-byte ARM `mov r0,r0` (needs 4-byte alignment
  and gets an `$a` mapping symbol). `push`/`pop` with any register above r7
  set the lr/pc bit. A conditional branch out of range becomes
  `b<!cc> .+4` + `b label`; for forward targets the second branch is
  computed from the address of the first.
- Instructions are never auto-aligned (error instead). Sections get the
  alignment of their `.align` directives and pools only.
- `$` in data directives is the address of the item (also in the other std
  syntax targets).
- ELF: REL relocations (addends in place), `e_flags` 0x04000000, mapping
  symbols `$a`/`$t`/`$d` (not for `.space` or bss sections), `.comm`
  alignment 4.

## ARM not implemented yet

- The `-a*`/`-m*` CPU options are accepted but do not reject instructions
  that the selected architecture lacks (vasm error 2001).
- `-opt-ldrpc` (two-instruction pc-relative `ldr`/`str`) is accepted but not
  implemented.
- gfasm accepts `.float`, `.double` and `.hword` for ARM; vasm does not.
- Literal pool entries whose value is a forward-referenced equate may be
  ordered differently from vasm's.

## 80x86 behaviours that gfasm matches

`vasmx86_std` 2.0f (AT&T operand order, `%` registers, `$` immediates):

- Without a suffix, the operation size comes from the last register operand;
  other registers are used with their number (`add %al,%ax` is `addw`
  with `%ax`). `shr %cl,(%eax)` is a byte shift. Registers encoded in the
  opcode (`xchg`, `push`, `mov $imm,%reg` short forms) must have that size.
- Immediates: the accumulator and sign-extended forms are chosen like GAS
  (`addl $1,%eax` -> `83`, `addl $128,%eax` -> `05`), but 16-bit forms
  accept 32-bit values and truncate them; the `80`/`c6`/`f6` forms take
  any value. `mov $imm` uses `b8+r` only when the value fits, otherwise
  `c7`. `shl $1` keeps the `c1` form.
- A written displacement is kept, even `0(%eax)` (disp8 0). Large
  displacements with 16-bit addressing are written as 32 bits. A segment
  override equal to the default segment (`%ss:` with `%ebp`/`%esp`) is
  dropped. Prefix order is 67, 66, segment.
- `jmp`/`jcc` use the short form when the displacement of the *long* form
  fits in 8 bits. `jcxz` is assembled as `jecxz`. A jump to an absolute
  address in a relocatable section is relative to the section start.
- `ljmp`/`lcall $seg,$off` (and `jmp`/`call` with two immediates) write the
  selector before the offset.
- Bytes that vasm omits are omitted too: `sfence`/`lfence`/
  `mfence` and `monitor`/`mwait` have no ModRM, `cmpeqps`... have no
  immediate, 3DNow! (`pfadd`...) has no suffix byte. Their memory forms
  put the condition (or suffix & 7) in ModRM.reg and the register in
  ModRM.rm.
- `cbw`, `cwd`, `movsbw`, and `smsw`/`str`/`sldt` with memory have no 0x66
  prefix; `fstsw`/`finit`/`fclex` have no `fwait`. `fadd`, `fsub`... without
  operands are the popping forms (`de c1`...), and `fsubp`/`fdivp` follow the
  AT&T operand swap.
- x87 memory operands take no `s`/`l` size suffix (`fld (%eax)` is single
  precision); `fild`/`fist` use `s`, `l`, `q`/`ll`.
- ELF: REL relocations (R_386_32, PC32, 16, PC16, 8, PC8), `.comm`
  alignment 1, local labels starting with `.` are not in the symbol table,
  `.file` names the FILE symbol, `.type`/`.size` are honoured.
- std syntax (all targets): GNU numeric local labels `1:` with `1b`/`1f`.

## 80x86 not implemented yet

- x86-64: `-m64` and `.code64` are rejected.
- CPU selection options are accepted but do not restrict the instruction set.
- SSE3 and later, and most of 3DNow!, are not in the table.

## oldstyle syntax and Z80 behaviours that gfasm matches

`vasmz80_oldstyle` 2.0f:

- Escape sequences in strings are off by default (a backslash-r in a string
  is two bytes), but a character constant of two backslashes is one
  backslash.
- A leading `0` makes a number octal and stops at the first digit 8/9
  (`08` is 0). `#` and `&` are hex prefixes on the Z80; `@` is binary there.
- `defp` writes 16 bits, not 24. `db`/`dw` without an operand write one
  zero element.
- `org` inside an absolute section pads up to the new address (it does not
  start a new section unless the address goes backwards).
- With `-dotdir` the dot before a directive is optional.
- `name:` defines a label in any column.
- Z80: `(c)` in `ld` means register `c`, `ld (hl),(hl)` assembles to `halt`,
  index prefixes and `h`/`l` mix freely, CB-prefixed operations on
  `ixh`/`ixl` have no displacement byte (`dd cb op`), `adc`/`sbc ix,rr` keep
  the index prefix before `ed`, and `in f,(c)` is accepted.

## oldstyle / Z80 not implemented yet

- `rorg`/`rend`/`phase`/`dephase`, `struct`, `dsect`/`dend`.
- The `-8080`, `-gbz80`, `-hd64180`, `-intel-syntax` and Rabbit (`-rcm*`)
  modes are rejected.
- Output formats `ihex`, `srec` and `vobj`. Use `-Fbin`.

## 6502 behaviours that gfasm matches

`vasm6502_oldstyle` 2.0f:

- Absolute operands become zero page when the value is known to fit
  (also for forward references); `<` forces zero page, `>`, `!` and `|`
  force absolute. In branches `!` is the logical-not operator.
- `blt` and `bge` are accepted as `bcc` and `bcs`.
- `rmb` is the WDC instruction, not the `blk` directive.
- oldstyle: `fill count,value` takes a fill value; anonymous labels `:` with
  `:+`/`:-`; a label on an `if`/`else`/`endif` line is defined; `.if(expr)`;
  `label :`; `*=` may be indented; `dsect`/`dend` are supported; a character
  constant with a backslash followed by any character is a backslash
  when escapes are off; `-esc` turns escapes on.
- bin output: `-cbm-prg` and `-apple-bin` headers.

## 6502 not implemented yet

- `-illegal`, `-dtv`, `-ce02`, `-6280` and `-mega65` are rejected;
  `-opt-branch` is accepted but does not rewrite branches; `ezp`, `zpage`,
  `setdp` and `zero` are missing.
- The main file of nickgammon/G-Pascal (`gpascal.asm`) still differs from
  vasm's output; its other sources match.
- `-atari-com`, `-coco-ml`, `-dragon-bin` and `-oric-mc` headers.
