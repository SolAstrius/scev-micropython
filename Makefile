# scev-micropython — top-level driver.
#
# Builds rvvm-hal first (libhal.a + picolibc-min if not present),
# then descends into port/ for the MicroPython compile + link.

HAL := vendor/rvvm-hal

.PHONY: all hal port clean run run-headless

all: port

# rvvm-hal builds picolibc-min on first invocation (~30s). HAL_PICOLIBC=min
# is the smallest libc that gives us printf/string/setjmp.
hal:
	$(MAKE) -C $(HAL) HAL_PICOLIBC=min

port: hal
	$(MAKE) -C port
	cp port/build/firmware.bin .
	cp port/build/firmware.elf .

clean:
	$(MAKE) -C port clean
	rm -f firmware.bin firmware.elf

# Bochs: GUI window with the gfx_text terminal grid.
run: port
	rvvm firmware.bin -bochs_display -nonet -nosound

# UART-only headless: stdin/stdout REPL on the host terminal.
run-headless: port
	rvvm firmware.bin -nogui -nonet -nosound

run-qemu: port
	qemu-system-riscv64 -M virt -nographic \
	    -cpu rv64,zba=true,zbb=true,zbs=true,zicond=true,zicboz=true \
	    -bios none -kernel firmware.elf
