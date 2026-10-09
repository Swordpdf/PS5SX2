# PS5 disc read-speed test after the disc_detect payload's DVD path (live-13, AI-assisted). make -> DumpSpeedTest.elf
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
DumpSpeedTest.elf: dumpspeed.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $<
clean:
	rm -f DumpSpeedTest.elf
