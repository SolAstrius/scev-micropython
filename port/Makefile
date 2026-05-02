# scev-micropython port Makefile.
#
# Invokes MicroPython's py/mkenv.mk + py/py.mk to drive the QSTR
# pre-pass and per-TU compile, then links against rvvm-hal's libhal.a
# and picolibc-min via zig cc + lld.
#
# Top-level rvvm-hal build is triggered by ../Makefile before this is
# entered, so libhal.a + picolibc-min are guaranteed present here.

# ---------- MicroPython infrastructure ----------
TOP_REL := ../vendor/micropython
include $(TOP_REL)/py/mkenv.mk

QSTR_DEFS = qstrdefsport.h

# ---------- Toolchain ----------
HAL_DIR  := ../vendor/rvvm-hal
PICOLIBC := $(HAL_DIR)/vendor/picolibc-build/min/install
ZIG      ?= zig
TARGET   := riscv64-freestanding-none

CC      = $(ZIG) cc -target $(TARGET)
AS      = $(CC)
LD      = $(CC)
# Use zig's bundled binutils so the only build dependency is `zig` on
# PATH. mkenv.mk sets these with `=`, so we force-override with `=` too.
# Override on the command line (e.g. AR=llvm-ar make) to swap.
AR      = $(ZIG) ar
OBJCOPY = $(ZIG) objcopy
SIZE    := $(shell command -v llvm-size 2>/dev/null || command -v size 2>/dev/null || echo true)

# ---------- Flags ----------
ARCH_EXTS := -Xclang -target-feature -Xclang +zba \
             -Xclang -target-feature -Xclang +zbb \
             -Xclang -target-feature -Xclang +zbs \
             -Xclang -target-feature -Xclang +zicond \
             -Xclang -target-feature -Xclang +zicboz

INC += -I.
INC += -I$(BUILD)
INC += -I$(TOP)
INC += -I$(HAL_DIR)/include
INC += -isystem $(PICOLIBC)/include

CFLAGS += $(INC) -Os -g
CFLAGS += -ffreestanding -fno-stack-protector -fno-pie -mcmodel=medany
CFLAGS += -nostdlib
CFLAGS += -Wall -Wno-unused-parameter -Wno-error
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += $(ARCH_EXTS)
CFLAGS += -DHAL_PICOLIBC -DHAL_PRIV_M=1

# Some MicroPython core files emit warnings under clang we don't want
# to treat as errors (struct-aliasing in objstr, etc.). The minimal
# port has the same accommodation.
CFLAGS += -Wno-pointer-sign -Wno-unused-but-set-variable

LDFLAGS += -nostdlib -static
LDFLAGS += -Wl,-T,$(HAL_DIR)/link.ld
LDFLAGS += -Wl,--gc-sections

LIBS = $(HAL_DIR)/libhal.a $(PICOLIBC)/lib/libc.a

# Tell py.mk to strip its default warnings-as-errors off of our build.
CSUPEROPT = -Os

# ---------- Now bring in the core. ----------
include $(TOP_REL)/py/py.mk

# ---------- Sources ----------
SRC_C = \
	main.c \
	mphalport.c \
	shared/libc/string0.c \
	shared/libc/printf.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \
	shared/runtime/interrupt_char.c \

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c

# Map shared/* paths to the upstream MP tree.
vpath %.c . $(TOP_REL)

OBJ += $(PY_CORE_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

# ---------- Targets ----------
.PHONY: all
all: $(BUILD)/firmware.bin

$(BUILD)/firmware.elf: $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(LD) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	-$(Q)$(SIZE) $@ 2>/dev/null || true

$(BUILD)/firmware.bin: $(BUILD)/firmware.elf
	$(ECHO) "OBJCOPY $@"
	$(Q)$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

include $(TOP_REL)/py/mkrules.mk
