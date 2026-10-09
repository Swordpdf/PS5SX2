# PS5SX2 disc-launch payload (vk-285-160b, AI-assisted). make -> DiscLaunch.elf
# Send via the console's ELF loader (port 9021): reads the PS2 disc's serial,
# finds its ISO in /data/PCSX2/games/, and relaunches PS5SX2 to start it directly.
# libSceSystemService is NOT linked — it is dlopen'd at runtime to avoid a hard
# NEEDED entry that the ELF loader process may not satisfy.
PS5_PAYLOAD_SDK ?= /home/claude/ps5deps/sdk285
CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
LIB := $(PS5_PAYLOAD_SDK)/target/lib
DiscLaunch.elf: disc_launch.c
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ $< \
	    -L$(LIB)
clean:
	rm -f DiscLaunch.elf
