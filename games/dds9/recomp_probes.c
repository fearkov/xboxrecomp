/* Diagnostic wrappers. Original code: each declares a function the
 * recompiler produced and calls it with a print around the entry, to ask
 * things the runtime cannot answer from outside -- what a function was
 * handed, how often it runs, whether an argument is sane.
 *
 * The functions they wrap come from your own copy of the title. Nothing of
 * the title is reproduced here.
 */

#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- interception through the dispatch table ---------------------------
 * A function reached by indirect call can be watched without touching the
 * chunk that defines it: the dispatch table is what the call goes through,
 * so pointing its entry at a wrapper costs one small file's rebuild instead
 * of a five-megabyte one. Used to find where a garbage `this` comes from.
 */
void sub_00313A1D(void);
void sub_00313B73(void);

/* A guest pointer that could plausibly be an object: inside RAM or the heap,
 * and not in the image or an aperture. Anything else is a bug worth naming
 * rather than following. */
static int probe_plausible(uint32_t p)
{
    return p >= 0x00010000u && p < 0x04000000u;
}

static void probe_bad(const char *tag, uint32_t a, uint32_t b)
{
    static int n;
    if (n++ < 12) {
        fprintf(stderr, "[PROBE] %s BAD this=%08X extra=%08X\n", tag, a, b);
        fflush(stderr);
    }
}

void probe_sub_00313A1D(void)
{
    uint32_t arg = MEM32(g_esp + 4);
    if (!probe_plausible(arg))
        probe_bad("00313A1D", arg, g_ecx);
    else
        recomp_probe("enter00313A1D", g_ecx, arg);
    sub_00313A1D();
}

void probe_sub_00313B73(void)
{
    if (!probe_plausible(g_ecx))
        probe_bad("00313B73", g_ecx, 0);
    else
        recomp_probe("enter00313B73", g_ecx, MEM32(g_ecx + 0x66));
    sub_00313B73();
}

/* DSOUND's mixer, reached by indirect call. The crash under investigation has
 * it as the last title address in the indirect-call history, with ecx=640,
 * edx=16 and ebx=8 in the registers -- a mix loop's shape. */
void sub_002A9430(void);

void probe_sub_002A9430(void)
{
    static int n;
    if (n++ < 8) {
        fprintf(stderr, "[PROBE] enter002A9430 ecx=%08X esp=%08X "
                "a0=%08X a1=%08X a2=%08X\n", g_ecx, g_esp,
                MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12));
        fflush(stderr);
    }
    sub_002A9430();
}

/* The XMV decoder's MMX row loop. Its last argument is the row count, and a
 * runaway value there walks the destination out of RAM. */
void sub_002C4C27(void);

void probe_sub_002C4C27(void)
{
    static int n;
    uint32_t rows = MEM32(g_esp + 0x1C);   /* arg6, past the return address */
    if (rows > 4096u || n++ < 6) {
        fprintf(stderr, "[PROBE] 002C4C27 rows=%u dst=%08X src=%08X pitch=%u\n",
                rows, MEM32(g_esp + 0xC), MEM32(g_esp + 4),
                MEM32(g_esp + 8));
        fflush(stderr);
    }
    sub_002C4C27();
}
