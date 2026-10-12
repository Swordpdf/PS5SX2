# Elevate the PS5SX2 title's credential (AI-assisted). make -> Elevate.elf
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
Elevate.elf: elevate.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f Elevate.elf
