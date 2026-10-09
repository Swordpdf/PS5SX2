# PS5 disc drive probe payload (AI-assisted). make -> DriveProbe.elf, for the console's ELF loader.
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
DriveProbe.elf: driveprobe.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f DriveProbe.elf
