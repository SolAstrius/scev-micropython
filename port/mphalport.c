/* MicroPython HAL implementation for scev-micropython.
 *
 * Wires the runtime's mp_hal_* surface to rvvm-hal: stdout fans out to
 * the UART (for headless / scripting) AND to a Bochs gfx_text grid
 * that renders the same character stream in a window. Stdin drains an
 * input ring fed concurrently by UART RX (so a host pipe still works
 * if you launch with -nogui) and HID-keyboard usage codes (the GUI
 * window's keyboard).
 *
 * The rendering loop lives inside mp_hal_stdin_rx_chr — it's the only
 * blocking primitive the REPL ever calls, so we pump HID, redraw the
 * grid, and wfi-pace inside that wait. No frame timer, no per-frame
 * thread.
 *
 * gfx_text + HID feeding + ESC parsing follow the apple-1 reference;
 * see scev-cores/apple-1/src/main.c for the prior art that this is
 * adapted from.
 */

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mpstate.h"
#include "shared/runtime/interrupt_char.h"

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "i2c.h"
#include "hid.h"
#include "gfx.h"
#include "gfx_text.h"
#include "rvvm.h"
#include "font_8x8.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "mphalport.h"

/* ---------- Terminal grid configuration ---------- */
/* 80x30 — classic terminal dimensions, fits Python tracebacks
 * comfortably. font_8x8 at scale 2 → 16×16 cell, 1280×480 surface. */
#define TERM_COLS    80
#define TERM_ROWS    30
#define TERM_CELL_W  8
#define TERM_CELL_H  8
#define TERM_SCALE   2
#define DISPLAY_W    (TERM_COLS * TERM_CELL_W * TERM_SCALE)
#define DISPLAY_H    (TERM_ROWS * TERM_CELL_H * TERM_SCALE)

#define TERM_FG      0x00CCDDEEu
#define TERM_BG      0x000F1117u

static gfx_t       g;
static gfx_text_t  term;
static uint8_t     term_chars[TERM_COLS * TERM_ROWS];
static uint32_t    term_col = 0, term_row = 0;
static uint32_t    blink_phase = 0;
static bool        have_gfx = false;
static bool        db_gfx   = false;

/* ESC sequence parser state for the in-grid renderer. */
static enum { ST_NORM, ST_ESC, ST_CSI } esc_state = ST_NORM;
static int csi_args[4];
static int csi_n;

/* ---------- Input ring (UART + HID merge) ---------- */
#define RING_SIZE 256
static uint8_t ring_buf[RING_SIZE];
static uint16_t ring_head, ring_tail;
static hid_keyboard_t kb;

static inline bool ring_push(uint8_t ch) {
    uint16_t next = (uint16_t)((ring_tail + 1) % RING_SIZE);
    if (next == ring_head) return false;
    ring_buf[ring_tail] = ch;
    ring_tail = next;
    return true;
}

static inline int ring_pop(void) {
    if (ring_head == ring_tail) return -1;
    uint8_t ch = ring_buf[ring_head];
    ring_head = (uint16_t)((ring_head + 1) % RING_SIZE);
    return ch;
}

/* ---------- Terminal rendering ---------- */

static inline void term_clear(void) {
    memset(term_chars, ' ', sizeof(term_chars));
    term_col = term_row = 0;
}

static void term_scroll(void) {
    memmove(term_chars, term_chars + TERM_COLS,
            (TERM_ROWS - 1) * TERM_COLS);
    memset(term_chars + (TERM_ROWS - 1) * TERM_COLS, ' ', TERM_COLS);
}

static inline void term_newline(void) {
    term_col = 0;
    if (++term_row >= TERM_ROWS) {
        term_scroll();
        term_row = TERM_ROWS - 1;
    }
}

static void term_erase_to_eol(void) {
    if (term_col >= TERM_COLS) return;
    memset(&term_chars[term_row * TERM_COLS + term_col], ' ',
           TERM_COLS - term_col);
}

static void term_erase_line(void) {
    memset(&term_chars[term_row * TERM_COLS], ' ', TERM_COLS);
}

