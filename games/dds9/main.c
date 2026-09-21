/**
 * Shin Megami Tensei: Nine (DDS9) - Recompiled Game Entry Point
 *
 * Host executable for the statically recompiled title. Boot sequence:
 *
 *   1. Load the original XBE from disk
 *   2. Initialise the Xbox memory layout (data sections mapped at their
 *      original VAs)
 *   3. Initialise the Xbox kernel replacement layer
 *   4. Redirect guest paths (D:\) at the host game directory
 *   5. Initialise the kernel bridge (thunk table inside Xbox memory)
 *   6. Set the guest stack pointer
 *   7. Install a VEH handler for crash diagnostics
 *   8. Build the flat dispatch table, arm the watchdog, enter the guest
 *
 * XBE details, from game_files/default_analysis.json:
 *   Title:       DDS9
 *   Title ID:    0x41540002
 *   Base addr:   0x00010000
 *   Entry point: 0x0021AF2F
 *   Image size:  7192608
 *   Libraries:   XAPILIB, CalcSig, D3DX8
 */

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime */
#include <xbox/xboxrecomp.h>

/* Generated register model and dispatch (src/game/recomp/gen). */
#include "recomp_types.h"

/* ── Guest register file ────────────────────────────────────────
 *
 * RECOMP_TLS is not optional. The runtime defines these thread-local, and a
 * plain `extern` against a thread-local definition does not resolve to the
 * calling thread's copy. The host would then write g_esp somewhere the
 * generated code never reads, and the guest would start with every register
 * at zero and fault immediately -- having apparently ignored the setup that
 * visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* ── Title constants ───────────────────────────────────────────── */

#define DDS9_ENTRY_POINT   0x0021AF2FU      /* XBE entry point VA */
#define DDS9_XBE_PATH      "game_files\\default.xbe"
#define DDS9_GAME_DIR      "game_files"

/* Recompiled entry point (generated: see recomp/gen/recomp_0013.c). */
extern void xbe_entry_point(void);

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* ── Crash diagnostics ─────────────────────────────────────────── */

/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view: every generated function is a real symbol in the image, so the
 * debug info maps host address back to guest function; and every lifted call
 * pushes its guest return address onto the guest stack, so scanning up from
 * esp for values inside the code sections recovers the chain.
 *
 * The scan is a scan, not a frame walk -- these are FPO frames with no
 * reliable ebp chain. It over-reports, since addresses from returned-from
 * calls linger below esp, but naming the guest function is the whole question
 * at a fault. */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    /* The host call stack.
     *
     * The guest frame is only meaningful while g_esp belongs to the code that
     * faulted, and it often does not: a lifted `rep movs` becomes a host
     * memcpy, and a fault inside it leaves g_esp pointing at whatever guest
     * function last updated it -- which can be an entirely different call,
     * made long before. Reading that frame as if it were the culprit's is how
     * three separate wrong answers got their evidence. The host stack has no
     * such ambiguity: it names the generated C function by symbol. */
    {
        void *frames[24];
        USHORT n = CaptureStackBackTrace(0, 24, frames, NULL);
        USHORT f;
        fprintf(stderr, "  host stack:\n");
        for (f = 0; f < n; f++) {
            DWORD64 d2 = 0;
            if (SymFromAddr(GetCurrentProcess(),
                            (DWORD64)(uintptr_t)frames[f], &d2, sym))
                fprintf(stderr, "    %-2u %s+0x%llX\n",
                        f, sym->Name, (unsigned long long)d2);
            else
                fprintf(stderr, "    %-2u %p\n", f, frames[f]);
        }
    }

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* The frame unfiltered as well.
         *
         * A fault inside a block copy leaves no return address in range --
         * the copy is a host memmove and its caller's frame is further up
         * than the filter reaches -- so the filtered list came out empty and
         * said nothing. The raw words carry the pointers and lengths that
         * were handed to it, which is the part worth reading. */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");
        /* When the frame is gone, the indirect-call history is the only trace
         * left. A fill that ran over the stack destroys every return address
         * on it, so the list above goes empty exactly when it is needed. */
        {
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;
            uint32_t k;
            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");
        }
        for (i = 0; i < 20; i++)
            fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
    }
}

/* Name the exception codes that actually end this process, so an early exit
 * is a diagnosis rather than a silent disappearance.
 *
 * STATUS_BREAKPOINT is the one that matters here. Lifted INT 3 becomes
 * __debugbreak(), which on MinGW is a real int3; with no debugger attached
 * Wine terminates the process on the spot, printing nothing. Reporting only
 * EXCEPTION_ACCESS_VIOLATION makes that look like a clean exit. */
static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
    case EXCEPTION_BREAKPOINT:            return "BREAKPOINT (lifted int3 / __debugbreak)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
    default:                              return "unknown";
    }
}

/* The emulated APU's MMIO entry point.
 *
 * Declared here because nothing declares it: apu_mmio_hook.c says "called
 * from VEH in main.c" and then no main.c, this one or the template's, ever
 * called it. With RECOMP_AC97_READY set the APU's 512 KB is deliberately
 * PAGE_NOACCESS so its registers fault and can be routed to the model -- but
 * an unrouted trap is just a crash, which is what DDS9 hit on the first
 * register DirectSound touched. */
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The APU model itself, and the pointer the hook dispatches through.
 *
 * apu_hook_handle_mmio's first line is `if (!g_apu_state) return false`, and
 * nothing in the tree ever assigned it -- so even with the registers trapped
 * and the trap routed, every access was declined and came back as a crash.
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
/* The USB host controllers, same story as the APU: xbox_OhciInit gates itself
 * on RECOMP_USB and had no callers, and xbox_OhciHandleMmio expects the VEH to
 * route faults to it. DDS9 needs this to reach its menu -- with no gamepad
 * enumerated its main loop takes the "controller disconnected" branch every
 * frame and the title never leaves its boot state. */
