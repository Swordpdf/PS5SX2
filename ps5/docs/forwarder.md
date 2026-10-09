# Starting a game from a home screen forwarder

A forwarder is a separate small app with its own home screen tile (its own title ID, icon and
name). When it is opened, it launches PS5SX2 (`PPSA99203`) with launch arguments, and PS5SX2
starts that disc image directly instead of opening the shelf.

## Arguments

| Argument | Meaning |
| --- | --- |
| `--rom <file>` or `--rom=<file>` | The disc image to start. An absolute path (`/mnt/usb0/PS2/Okami.iso`), or a path inside `/data/PCSX2/games/` or else `/data/PCSX2/` (`Okami.iso`, `RPG/Okami.chd`), the places the plain list looks. A relative path may not contain `..`. |
| `--exit-after-game` | When that game goes back to the menu, close PS5SX2 so the console returns to the home screen. Without it, the shelf opens, as usual. |

Unknown arguments are ignored. Parsing is in `ps5/coreorbis/orbis-shims/OrbisForward.h` (checked
on a PC by `ps5/coreorbis/tests/forward/test_forward.cpp`); `ps5/coreorbis/main-boot.cpp` looks for
the image after the jailbreak, when `/data` and the USB drives can be read.

## Behaviour

- The game starts with its own settings file and `gs.ini`, as from the shelf. The shelf and the
  cover prefetch before the jailbreak are skipped.
- Back to the menu saves the memory cards and the NVRAM, then opens the shelf, or, with
  `--exit-after-game`, closes PS5SX2.
- If the image isn't found, a notification says so and the shelf opens.
- If the game doesn't start, PS5SX2 restarts into the shelf, as for a game picked there.
- Every restart of PS5SX2 by itself (back to the menu, a failed start, a new eboot) runs without
  arguments, so it never starts the forwarded game again.
- Arguments only reach a new PS5SX2 process. If PS5SX2 is already running, the system brings it to
  the front and `main` does not run again; a forwarder should close it first.