void scev_term_putc(uint8_t ch) {
    if (esc_state == ST_NORM) {
        switch (ch) {
        case '\n': term_newline(); return;
        case '\r': term_col = 0; return;
        case '\b':
        case 0x7F:
            if (term_col > 0) term_col--;
            term_chars[term_row * TERM_COLS + term_col] = ' ';
            return;
        case 0x07: return;                  /* bell */
        case 0x1B:
            esc_state = ST_ESC;
            return;
        }
        if (ch < 0x20 || ch >= 0x7F) return;
        if (term_col >= TERM_COLS) term_newline();
        term_chars[term_row * TERM_COLS + term_col++] = ch;
        return;
    }
    if (esc_state == ST_ESC) {
        if (ch == '[') {
            esc_state = ST_CSI;
            csi_n = 0;
            for (int i = 0; i < 4; i++) csi_args[i] = 0;
        } else {
            esc_state = ST_NORM;
        }
        return;
    }
    /* ST_CSI */
    if (ch >= '0' && ch <= '9') {
        csi_args[csi_n] = csi_args[csi_n] * 10 + (ch - '0');
        return;
    }
    if (ch == ';') {
        if (csi_n < 3) csi_n++;
        return;
    }
    /* Final byte. We support: K (erase line), D/C (cursor L/R), H (set pos). */
    int n = csi_args[0];
    if (n == 0 && (ch == 'C' || ch == 'D')) n = 1;
    switch (ch) {
    case 'K':
        if (csi_args[0] == 2) term_erase_line();
        else                   term_erase_to_eol();
        break;
    case 'D':
        term_col = (term_col > (uint32_t)n) ? term_col - n : 0;
        break;
    case 'C':
        term_col += n;
        if (term_col >= TERM_COLS) term_col = TERM_COLS - 1;
        break;
    case 'H': {
        int row = csi_args[0] > 0 ? csi_args[0] - 1 : 0;
        int col = csi_args[1] > 0 ? csi_args[1] - 1 : 0;
        if (row >= TERM_ROWS) row = TERM_ROWS - 1;
        if (col >= TERM_COLS) col = TERM_COLS - 1;
        term_row = row; term_col = col;
        break;
    }
    case 'J':
        /* Erase display variants — we only implement 2 (whole) here. */
        if (csi_args[0] == 2) {
            memset(term_chars, ' ', sizeof(term_chars));
            term_row = 0; term_col = 0;
        }
        break;
    default: break;
    }
    esc_state = ST_NORM;
}

static void term_render_frame(void) {
    if (!have_gfx) return;
    gfx_text_render(&term, &g, 0, 0);
    /* Cursor: blink an underscore at (col, row) at ~2 Hz.
     * Called inside mp_hal_stdin_rx_chr's idle loop, which runs at
     * ~240 Hz; >> 7 gives ~1 Hz on/off. */
    if ((blink_phase++ >> 7) & 1) {
        if (term_col < TERM_COLS && term_row < TERM_ROWS) {
            uint32_t px = term_col * TERM_CELL_W * TERM_SCALE;
            uint32_t py = term_row * TERM_CELL_H * TERM_SCALE;
            for (uint32_t s = 0; s < TERM_SCALE; s++) {
                uint32_t y = py + (TERM_CELL_H - 2) * TERM_SCALE + s;
                for (uint32_t x = 0; x < TERM_CELL_W * TERM_SCALE; x++) {
                    gfx_pixel(&g, px + x, y, TERM_FG);
                }
            }
        }
    }
    if (db_gfx) gfx_flip(&g);
}

/* ---------- HID translation ---------- */

static struct { bool shift, ctrl; } host_mods;

static const char digits_plain[]   = "1234567890";
static const char digits_shifted[] = "!@#$%^&*()";

static int hid_to_ascii(uint8_t usage, bool shift, bool ctrl) {
    /* Letters: 0x04..0x1D = a..z. ctrl→ctrl-X (0x01..0x1A). */
    if (usage >= 0x04 && usage <= 0x1D) {
        if (ctrl) return (usage - 0x04) + 1;
        char c = (char)('a' + (usage - 0x04));
        return shift ? (c - 0x20) : c;
    }
    if (usage >= 0x1E && usage <= 0x27) {
        int i = usage - 0x1E;
        return shift ? digits_shifted[i] : digits_plain[i];
    }
    switch (usage) {
    case 0x28: return '\r';                   /* Enter */
    case 0x29: return 0x1B;                   /* Esc */
    case 0x2A: return 0x7F;                   /* Backspace → DEL */
    case 0x2B: return '\t';                   /* Tab */
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    /* Arrows: emit a 3-byte ANSI sequence. We push ESC '[' first then
     * return the final letter; readline reassembles. */
    case 0x4F: ring_push(0x1B); ring_push('['); return 'C'; /* right */
    case 0x50: ring_push(0x1B); ring_push('['); return 'D'; /* left */
    case 0x51: ring_push(0x1B); ring_push('['); return 'B'; /* down */
    case 0x52: ring_push(0x1B); ring_push('['); return 'A'; /* up */
    default:   return -1;
    }
}

static void on_hid_key(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    /* Modifiers. */
    switch (usage) {
    case 0xE1: case 0xE5: host_mods.shift = pressed; return;
    case 0xE0: case 0xE4: host_mods.ctrl  = pressed; return;
    /* Alt/Meta dropped — readline doesn't use them, and emitting
     * them as raw chars confuses MicroPython's parser. */
    case 0xE2: case 0xE3: case 0xE6: case 0xE7: return;
    }
    if (!pressed) return;
    int ch = hid_to_ascii(usage, host_mods.shift, host_mods.ctrl);
    if (ch < 0) return;
    /* CTRL-C also raises KeyboardInterrupt directly. */
    if (ch == 0x03) {
        mp_sched_keyboard_interrupt();
    }
    ring_push((uint8_t)ch);
}

/* ---------- mp_hal_* exports ---------- */

