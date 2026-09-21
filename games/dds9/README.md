# Shin Megami Tensei: Nine — notes, and the parts that are ours

Everything under `src/game/` in a working tree is output: several
generations of translated C, 3.8 GB of it, all reproducible from the XBE in
minutes. None of it is here, and none of it should be.

**Nothing in this directory is derived from the title's code.** The files
are the project's own: an entry point, a build target, two C runtime
routines written against the host, and diagnostic wrappers. What the
recompiler produces from your own copy of the game stays on your machine,
the way the rest of this toolkit already works.

**This branch is not for upstream.** It is game-specific and lives here so
the analysis survives being on one disk.

## What is here

| file | what it is |
|---|---|
| `recomp_crt_replacements.c` | `memcpy` and `memset` written against the host, replacing two the lifter gets wrong |
| `recomp_probes.c` | diagnostic wrappers around functions the recompiler produces |
| `main.c` | entry point and crash reporter |
| `recomp_manual.c` | the manual-override hook |
| `CMakeLists.txt` | the game target |

## Eleven functions the lifter gets wrong

Nine of these are not here, because a lift of them is a translation of the
title. They are worth **95 files opened against 70** — a regeneration
without them stalls early enough that the regeneration itself looked like
the problem for a while. Produce them from your own copy:

    python -m tools.recomp game_files/default.xbe -f <address>

after seeding `functions.json` with the extent, taken as the gap to the
next detected function. The addresses, and why each is missed:

**Reachable only through an indirect call** — the disassembler finds
functions by following call targets, so one whose only callers go through a
pointer is never discovered. Two sit inside XAPI's input code, next to the
device-type structures at `0x0030F234`.

    0x0030FD65 - 0x0030FDCD    0x0031447F - 0x003145D3
    0x00243129 - 0x00243153    0x0025CC70 - 0x0025CEA0
    0x00313B73 - 0x00313BC5

**Reached by tail jump**, each landing in a gap between detected functions,
twice at the exact byte where the previous one was truncated. Found by
counting stub calls at runtime: of 383 unresolved stubs only these four are
ever reached, all during startup.

    0x0016712F - 0x00167177    three calls to 0x00013A07, then ret 8
    0x001B431A - 0x001B433D    an SEH epilogue
    0x001B4DC5 - 0x001B4E3C    a constructor, points an object at 0x005AB028
    0x001C0BA9 - 0x001C0BC4    a continuation back into sub_001C0B32

The two in `recomp_crt_replacements.c` are the other two, and they are here
because they are reimplementations rather than lifts.

After producing them, rename the lifted copy of **each of the eleven** in
its chunk to `<name>_miscompiled_unused` or the link fails on a duplicate:

    sub_0016712F  sub_001B431A  sub_001B4DC5  sub_001C0BA9  sub_00238B50
    sub_00238BE0  sub_00243129  sub_0025CC70  sub_0030FD65  sub_00313B73
    sub_0031447F

## Regenerating

**Steps 1 to 4 of `docs/GETTING_STARTED.md` first.** They produce
`tools/disasm/output`, `tools/func_id/output` and
`tools/abi_analysis/output`, which the command below reads and which are
gitignored because they are derived. Skipping them does not fail loudly:
the recompiler warns about what it cannot find and carries on, and the
result is quietly worse.

    python3 -m tools.recomp game_files/default.xbe --all --split 250 \
        --game-name DDS9 --force-return 0x0015D780=0 \
        --gen-dir src/game/recomp/gen_rcr2

`--split 250` is not optional: at the documented 1000 the largest chunk
reaches 257 MB and the compiler is OOM-killed on a 15 GB machine.

Open pull requests this depends on, none merged at the time of writing:
**#100** `--force-return`, **#104** `rcl`/`rcr`, **#106** the keyboard,
**#107** the interrupt give-up counter, and **#102**, **#103**, **#108**
for the graphics.

    RECOMP_KEYBOARD=1 RECOMP_FB_WINDOW=1 RECOMP_FORCE_RETURN=1 \
    RECOMP_ASYNC_IO=1 RECOMP_PB_EXEC=1 RECOMP_USB=1 \
    RECOMP_USB_NDP=4 RECOMP_USB_PORT=2 \
    RECOMP_AC97_READY=1 RECOMP_DSP_ACK=0x804A8810 \
    wine build/src/game/smt9_recomp.exe

Each is load-bearing; the title reaches a different, earlier dead end
without any one of them.

## Where it gets to, and what is still wrong

Boots to its menu, takes a keypress, and is navigable through character
creation to the name-entry keyboard.

- an intermittent crash at `0xCE841028` in the USB driver's done-queue
  walker, about two runs in ten
- glyphs draw only partially: ten to fifteen of seventy cells, and the set
  changes between frames on a screen that is not moving
