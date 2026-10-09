# PS5 disc drive insert test payload (AI-assisted). make -> DriveInsert.elf, for the console's ELF loader.
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
DriveInsert.elf: driveinsert.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f DriveInsert.elf