mp_uint_t mp_hal_ticks_ms(void) {
    /* time_now() returns rdtime ticks; convert to ms. */
    return (mp_uint_t)(time_now() / (RVVM_TIME_HZ / 1000ULL));
}
mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)(time_now() / (RVVM_TIME_HZ / 1000000ULL));
}
mp_uint_t mp_hal_ticks_cpu(void) {
    return (mp_uint_t)time_now();
}

void mp_hal_delay_ms(mp_uint_t ms) {
    uint64_t deadline = time_now() + ms * (RVVM_TIME_HZ / 1000ULL);
    while (time_now() < deadline) {
        /* Pump the input ring + redraw so a delayed Python script
         * doesn't black-out the terminal or eat keypresses. */
        int c;
        while ((c = uart_getc_nb()) >= 0) {
            if (c == 0x03) mp_sched_keyboard_interrupt();
            ring_push((uint8_t)c);
        }
        hid_kb_poll(&kb, on_hid_key, NULL);
        term_render_frame();
        time_busy_until(time_now() + (RVVM_TIME_HZ / 240ULL));
    }
}
void mp_hal_delay_us(mp_uint_t us) {
    uint64_t deadline = time_now() + us * (RVVM_TIME_HZ / 1000000ULL);
    while (time_now() < deadline) { /* spin */ }
}

/* mp_hal_set_interrupt_char is provided by shared/runtime/interrupt_char.c */

int mp_hal_stdin_rx_chr(void) {
    for (;;) {
        /* Drain UART (host pipe / -nogui mode). */
        int c;
        while ((c = uart_getc_nb()) >= 0) {
            if (c == 0x03) mp_sched_keyboard_interrupt();
            ring_push((uint8_t)c);
        }
        /* Drain HID. */
        hid_kb_poll(&kb, on_hid_key, NULL);
        /* Check ring. */
        int ch = ring_pop();
        if (ch >= 0) return ch;
        /* Idle: redraw + sleep ~4ms. */
        term_render_frame();
        time_busy_until(time_now() + (RVVM_TIME_HZ / 240ULL));
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len) {
    for (mp_uint_t i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)str[i];
        uart_putc((char)ch);
        scev_term_putc(ch);
    }
    return len;
}
/* mp_hal_stdout_tx_strn_cooked is provided by
 * shared/runtime/stdout_helpers.c — it calls our mp_hal_stdout_tx_strn
 * once for each run of bytes, with a \r inserted before each \n. */

/* ---------- Init helper called from main ---------- */

bool scev_mp_hal_init(uintptr_t fdt_addr) {
    uart_init(0);

    static fdt_t fdt;
    bool have_fdt = (fdt_addr != 0) && fdt_init(&fdt, (const void *)fdt_addr);

    /* CLINT + tick rate from FDT if available, else RVVM defaults. */
    uintptr_t clint_at = RVVM_CLINT_BASE;
    uint64_t  hz       = 0;
    if (have_fdt) {
        uint32_t off = fdt_find_compatible(&fdt, "sifive,clint0");
        if (off != UINT32_MAX) {
            uint64_t a = 0;
            if (fdt_node_reg64(&fdt, off, 0, &a, NULL)) clint_at = (uintptr_t)a;
        }
        uint32_t cpus = fdt_find_node_named(&fdt, "cpus");
        if (cpus != UINT32_MAX) {
            uint32_t hz32 = 0;
            fdt_node_prop_u32(&fdt, cpus, "timebase-frequency", &hz32);
            hz = hz32;
        }
    }
    time_init(clint_at, hz);
    pci_init(0);
    i2c_init(RVVM_I2C_OC_BASE);
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);

    have_gfx = have_fdt && gfx_init_fdt(&g, &fdt, DISPLAY_W, DISPLAY_H);
    if (have_gfx) {
        gfx_fill(&g, TERM_BG);
        if (gfx_enable_double_buffer(&g)) {
            db_gfx = true;
            gfx_fill(&g, TERM_BG);
            gfx_flip(&g);
            gfx_fill(&g, TERM_BG);
        }
    }
    term_clear();
    term = (gfx_text_t){
        .cols   = TERM_COLS, .rows = TERM_ROWS,
        .cell_h = TERM_CELL_H, .scale = TERM_SCALE,
        .font   = font_8x8_ascii,
        .chars  = term_chars, .attrs = NULL,
        .fg     = TERM_FG,    .bg = TERM_BG,
    };
    return have_gfx;
}

/* ---------- Help text shown by Python's help() ---------- */
const char scev_help_text[] =
    "Welcome to MicroPython on RVVM bare-metal!\n"
    "\n"
    "Built on rvvm-hal: UART REPL, Bochs gfx_text terminal grid,\n"
    "HID keyboard, all glued to MicroPython's runtime.\n"
    "\n"
    "Try:\n"
    "  >>> for i in range(10): print(i, i*i)\n"
    "  >>> import sys; sys.platform\n"
    "  >>> help('modules')\n"
    "\n"
    "CTRL-C interrupts a running program.\n"
    "CTRL-D at an empty prompt exits the REPL (and powers off RVVM).\n";
