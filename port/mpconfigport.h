/* MicroPython port config for scev-micropython on rvvm-hal.
 *
 * Conservative starting point — most optional features off so the
 * binary stays small and the build is reproducible. Float disabled
 * (we'd need libm; picolibc-min doesn't ship it). Everything that
 * doesn't need extra C stdlib is on. */
#include <stdint.h>
#include <alloca.h>     /* mp_local_alloc expands to alloca() in compile.c */

#define MICROPY_CONFIG_ROM_LEVEL          (MICROPY_CONFIG_ROM_LEVEL_BASIC_FEATURES)

#define MICROPY_ENABLE_COMPILER           (1)
#define MICROPY_ENABLE_GC                 (1)
#define MICROPY_HELPER_REPL               (1)
#define MICROPY_REPL_AUTO_INDENT          (1)
#define MICROPY_KBD_EXCEPTION             (1)

#define MICROPY_ENABLE_EXTERNAL_IMPORT    (0)
#define MICROPY_PY_SYS_MODULES            (0)
#define MICROPY_PY_SYS_EXIT               (1)
#define MICROPY_PY_SYS_PATH               (0)
#define MICROPY_PY_SYS_ARGV               (0)
#define MICROPY_PY_SYS_PLATFORM           "rvvm-hal"

#define MICROPY_PY_BUILTINS_HELP          (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT     scev_help_text
#define MICROPY_PY_BUILTINS_HELP_MODULES  (0)

/* No float — saves ~10 KiB and avoids libm dependency. */
#define MICROPY_FLOAT_IMPL                (MICROPY_FLOAT_IMPL_NONE)
#define MICROPY_PY_BUILTINS_FLOAT         (0)
#define MICROPY_PY_MATH                   (0)
#define MICROPY_PY_CMATH                  (0)

#define MICROPY_LONGINT_IMPL              (MICROPY_LONGINT_IMPL_LONGLONG)

#define MICROPY_ALLOC_PATH_MAX            (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT    (16)

/* Single-precision time in ms — wraps every 49 days, fine for a REPL. */
typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME             "RVVM bare-metal"
#define MICROPY_HW_MCU_NAME               "riscv64gc"

#define MP_STATE_PORT                     MP_STATE_VM

extern const char scev_help_text[];
