# Building the disc-auto daemon into another payload (live-14)

`disc_launch.c` builds two ways:

- **Standalone** (`make` → `DiscLaunch.elf`): has `main()`, sets its own credentials, and keeps one instance via
  `/data/PCSX2/logs/disc-auto.pid` (a new copy kills the old one).
- **Embedded** (`-DDISC_AUTO_EMBEDDED`): no `main()`. The host payload starts the loop on a thread of its own:

```c
void *disc_auto_thread(void *arg);   /* from disc_launch.c; loops forever */

pthread_t t;
pthread_attr_t a;
pthread_attr_init(&a);
pthread_attr_setstacksize(&a, 256 * 1024);
pthread_create(&t, &a, disc_auto_thread, NULL);
```

## What the host has to provide
- **Credentials:** embedded, the file does not touch the process's credentials. The host must already be able to open
  `/dev/cd0` and `/dev/pass*` and send notifications (the standalone build does
  `kernel_set_ucred_authid(getpid(), 0x4800000000010003)` and `kernel_set_ucred_caps(getpid(), all 0xff)`).
- **Signals:** the standalone build sets `SIGCHLD` to `SIG_IGN`; embedded leaves signals to the host.
- **Files to compile:** `disc_launch.c` and `disc_hash.c` (with `disc_hash.h`), `-std=gnu11`, the PS5 payload SDK
  (`<cam/...>`, `<ps5/kernel.h>`, `<ps5/klog.h>`). No extra libraries: libSceSystemService and libSceLncUtil are opened
  with `dlopen` at run time.
- **Only one copy:** don't send `DiscLaunch.elf` while the host runs it. Embedded doesn't write the pid file, so the two
  wouldn't kill each other, and both would act on every disc.

## What it does (same in both builds)
- **PS2 DVD in:** not dumped yet → PS5SX2 is started (if needed) and dumps it to `/data/PCSX2/games/<Title> (<SERIAL>).iso`
  at Sony's 3.2x, then the game starts in front. Already dumped → the game starts in front.
- **PS2 CD in:** dumped by this code through the drive's pass device (READ CD, 2352-byte sectors) to
  `games/<Title> (<SERIAL>).bin` + `.cue` + `.hashes.txt` (CRC-32, MD5, SHA-1), then started.
- **Disc out:** PS5SX2's autostart guard and signal files are removed, so opening it lands on the shelf.
- **PS1 / PS3 discs:** logged only ("not handled yet").
- **USB DVD drives:** tried through their pass device (the 4.03 phat's USB stack refuses the reads; see
  `claude/ps5sx2-usb-optical-live2-5.md`).
- Talks to PS5SX2 through files in `/data/PCSX2/logs/` (`disc-launch.txt`, `disc-launch-exec.txt`, `ps5sx2-alive.txt`,
  `disc-auto-alive.txt`); needs PS5SX2 vk-285-160 / live-1 or later. Logs to the klog as `[disc-auto] ...`.

Licence: GPL-3.0-or-later, so the payload it's built into has to be GPL-3.0-compatible.
Host check of the checksums: `bash tests/run-hash-test.sh` (70 cases against Python's zlib/hashlib).
