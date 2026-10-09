# PS5SX2 disc speed test payload (AI-assisted). make -> PS5SX2DiscTest.elf, for the console's ELF loader.
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5SX2DiscTest.elf: disctest.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f PS5SX2DiscTest.elf
