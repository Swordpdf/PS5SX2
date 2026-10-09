# PS5 disc drive report payload (AI-assisted). make -> DriveReport.elf, for the console's ELF loader.
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
DriveReport.elf: drivereport.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f DriveReport.elf