extern void xbox_OhciInit(void);
extern int  xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code != EXCEPTION_ACCESS_VIOLATION) {
        /* Anything that is not an AV still ends the process if nothing else
         * handles it, and the guest's own SEH may well handle it -- so report
         * and continue the search rather than deciding here. */
        fprintf(stderr, "[EXCEPTION] 0x%08lX %s at RIP=0x%llX\n",
                (unsigned long)code, exception_name(code),
                (unsigned long long)ep->ContextRecord->Rip);
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /* NV2A register probe. Some titles poke GPU registers directly; on
         * hardware that returns GPU state. Leave it to the next handler
         * rather than reporting it as a crash. */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000)
            return EXCEPTION_CONTINUE_SEARCH;

        /* APU registers -> the emulated APU.
         *
         * This is the other half of RECOMP_AC97_READY. Reporting the codec
         * ready is what lets DirectSoundCreate get past its first gate, and
         * from there it drives the APU directly; unmapping those registers is
         * how the model sees the traffic. Handled means the instruction was
         * decoded and stepped over, so execution resumes rather than
         * unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0))
                return EXCEPTION_CONTINUE_EXECUTION;

            /* USB host controller registers -> the OHCI model. It owns two
             * 4 KB blocks and answers reads and writes for both. */
            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
                return EXCEPTION_CONTINUE_EXECUTION;
        }

        fprintf(stderr,
                "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
                (unsigned long long)ep->ContextRecord->Rip,
                (unsigned long long)fault_addr,
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
                g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── Boot ──────────────────────────────────────────────────────── */

static int run(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    /* Unbuffered: under Wine the interesting output is whatever was printed
     * immediately before the thing that killed the process. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Shin Megami Tensei: Nine (DDS9) - Static Recompilation ===\n");

    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize allocates.
     * Failure is not fatal -- the handler just prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    printf("Loading XBE: %s\n", DDS9_XBE_PATH);
    if (!load_xbe(DDS9_XBE_PATH, &xbe_data, &xbe_size)) {
        fprintf(stderr, "Failed to load %s -- run from the directory that "
                        "holds %s\\.\n", DDS9_XBE_PATH, DDS9_GAME_DIR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    printf("Initialising Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Failed to initialise Xbox memory layout: the "
                        "required virtual address range is unavailable.\n");
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n",
           (unsigned long long)g_xbox_mem_offset);

    printf("Initialising Xbox kernel replacement...\n");
    xbox_kernel_init();

    xbox_path_init(DDS9_GAME_DIR, NULL);

    printf("Initialising kernel bridge...\n");
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this, recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on every indirect
     * call, which for a C++ title is a dozen or more every time it goes
     * through a vtable. That reads as a hang during static initialisation.
     *
     * Optional by design: if the allocation fails the search still works, so
     * a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: trapping without a model declines every
     * access, and a model nothing traps into never sees a register. Xbox
     * physical RAM is what the APU walks to find voice buffers, so it gets
     * the guest RAM base. */
    /* Gates itself on RECOMP_USB, so this is unconditional. */
    xbox_OhciInit();

    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* The pushbuffer completion fence.
     *
     * DDS9's pushbuffer reserve (sub_002F6E40) spins in a three-instruction
     * loop at 0x002F6F33 waiting for the GPU to report progress:
     *
     *     edx = [dev+0x34]          ; pointer to the fence word
     *     esi = [dev+0x30]          ; sequence the title has submitted
     *     eax = esi - ebp           ; how far ahead of the target we are
     *   L: ecx = [edx]              ; re-read the fence every iteration
     *     edi = esi - ecx
     *     cmp eax, edi
     *     jb  L                     ; spin until the fence reaches ebp
     *
     * On hardware the GPU writes that word. Nothing here does, so it sits at
     * whatever it was last set to and the loop never ends. Watchdog samples
     * name it exactly: edx=80001000, esi=0000000B, edi=00000008, i.e. the
     * fence at 0x80001000 holds 3 while the title waits for 0x0B -- and the
     * count never moves, which is why kernel and indirect-call totals were
     * identical in samples taken 40 seconds apart.
     *
     * 0x0030BEFC is the global holding the device pointer (sub_002F6E40 loads
     * esi from it); the fence lives at 0x80001000, in the contiguous window,
     * which is where MmAllocateContiguousMemory puts a GPU-written semaphore.
     *
     * Reporting completion is honest here for the same reason the DMA_GET
     * acknowledgement is: the D3D11 layer does the drawing, so work the title
     * submitted really has been dealt with by the time it asks. */
    if (xbox_Nv2aMirrorFence(0x0030BEFCu, 0x30u, 0x34u) != 0)
        fprintf(stderr, "[BOOT] pushbuffer fence mirror not registered; "
                        "the title will hang in its reserve loop\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples
     * are thread-local, so it has to be handed the copies belonging to the
     * thread that runs guest code. */
    xbox_WatchdogStart();

    printf("\n=== Initialisation complete ===\n");
    printf("Entry point: 0x%08X\n", DDS9_ENTRY_POINT);
    printf("ESP:         0x%08X\n", g_esp);
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}

/* ── XBE loading ───────────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;

    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* The target is built WIN32 (no console), so WinMain is the real entry point.
 * main() is kept so the same image can be started from a terminal -- under
 * Wine that is how the boot log above becomes visible at all. */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    return run();
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    return run();
}
