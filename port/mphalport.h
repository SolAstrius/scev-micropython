/* MicroPython HAL header — declares only what mp_hal_*() implementations
 * the rest of MicroPython needs to call. Implementations live in
 * mphalport.c. */
#pragma once
#include <stdint.h>
#include "py/mpconfig.h"
#include "py/runtime.h"

/* Time. ticks_ms is monotonic, wraps. Used by uasyncio scheduling +
 * uctypes bench paths we don't enable, but keep correct. */
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
void      mp_hal_delay_ms(mp_uint_t ms);
void      mp_hal_delay_us(mp_uint_t us);

/* IO. We provide the long-form *_strn writes; the runtime calls these
 * for prints, errors, REPL output. */
int       mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len);
/* mp_hal_stdout_tx_strn_cooked provided by shared/runtime/stdout_helpers.c */

/* CTRL-C handler — registered char gets translated into KeyboardInterrupt
 * when seen on stdin. We support that; default is 0 (off). */
/* Provided by shared/runtime/interrupt_char.c — declared here so
 * py/modmicropython.c sees the prototype via runtime.h → mphal.h →
 * mphalport.h chain. */
void mp_hal_set_interrupt_char(int c);

/* Forwarded to the host-terminal grid renderer for our gfx_text mirror.
 * No-op if gfx isn't initialised. Defined in mphalport.c. */
void scev_term_putc(uint8_t ch);
