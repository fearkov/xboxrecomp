/* CRT routines the lifter miscompiles, written against the host instead.
 *
 * Not translations of the title's code: each is a small, well-known C
 * runtime routine whose lifted form comes out wrong, replaced by a call to
 * the host's own -- which is both correct and faster than interpreting a
 * rep movsd one byte at a time.
 *
 * The lifted copies are still generated, so after regenerating, rename
 * each of these names in its chunk to <name>_miscompiled_unused, or the
 * link fails on a duplicate symbol.
 */

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <string.h>

/**
 * sub_00238BE0 -- the CRT's memcpy/memmove, hand-written.
 *
 * The lifted version is unusable, and quietly so. The routine ends with the
 * classic MSVC tail dispatch for the last 0..3 bytes:
 *
 *     jmp dword ptr [edx*4 + 0x238EC8]
 *
 * and the four-entry table sits immediately after it, at 0x238EC8. Two things
 * then go wrong at once. The disassembler kept decoding past the jump and read
 * the table as instructions, so the tail of the generated function is nonsense
 * -- `mov fs, ...`, `lodsb`, writes through displacements like -1901854685,
 * and two `esp++` that walk the stack pointer. And the jump itself became an
 * indirect call whose targets are labels inside the function rather than
 * functions, so it fails to resolve and execution falls straight into that
 * nonsense.
 *
 * Which makes this the worst kind of bug: every memcpy in the title runs a
 * stack-corrupting wild write on the way out. It is also why the runtime
 * reported unresolved indirect calls to 0x00238E84 and 0x00238F04 -- both are
 * entries in that table, not functions at all.
 *
 * Replacing it rather than patching the lifter, because what the function
 * computes is exactly memmove: the prologue reads dst, src and count, and the
 * body is an overlap check followed by an aligned block copy. cdecl, and it
 * returns dst, like the C library it came from.
 */
void sub_00238BE0(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), (size_t)n);
    eax = dst;
    esp += 4;   /* cdecl: only the return address */
}


/**
 * sub_00238B50 -- the CRT's strlen, hand-written.
 *
 * Same fault as the memcpy above, and found the same way: the generated body
 * contains segment-register moves, which no compiler emits in ordinary code
 * and which only appear when a disassembler has walked off the end of a
 * function into data. There are 35 functions in this title with that
 * signature; these two are the ones every other routine leans on.
 *
 * Identified from its own code rather than by name: one argument, an
 * alignment loop over single bytes, then the classic MSVC word-at-a-time scan
 * with 0x7EFEFEFF. That is strlen and nothing else.
 */
void sub_00238B50(void)
{
    uint32_t p = MEM32(esp + 4);

    eax = (uint32_t)strlen((const char *)XBOX_PTR(p));
    esp += 4;   /* cdecl */
}
