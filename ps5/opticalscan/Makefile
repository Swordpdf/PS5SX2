# PS5 disc optical drive scan payload (AI-assisted). make -> OpticalScan.elf, for the console's ELF loader.
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
OpticalScan.elf: opticalscan.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f OpticalScan.elf
