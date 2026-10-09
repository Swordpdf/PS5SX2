# PS5SX2 disc-auto daemon (vk-285-160k, AI-assisted). make -> DiscLaunch.elf
# Send once via the console's ELF loader (port 9021) and leave it running: a PS2 DVD put in
# starts its game in PS5SX2 (dumping it first when needed). See disc_launch.c.
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
