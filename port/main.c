/* MicroPython entry point for scev-micropython.
 *
 * HAL's start.S sets up the stack and tail-calls kmain(hartid, fdt).
 * We init the HAL surface (via mphalport's helper), set up the GC
 * heap from a static buffer, then drop into the friendly REPL. The
 * REPL's blocking stdin read does the per-frame pumping (HID drain,
 * gfx_text redraw, wfi-pace) for us.
 */
#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "shared/runtime/pyexec.h"

#include "uart.h"
#include "rvvm.h"

#include <stdint.h>

extern bool scev_mp_hal_init(uintptr_t fdt_addr);

/* Heap for the GC. 256 KiB — comfortable for typical REPL workloads
 * (tracebacks, modest object graphs). RVVM gives us plenty of guest
 * RAM, so we're not pinching pennies here.
 *
 * Lives in BSS so it costs zero in the firmware.bin and gets zeroed
 * by start.S before kmain runs. */
#define HEAP_BYTES   (256u * 1024u)
__attribute__((aligned(16)))
static char heap[HEAP_BYTES];

/* Stack-bottom marker, captured at kmain entry. gc_collect_root walks
 * from the current stack frame up to here to find live pointers held
 * in C-side locals. Saving callee-save regs to a small stack buffer
 * before scanning makes that walk see registers too. */
static char *stack_top;

void gc_collect(void) {
    /* Spill callee-save regs so the GC roots them. */
    uintptr_t regs[12];
    __asm__ volatile (
        "sd s0,  0(%0)\n"  "sd s1,  8(%0)\n"
        "sd s2, 16(%0)\n"  "sd s3, 24(%0)\n"
        "sd s4, 32(%0)\n"  "sd s5, 40(%0)\n"
        "sd s6, 48(%0)\n"  "sd s7, 56(%0)\n"
        "sd s8, 64(%0)\n"  "sd s9, 72(%0)\n"
        "sd s10,80(%0)\n"  "sd s11,88(%0)\n"
        : : "r"(regs) : "memory"
    );
    void *sp = (void *)regs;
    gc_collect_start();
    gc_collect_root((void **)sp,
                    ((char *)stack_top - (char *)sp) / sizeof(void *));
    gc_collect_end();
}

/* MicroPython needs these to compile but we don't have a filesystem
 * yet — every import becomes ENOENT. Adding FatFs later flips this. */
mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    (void)filename;
    mp_raise_OSError(MP_ENOENT);
}

mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)n_args; (void)args; (void)kwargs;
    mp_raise_OSError(MP_ENOENT);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

/* Uncaught exception → panic. Reached when the REPL itself faults. */
void nlr_jump_fail(void *val) {
    (void)val;
    uart_puts("\n!!! nlr_jump_fail — uncaught NLR exception\n");
    for (;;) __asm__ volatile ("wfi");
}

/* Picolibc's assert lands here on assertion failures. */
void __assert_func(const char *file, int line, const char *func, const char *expr) {
    (void)func;
    uart_printf("\n!!! assert: %s:%u  %s\n", file, (uint64_t)line, expr);
    for (;;) __asm__ volatile ("wfi");
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    (void)hartid;

    /* Stack-walk anchor — must be a stack local before any other call. */
    int stack_dummy;
    stack_top = (char *)&stack_dummy;

    /* HAL: UART, time, PCI, I2C, HID, gfx_text grid (Bochs preferred
     * via FDT autodiscovery; falls back to UART-only if -nogui). */
    bool have_gfx = scev_mp_hal_init((uintptr_t)fdt_addr);

    /* Generous stack budget — main hart has 16 KiB per HAL link.ld; we
     * cap MicroPython's own recursion check well below that. */
    mp_stack_set_top(stack_top);
    mp_stack_set_limit(12 * 1024);

    gc_init(heap, heap + sizeof(heap));
    mp_init();

    uart_puts("\n");
    uart_puts("scev-micropython — MicroPython on RVVM bare-metal\n");
    uart_printf("heap=%u KiB  gfx=%s  hartid=%u\n",
                (uint64_t)(HEAP_BYTES >> 10),
                have_gfx ? "bochs+gfx_text" : "uart-only",
                hartid);
    uart_puts("Type help() for a quick tour.\n\n");

    pyexec_friendly_repl();

    mp_deinit();

    /* REPL exited cleanly (CTRL-D at empty prompt) → ask RVVM to
     * power off via the HAL convention. */
    uart_puts("\nGoodbye.\n");
    extern void hal_exit(int);
    hal_exit(0);

    /* Defensive — should never reach. */
    for (;;) __asm__ volatile ("wfi");
}
