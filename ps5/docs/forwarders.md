# PS5 home-screen forwarders

PS5SX2 accepts [PS5 Forwarder Format version 1](https://github.com/heydemoura/ps5-forwarder-format/blob/main/SPEC.md) launch arguments. A forwarder tile targeting `PPSA99203` can start a PS2 game directly, without opening the game shelf.

Create the tile with [Forwarder Manager](https://github.com/heydemoura/ps5-forwarder-manager), choosing **PS5SX2** as the target and the game's absolute path or its file name as the ROM. To make the in-game return action close PS5SX2 and show the PS5 home screen, add `"--exit-after-game"` after the ROM path in the generated tile's `forwarder.json` as shown below. ShadowMountPlus must register the new tile. The forwarder format's launcher needs elfldr on port 9021 when no launcher is already running. Install a PS5SX2 build with this argument support before opening the tile.

The tile's `forwarder.json` should contain, for example:

```json
{
  "format": "ps5-forwarder/1",
  "target": "PPSA99203",
  "args": ["--rom", "/data/PCSX2/games/My Game.chd", "--exit-after-game"],
  "creator": "Forwarder Manager"
}
```

`--rom` accepts a full path, including a mounted USB or NFS game, or a file name found in the same folders PS5SX2 scans for the shelf. It accepts disc images and PS2 ELF files through the normal game boot path, including their per-game settings and disc handling. A file name search checks the main games folder, `/data/PCSX2`, configured game folders, USB drives and mounted NFS shares, plus one subfolder in each. When the game cannot be found, PS5SX2 reports the problem and closes instead of starting another game. Without `--exit-after-game`, the in-game return action opens PS5SX2's shelf as usual.

For manual tiles, follow the format specification for the other required files (`eboot.bin`, `sce_module/libc.prx`, `sce_sys/param.json`, and the 512 x 512 `sce_sys/icon0.png`). The forwarder program and runtime must come from a matching version of the format's template.

## Console verification status

An installed version 1 tile targeting `PPSA99203` passed `--rom` and a game file name to a PS5SX2 test build on a PS5. The boot log recorded the resolved game path and skipped the shelf. With a test build of the public PS5VK driver, Radiata Stories reached its PS2 executable, ran at about 60 fps, displayed correctly and responded to the controller. The tester then selected the same tile directly on the PS5 home screen and confirmed that it launched the game again. The host test covers parsing `--exit-after-game`; the exit behavior has not been checked on the console.

The unmodified public PS5VK driver does not advertise `VK_KHR_push_descriptor`, which PCSX2's Vulkan renderer requires. The console test used a temporary local patch to advertise the extension and report a limit of 16; the driver's push descriptor path already exists. An earlier RADV test build aborted during Vulkan GS initialization.
