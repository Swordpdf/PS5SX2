<p align="center">
  <img src="ps5/docs/banner.png" alt="PS5SX2" width="100%">
</p>

<h3 align="center">PCSX2, the PS2 emulator, running natively on a jailbroken PS5.</h3>

<p align="center">
  <img alt="Platform: jailbroken PS5" src="https://img.shields.io/badge/platform-jailbroken%20PS5-8c6bff">
  <img alt="Renderer: Vulkan" src="https://img.shields.io/badge/renderer-Vulkan-6f55d9">
  <img alt="Based on PCSX2" src="https://img.shields.io/badge/based%20on-PCSX2-5a45c8">
  <img alt="Licence: GPL-3.0-or-later" src="https://img.shields.io/badge/licence-GPL--3.0--or--later-3d3a8c">
</p>

---

## What is PS5SX2?

PS5SX2 is a port of [PCSX2](https://github.com/PCSX2/pcsx2) to the PS5. It runs as a native app on a jailbroken console:

- PCSX2's recompilers run straight on the PS5's CPU.
- Its Vulkan renderer draws through a modified version of ps5vk, [Mihawk-99](https://github.com/mihawk-99)'s native Vulkan driver for the PS5's GPU ([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)), tweaked specifically for PS5SX2.
- You pick a game from a cover-flow shelf and play it upscaled on a 4K TV.
- You change its settings from your phone while you play.

## A passion project

PS5SX2 is a passion project. It started as a "can this even work?" experiment, done in spare time out of love for the PS2's library and the fun of seeing those games on new hardware. There's no company or schedule behind it, and nothing is for sale.

Expect rough edges. Bug reports with logs are very welcome, and so is patience.

## Highlights

- **Native, not streamed.** The emulator, its recompilers and its renderer all run on the console itself.
- **Up to 6x native resolution.** PCSX2's hardware renderer runs on Vulkan with 4K output, and FSR is one of the display filters.
- **A shelf for your games.** A 3D cover flow of your library. Covers download automatically the first time you start it. Games play from `.iso`, `.chd`, `.cso` and `.zso` files, and each is titled from its serial in PCSX2's game database, whatever the file is called.
- **Settings from your phone.** The shelf shows a QR code: scan it, or type the address under it (`http://<PS5 IP>:<port>/`) into any browser on the same network, and a settings page opens. Triangle on the shelf shows the QR code large.
  - Change the resolution, aspect ratio, filters, patches and more, for all games or one game.
  - Most changes apply while you play.
  - In a game, hold L2 + D-pad Down for 2 seconds to open the page in the PS5's own web browser. The game keeps running.
- **Widescreen and 60 FPS patches.** Put PCSX2 patch files (`.pnach`) in `/data/PCSX2/patches/` and switch them on per game from the settings page.
- **RetroAchievements.** Sign in on the shelf and earn [RetroAchievements](https://retroachievements.org) as you play (softcore). Unlocks pop up like PS5 trophies, with the badge and the trophy sound, and every game's list is in its Square menu. [RetroAchievements](#retroachievements) has the details.
- **Online play.** PCSX2's emulated network adapter is on for every game and goes out through the PS5's own connection. SOCOM II has played online matches on [PS Rewired](https://psrewired.com)'s revival servers.
- **Recommended settings** for the games played during development, one tap away on the settings page.
- **Bigger memory cards.** Make blank 8, 16, 32 or 64 MB cards on the settings page and pick the card in each slot, for all games or for one.
- **HD texture packs in one button.** Press Square on a game to download its HD texture pack from archive.org's [PCSX2 HD Texture Packs](https://archive.org/details/pcsx2-hd-texture-packs) collection (about 340 games). It installs itself. Your own PNG or DDS packs work too, kept on the console, on a USB drive or in any folder you name.
- **Rumble and an FPS graph.** The game's vibration goes to the controller, at the strength you choose, and a blue frame-rate graph can sit in the corner of the picture. The settings page switches them on and off, and chooses what the info box shows.
- **Up to 4 players** with a multitap: the other PS5 users logged in when the game starts play on their own controllers.
- **Fast forward** (experimental) on a button combo you choose, to skip videos.
- **Games, BIOS and textures on a USB drive.** Games are found in the drive's top folder, `games/`, `PCSX2/games/` and `PS5SX2/games/`; a BIOS in `bios/`, `PCSX2/bios/` or `PS5SX2/bios/`.
- **USB keyboard and mouse** play as the PS2 controller, with PCSX2's own keys. This doesn't work on firmware 11.x and 12.00 yet: see [Known limitations](#known-limitations).
- **Logs that survive.** The last sessions' boot, emulator and settings logs stay on the console, so a problem can be tracked down afterwards. When a game runs slow, a built-in profiler notes in the log which part of the emulator the time went to: logs from slow games are the most useful ones to send.

## What you need

- **A jailbroken PS5.** Development happens on a PS5 Pro on firmware 11.40. Testers have also run it on other PS5 models, including a Slim, on firmware 6.02 to 12.70. [Tested consoles](#tested-consoles) has the list.
- **The PS5SX2 Helper payload**, from the releases, loaded together with kstuff. It's based on OnionHEN. It jailbreaks PS5SX2 when it starts, which the emulator's recompilers need, and gives the app access to `/data`.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**, loaded at every boot like kstuff. It mounts PS5SX2 from `/data/homebrew/PPSA99203/` and puts it on the home screen.
- **Your own PS2 BIOS**, dumped from your own console.
- **Your own games**, as `.iso`, `.chd`, `.cso` or `.zso` files.

No BIOS, games or keys come with PS5SX2.

## Getting started

For the main release, use PS5SX2Helper.elf and PS5SX2Installer.elf to launch PS5SX2 and automatically keep your build up to date.

For now, updates are planned to roll out weekly as we work toward full compatibility across the PS2 library. Bug fixes, compatibility improvements, and new features will be bundled into batches rather than pushed one by one.

Thanks again for all the testing, logs, and feedback. It genuinely helps move this forward.

TO GET IT RUNNING:

-Put PS5SX2Helper.elf and PS5SX2Installer.elf in your autoload. They’ll make sure you always get the latest build directly from GitHub.

-Place your legally acquired PS2 BIOS in:
/data/PCSX2/bios

-Place your legally made game backups in:
/data/PCSX2/games

-Enjoy.

The first start downloads the covers for your games, then the shelf opens.

## Controls

**On the shelf**

| Button | Does |
|---|---|
| D-pad or left stick | Browse the games |
| L1 / R1 | Jump a page |
| Cross or OPTIONS | Play |
| Square | The game's settings: L1 / R1 switch between this game and all games, L2 / R2 between *Settings*, *Controls* and *Achievements* |
| L1 + Square | RetroAchievements: sign in or out |
| Triangle | The settings page's QR code, large |

**In a game**

| Combo | Does |
|---|---|
| Hold L1 + R1, click the touchpad | Back to the shelf |
| Hold L3 + R3, then D-pad Up / Down | Save / load state (slot 1) |
| Hold L3 + R3 and let go | Next display filter |
| Hold L2 + D-pad Down for 2 seconds | Open the settings page in the PS5's web browser; the game keeps running |
| The fast forward buttons (Controls tab; none by default) | Fast forward on or off |

In a game the touchpad click is the PS2's SELECT and OPTIONS its START. The Create button (left of the touchpad) does nothing.

You can change the save and load buttons on the *Controls* tab (*Buttons*): pick two buttons for each, and how long to hold them (instant up to 3 seconds). A finger on the touchpad's left or right side counts as a button too: the old touchpad + Cross combo is *Touchpad left* + *Cross* to save and *Touchpad right* + *Cross* to load. These combos always use the controller's own buttons, whatever the remapping below.

### USB keyboard and mouse

With a USB keyboard and mouse plugged into the PS5, the keyboard and mouse play as the PS2 controller, with PCSX2's own keys:

| Keys | PS2 controller |
|---|---|
| Arrow keys | D-pad |
| W A S D | Left stick |
| T F G H | Right stick |
| I J K L | Triangle, Square, Cross, Circle |
| Enter, Backspace | Start, Select |
| Q, E | L1, R1 |
| 1, 3 | L2, R2 |
| 2, 4 | L3, R3 |
| F1, F3 | Save state, load state (slot 1) |
| Hold Esc for a second | Back to the shelf |

The mouse moves a stick (the right one by default), and its left and right buttons are R1 and L1 (or R2 and L2). Games made for a USB keyboard and mouse, such as Half-Life, get the PS2's own USB devices: *Auto* switches to them when a game starts reading them, or you can pick *USB devices* on the settings page.

**Firmware:** this does nothing on firmware 11.x and 12.00 yet. The console refuses to load its keyboard and mouse libraries for PS5SX2 there (seen on 11.40 and 12.00), and the `[kbm]` lines in `boot.log` say so. It's expected to work on 10.60 and older, but no console has tried it yet.

## The settings page

Scan the QR code in the corner of the shelf with a phone on the same network, or type the address shown under it (`http://<PS5 IP>:<port>/`) into any browser there. You can change settings for all games or for one game:

- **Display:** resolution (1x to 6x), aspect ratio, widescreen patches and display filter (FSR, FSR soft, Classic, CRT).
- **Graphics:** the renderer (hardware, or software for games the hardware renderer gets wrong; it takes a restart), texture filtering, anisotropic filtering, blending accuracy, mipmapping, texture replacements and the folder they're read from.
- **Performance:** EE cycle rate and skip, MTVU, and *GPU readbacks* for the few games that read the picture back from the GPU (Guitar Hero II and III, OutRun 2006): *Accurate* is PCSX2's own way, the others trade accuracy for speed. On firmware below 10 a readback can wait for the next vblank, and some games read back so often that the waits add up elsewhere too, so a game that slows down for them switches to *Don't wait* by itself unless you've set this. TimeSplitters: Future Perfect never switches by itself: it builds textures from what it reads back.
- **Game:** the language the PS2 tells games.
- **On screen:** the info box in the top right corner (off, FPS, or FPS and how busy the EE, GS and VU threads are) and the blue FPS graph.
- **Patches:** the game's patch groups, such as 60 FPS.
- **Memory cards:** make blank 8, 16, 32 or 64 MB cards (they go in `/data/PCSX2/memcards/`) and choose the card in each slot, for all games or for one.

The **Controls** tab has the controller's settings, for all games or for one game:

- **Buttons:** the two buttons that save the state, the two that load it, the two that turn fast forward on and off (experimental; and its speed), and how long they're held first. Then what each of the controller's buttons presses on the PS2 controller: any PS2 button, the analog button, a *light press* (while it's held, the other buttons press at half strength, for games such as Metal Gear Solid 2 and 3 that read how hard a button is pressed) or nothing. A trigger on a face button presses as hard as it's pulled.
- **Button strength:** how hard each button presses (10% to full), for games that act on a light press: SOCOM II crouches with Triangle at 20% (Combined Assault at 30%), so the touchpad click set to Triangle at 20% makes a crouch button.
- **Sticks:** swap the sticks, invert either one, make the left stick press the D-pad (as well as moving, or instead), and a dead zone for each stick.
- **Controller:** rumble and its strength (25% to 200%), and a multitap in PS2 port 1 or 2 for up to 4 players (it takes a restart).
- **Keyboard and mouse:** whether the keys and the mouse play as the controller or as the PS2's USB devices, which stick the mouse moves, its speed and its buttons.

Most changes show up in the running game straight away. The page marks the few that need a restart. **Recommended** puts back the settings tuned for that game; it leaves your controls, language and memory cards alone.

The same settings are on the shelf: press Square on a game.

In a game, hold L2 + D-pad Down for 2 seconds and the PS5's own web browser opens on the page. The game keeps running behind it. If the console won't open its browser, the page's QR code and address show over the game instead, and the same combo hides them.

## Texture packs

**Downloading a pack:** press Square on a game. When archive.org's [PCSX2 HD Texture Packs](https://archive.org/details/pcsx2-hd-texture-packs) collection has a pack for it, the *HD texture pack* row shows its size: press Cross to download it. It downloads in the background (the shelf shows how far it is), unpacks into one file, `/data/PCSX2/textures/<serial>/replacements.pak`, switches on *Texture replacements* for that game and pops up when it's ready. A download that a game interrupts carries on when you're back on the shelf. Triangle twice cancels a download or deletes a pack PS5SX2 downloaded. The packs are made by the community; PS5SX2 only fetches them.

**Your own packs:** PS5SX2 loads PCSX2-style texture replacement packs as PNG or DDS files (DDS in BC1, BC2, BC3, BC7 or uncompressed). Switch on *Texture replacements* on the settings page. A game's pack is a folder named after its serial with a `replacements` folder inside: `<serial>/replacements/`. PS5SX2 looks for it here, in this order:

1. The folder the *Textures folder* setting names, as `<that folder>/<serial>/`.
2. A USB drive: `PS5SX2/textures/<serial>/`, `PCSX2/textures/<serial>/` or `textures/<serial>/`, and a pack copied to the drive's top folder as it is (`PS5SX2/<serial>/` or `<serial>/`, with its `replacements` folder).
3. `/data/PCSX2/textures/<serial>/`, PCSX2's own place.

Folder names match in any case.

## Online play

PS5SX2 emulates the PS2's network adapter on top of the PS5's own connection. It's on for every game: PCSX2's sockets backend, its DHCP server giving the game an address, and the DNS servers the PS5 uses. Games that never use the network don't pay for it. Switch it off for a game with *Network adapter* in its Square menu or on the settings page, or for every game with a file named `nonetwork` in `/data/PCSX2/flags/`.

Revival servers usually need their own DNS server and sometimes a DNAS bypass cheat: see their guides. SOCOM II (SCUS-97275) has played online matches on PS Rewired's servers.

<details>
<summary>How SOCOM II was set up</summary>

1. **The DNAS bypass:** put PS Rewired's DNAS bypass cheat (`0F6FC6CF.pnach`, linked from their [SOCOM II guide](https://psrewired.com/guides/socom2)) in `/data/PCSX2/cheats/`.
2. **The game's settings file:** add these lines to `/data/PCSX2/settings/SOCOM II - U.S. Navy SEALs (USA).ini` (the network adapter's own lines, `EthEnable`, `EthApi`, `EthDevice` and `InterceptDHCP`, are the defaults since 1.9; files that still have them work the same):

   ```ini
   DEV9/Eth/ModeDNS1=Manual
   DEV9/Eth/DNS1=67.222.156.250
   EmuCore/EnableCheats=true
   Cheats/Enable=General Cheats\DNAS Bypass
   EmuCore/EnableWideScreenPatches=false
   ```
3. **In the game:** create a network configuration (automatic settings work), connect, pick a universe, download the update and make your account.

PS Rewired doesn't allow the widescreen patch online. Stretching the picture to 16:9 on the settings page is fine. See [PS Rewired's guides](https://psrewired.com) for other games.

</details>

## RetroAchievements

Earn [RetroAchievements](https://retroachievements.org) as you play. It's softcore only: hardcore mode isn't available.

- **Signing in:** on the shelf, press L1 + Square and sign in with your RetroAchievements account. The password isn't kept, only a sign-in token, in `/data/PCSX2/achievements-secrets.ini`. Sign out from the same panel. Until you sign in, nothing connects.
- **Unlocks** pop up like PS5 trophies, with the achievement's badge, its title and the trophy sound. Mastering a game plays the platinum sound. The popups go through your jailbreak's ELF loader (port 9021); without one they're plain notifications.
- **The lists:** press Square on a game and go to *Achievements* with L2 / R2 to see its achievements and the ones you've unlocked. While you play, the settings page has the running game's list.
- **A dropped connection:** unlocks are retried. Going back to the shelf waits up to 5 seconds for any still being sent, and a notification says if one couldn't be.
- **Your disc has to be a version RetroAchievements knows.** One it doesn't recognise gets no achievements.

## Slow games

If a game runs slower than it should, [PERFORMANCE.md](PERFORMANCE.md) shows how to read the info box to find what's holding it back, which settings to change for it, and how to report it if that doesn't help.

## Tested consoles

PS5SX2 is developed on a PS5 Pro. Testers ran the first test builds on other consoles too:

| Console | Firmware |
|---|---|
| PS5 Pro | 10.01, 11.40 |
| Other PS5 models, including a Slim | 6.02, 8.40, 10.01, 10.60, 11.40, 11.60, 12.00, 12.70 |

Firmware that isn't listed hasn't been tried.

## Tested games

Every game below has been started on PS5SX2, on the PS5 Pro during development or on testers' consoles during the first test builds (September 2026): 108 games in all. It isn't a compatibility guarantee. PS5SX2 keeps changing, so a game can run better or worse now than it did in its test, and a few rows say that a fix hasn't been re-tested yet. Fan mods and homebrew aren't listed. Widescreen patches are on by default for every game that has one.

How to read the table:

- **Tested on** is the console and its firmware. *PS5* means a model that isn't a Pro.
- **4x** and **6x** are the resolution multipliers. **Full speed** means the game ran at its normal speed at that resolution. Many games are locked at 30 fps, or 25 fps on PAL discs. The resolution named is the highest one that held full speed in the tests.
- **60 and 50 FPS patches:** for a game locked at 30 fps (25 on PAL discs) whose fps patch is in PCSX2's patch collection, the row gives the patched frame rate, 60 fps (50 on PAL), with the unpatched one in brackets. Switch the patch on for the game on the settings page. A doubled frame rate doubles the work, so a patched game may need a lower resolution than its row says.
- **Short test** means less than three minutes of play were recorded. **Starts; no play time recorded** means the game started but there is nothing to judge its speed by.
- **Recommended** marks a game with a preset on the settings page. The **Recommended** button puts those settings back.

| Game | Tested on | Result |
|---|---|---|
| **007: Everything or Nothing** | PS5 10.60 | Starts; no play time recorded. |
| **007: From Russia with Love**<br><sub>SLUS-21282</sub> | PS5 10.60 | Full speed at 4x, about 60 fps. One crash after 30 minutes of play, cause unknown. |
| **007: Nightfire** | PS5 10.60 | Full speed at 4x, about 30 fps. |
| **Alone in the Dark**<br><sub>SLUS-21690</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. |
| **Alter Echo**<br><sub>SLUS-20465</sub> | PS5 12.00 | Full speed at 4x, about 30 fps. |
| **Area 51 (PAL)**<br><sub>SLES-52570</sub> | PS5 10.60 | Full speed at 4x, about 50 fps with the 50 FPS patch (25 without); slowdowns at 5x. |
| **Area 51**<br><sub>SLUS-20595</sub> | PS5 10.60 | Mostly full speed at 4x, with dips (about 55 fps). |
| **Bakugan: Battle Brawlers**<br><sub>SLUS-21902</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. |
| **Black**<br><sub>SLUS-21376</sub> | PS5 10.60 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). |
| **Bloody Roar 4** | PS5 10.60 | Short test at 4x: full speed, about 60 fps. |
| **Bully**<br><sub>SLUS-21269</sub> | PS5 10.60, 12.00 | Full speed at 4x, about 60 fps. With the 60 FPS patch it drops to about 40 fps near the school entrance, and a lower resolution doesn't help. An EE clock of 180% brings it to 53–55 fps. |
| **Burnout 3: Takedown (PAL)**<br><sub>SLES-52584</sub> | PS5 10.60 | Full speed at 4x, about 55 fps. |
| **Burnout 3: Takedown**<br><sub>SLUS-21050</sub> | PS5 12.00 | Full speed at 4x, about 30 fps. |
| **Call of Duty: World at War – Final Fronts**<br><sub>SLUS-21746</sub> | PS5 11.40 | Full speed at 4x, about 30 fps. |
| **Castlevania: Lament of Innocence**<br><sub>SLUS-20733</sub> | PS5 Pro 11.40 | Recommended: 6x, MTVU off and EE clock at 100%. The game freezes with MTVU on. |
| **Code Lyoko: Quest for Infinity**<br><sub>SLUS-21743</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **Crash Bandicoot: The Wrath of Cortex**<br><sub>SLUS-20238</sub> | PS5 10.01, 12.00 | Full speed at 6x, about 60 fps. |
| **Crash of the Titans**<br><sub>SLUS-21583</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. With the 60 FPS patch. Brief stalls while shaders compile. |
| **Crash Twinsanity (PAL)**<br><sub>SLES-52568</sub> | PS5 8.40 | Full speed at 6x, about 50 fps. |
| **Crash Twinsanity**<br><sub>SLUS-20909</sub> | PS5 12.00 | Short test at 4x: full speed, about 50 fps. |
| **Crash: Mind over Mutant**<br><sub>SLUS-21728</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. With the 60 FPS patch. |
| **Def Jam: Fight for NY**<br><sub>SLUS-21004</sub> | PS5 12.70 | Short test at 4x: full speed, about 40 fps. |
| **Dragon Ball Z: Budokai Tenkaichi 3**<br><sub>SLUS-21678</sub> | PS5 11.40 | Full speed at 4x, about 50 fps. |
| **Driv3r**<br><sub>SLUS-20587</sub> | PS5 10.01 | Full speed at 6x, about 40 fps. The GPU hung three times at 6x; the app closed itself each time. |
| **FIFA Street 2 (PAL)**<br><sub>SLES-53797</sub> | PS5 10.01 | Full speed at 4x, about 50 fps. |
| **FIFA Street 2**<br><sub>SLUS-21369</sub> | PS5 11.40 | Full speed at 4x, about 60 fps. |
| **Freedom Fighters**<br><sub>SLUS-20658</sub> | PS5 12.00 | Full speed at 6x, about 55 fps. With the 60 FPS patch. Some drops when helicopters are on screen. |
| **Ghost Rider**<br><sub>SLUS-21306</sub> | PS5 11.60 | Full speed at 4x, about 60 fps. |
| **God of War (PAL)**<br><sub>SCES-53133</sub> | PS5 10.01 | Full speed at 4x, about 50 fps. |
| **God of War**<br><sub>SCUS-97399</sub> | PS5 Pro 10.01, 11.40<br>PS5 6.02, 10.60, 11.60 | Recommended: 6x. Full speed at 6x, about 60 fps. Micro freezes reported in one test. |
| **God of War II**<br><sub>SCUS-97481</sub> | PS5 Pro 10.01<br>PS5 6.02, 10.60, 11.40 | Full speed at 4x, about 40 fps. |
| **The Godfather: The Game**<br><sub>SLUS-21385</sub> | PS5 10.60 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). |
| **Gran Turismo 4**<br><sub>SCUS-97328</sub> | PS5 Pro 11.40<br>PS5 8.40, 12.00 | Recommended: 6x. Also set 16:9 in the game's own options. Full speed at 6x, about 60 fps. |
| **Grand Theft Auto: Liberty City Stories**<br><sub>SLUS-21423</sub> | PS5 Pro 11.40<br>PS5 10.01 | Full speed at 4x, about 35 fps; slowdowns at 6x. Since 1.85, with the 60 FPS patch on the PS5 Pro: full speed at 6x, about 60 fps. |
| **Grand Theft Auto: San Andreas**<br><sub>SLUS-20946</sub> | PS5 Pro 11.40<br>PS5 12.00 | Full speed at 6x, about 60 fps. With the 60 FPS patch. Dips to 53–57 fps now and then. With the 60 FPS patch, set EE cycle rate to 180% (*Performance* tab): it held 60 fps at 6x on the PS5 Pro. |
| **Grand Theft Auto: Vice City**<br><sub>SLUS-20552</sub> | PS5 Pro 11.40<br>PS5 11.60 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). On the PS5 Pro at 6x the 60 FPS patch needs EE cycle rate 300% (*Performance* tab) to hold 60 in busy places; 180% gives 58–60 fps. |
| **Guitar Hero III: Legends of Rock**<br><sub>SLUS-21672</sub> | PS5 6.02, 10.01, 12.00 | Mostly full speed at 6x, with dips (about 50 fps). Slowdowns in songs, worst with the crowd in view. |
| **Guitar Hero: Smash Hits**<br><sub>SLUS-21866</sub> | PS5 10.01 | Short test at 4x: about 80% speed. Still slow in places. |
| **I-Ninja**<br><sub>SLUS-20705</sub> | PS5 10.60 | Full speed at 4x, about 50 fps. |
| **The Incredible Hulk: Ultimate Destruction**<br><sub>SLUS-20941</sub> | PS5 10.60 | Full speed at 4x, about 50 fps. |
| **Killzone (PAL)**<br><sub>SCES-52004</sub> | PS5 10.60 | Starts; no play time recorded. A fix for its graphics code came after the test and hasn't been re-tested. |
| **Kingdom Hearts**<br><sub>SLUS-20370</sub> | PS5 11.60 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). |
| **Kingdom Hearts Final Mix (English patch)**<br><sub>SLPS-25198</sub> | PS5 Pro 11.40 | Recommended: 6x, Cross/Circle swap patch. |
| **LEGO Batman: The Videogame (PAL)**<br><sub>SLES-55135</sub> | PS5 12.00 | Short test at 4x: full speed, about 45 fps. |
| **LEGO Batman: The Videogame**<br><sub>SLUS-21785</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. Don't use the Nearest filter: cutscenes look wrong. |
| **The Lord of the Rings: The Fellowship of the Ring**<br><sub>SLUS-20520</sub> | PS5 Pro 11.40 | Recommended: 6x, 60 FPS patch. |
| **The Lord of the Rings: The Return of the King**<br><sub>SLUS-20770</sub> | PS5 Pro 11.40 | Recommended: 6x, 60 FPS patch. |
| **The Lord of the Rings: The Two Towers**<br><sub>SLUS-20578</sub> | PS5 Pro 11.40 | Recommended: 6x, 60 FPS patch. |
| **The Mark of Kri**<br><sub>SCUS-97140</sub> | PS5 10.60 | Full speed at 4x, about 60 fps. |
| **The Matrix: Path of Neo (PAL)**<br><sub>SLES-53462</sub> | PS5 10.60 | Full speed at 6x, about 30 fps. |
| **Medal of Honor: European Assault**<br><sub>SLUS-21199</sub> | PS5 11.40 | Full speed at 4x, about 50 fps. |
| **Metal Gear Solid 3: Subsistence**<br><sub>SLUS-21359</sub> | PS5 12.00 | Mostly full speed at 6x, with dips (about 60 fps). With the 60 FPS patch the bridge drops to 42–53 fps and some cutscenes to 35 fps. |
| **Midnight Club 3: DUB Edition Remix**<br><sub>SLUS-21355</sub> | PS5 8.40, 10.01 | Full speed at 6x, about 60 fps with the 60 FPS patch (30 without). |
| **Mortal Kombat: Shaolin Monks**<br><sub>SLUS-21087</sub> | PS5 10.01, 10.60, 11.40 | Full speed at 6x, about 60 fps. |
| **NBA Street Vol. 2**<br><sub>SLUS-20651</sub> | PS5 10.01 | Short test at 4x: full speed, about 45 fps. |
| **Need for Speed: Carbon (PAL)**<br><sub>SLES-54321</sub> | PS5 10.60 | Starts; no play time recorded. |
| **Need for Speed: Hot Pursuit 2**<br><sub>SLUS-20362</sub> | PS5 10.01, 10.60 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). |
| **Need for Speed: Most Wanted**<br><sub>SLUS-21267</sub> | PS5 10.60, 11.60, 12.70 | Full speed at 6x, about 60 fps. |
| **Need for Speed: Most Wanted (Black Edition)**<br><sub>SLUS-21351</sub> | PS5 Pro 11.40<br>PS5 11.60, 12.00 | Recommended: 6x. Full speed at 6x, about 60 fps. A tester on 12.00 saw drops to 52 fps at the start of races at 6x. |
| **Need for Speed: Underground 2**<br><sub>SLUS-21065</sub> | PS5 10.01, 10.60, 11.60 | Full speed at 4x, about 55 fps. An early test saw ghosting on car lights and flicker on the ground. |
| **Oni**<br><sub>SLUS-20064</sub> | PS5 Pro 11.40 | Recommended: 4x, since 6x runs too slowly. |
| **Ookami**<br><sub>SLUS-21115</sub> | PS5 10.60 | Full speed at 4x, about 30 fps. |
| **PES 2009: Pro Evolution Soccer (PAL)**<br><sub>SLES-55406</sub> | PS5 10.01 | Full speed at 6x, about 60 fps. |
| **Prince of Persia: The Sands of Time**<br><sub>SLUS-20743</sub> | PS5 10.60 | Starts; no play time recorded. |
| **Prince of Persia: The Two Thrones**<br><sub>SLUS-21287</sub> | PS5 10.60 | Mostly full speed at 4x, with dips (about 35 fps). |
| **Prince of Persia: Warrior Within**<br><sub>SLUS-21022</sub> | PS5 10.60 | Mostly full speed at 4x, with dips: about 60 fps with the 60 FPS patch (30 without). |
| **Ratchet & Clank**<br><sub>SCUS-97199</sub> | PS5 12.00 | Mostly full speed at 6x, with dips (about 55 fps). Dips to 42–48 fps in the dense part of the first area. |
| **Ratchet & Clank (PAL)**<br><sub>SCES-50916</sub> | PS5 Pro 11.40 | Recommended: 6x, holds 50 fps. PS5SX2 has built-in widescreen for this disc. |
| **Ratchet & Clank 3**<br><sub>SCUS-97353</sub> | PS5 Pro 11.40 | Recommended: 6x. |
| **Rayman 2: Revolution**<br><sub>SLUS-20138</sub> | PS5 12.00 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). |
| **Rayman 3: Hoodlum Havoc**<br><sub>SLUS-20601</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **Rayman Arena**<br><sub>SLUS-20272</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **Rayman: Raving Rabbids**<br><sub>SLUS-21576</sub> | PS5 12.00 | Full speed at 4x, about 55 fps. |
| **Red Dead Revolver**<br><sub>SLUS-20500</sub> | PS5 10.01 | Full speed at 4x, about 60 fps with the Force 60FPS patch (30 without). |
| **Resident Evil 4**<br><sub>SLUS-21134</sub> | PS5 10.01, 12.00 | Full speed at 6x, about 30 fps. The radio froze for a moment once, probably a shader compiling. |
| **The Rise of the Kasai**<br><sub>SCUS-97416</sub> | PS5 10.60 | Full speed at 4x, about 55 fps. |
| **Runabout 3: Neo Age (PAL)**<br><sub>SLES-51223</sub> | PS5 10.01 | Full speed at 6x, about 25 fps. |
| **Scarface: The World Is Yours (PAL)**<br><sub>SLES-54182</sub> | PS5 10.60 | Short test at 4x: about 60% speed. A fix for its graphics code came after the test and hasn't been re-tested. |
| **Scarface: The World Is Yours**<br><sub>SLUS-21111</sub> | PS5 10.60 | Short test at 4x: about 55% speed. A fix for its graphics code came after the test and hasn't been re-tested. |
| **Shadow of the Colossus**<br><sub>SCUS-97472</sub> | PS5 Pro 11.40<br>PS5 10.60, 12.00 | Slow on a regular PS5, even at 1x: about 15–20 fps in one test, and hardly over 50 fps outside the temple in a later one. On the PS5 Pro with tuned settings (EE clock 180%, Instant DMA, fast MIN/MAX, clamp none) it reaches 53–55 fps at 5x and 6x. |
| **ShellShock: Nam '67**<br><sub>SLUS-20828</sub> | PS5 10.60 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). A fix for its graphics code came after the test and hasn't been re-tested. |
| **Silent Hill 2**<br><sub>SLUS-20228</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. With its 60 FPS patch. The flashlight scene used to drop frames; that was fixed. |
| **Silent Hill 3**<br><sub>SLUS-20622</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. With the 60 FPS patch; the lowest seen was about 57 fps (51 on an earlier build). |
| **Silent Hill 4: The Room**<br><sub>SLUS-20873</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. With the 60 FPS patch. |
| **SOCOM II: U.S. Navy SEALs**<br><sub>SCUS-97275</sub> | PS5 Pro 11.40 | Recommended: 6x, online (see [Online play](#online-play)). |
| **Sonic Gems Collection (PAL)**<br><sub>SLES-53350</sub> | PS5 12.00 | Slowdowns at 4x (about 90% speed). |
| **Sonic Mega Collection Plus**<br><sub>SLUS-20917</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **Spider-Man 3**<br><sub>SLUS-21552</sub> | PS5 10.01 | Slowdowns at 6x (about 65% speed). |
| **Teenage Mutant Ninja Turtles (2007)**<br><sub>SLUS-21595</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. A crash at first start on firmware 12.00 has been fixed. |
| **Tenchu: Fatal Shadows**<br><sub>SLUS-21129</sub> | PS5 10.60 | Full speed at 6x, about 60 fps. |
| **Tenchu: Wrath of Heaven**<br><sub>SLUS-20397</sub> | PS5 10.60 | Full speed at 6x, about 60 fps. |
| **Test Drive Unlimited**<br><sub>SLUS-21490</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. Slowdowns at the start, probably shaders compiling. |
| **Test Drive: Eve of Destruction**<br><sub>SLUS-20910</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. |
| **Tom Clancy's Rainbow Six: Lockdown (PAL)**<br><sub>SLES-53104</sub> | PS5 10.60 | Mostly full speed at 4x, with dips (about 30 fps). |
| **Tom Clancy's Splinter Cell: Chaos Theory (PAL)**<br><sub>SLES-53287</sub> | PS5 10.60 | Full speed at 4x, about 45 fps. |
| **Tom Clancy's Splinter Cell: Double Agent (PAL)**<br><sub>SLES-53826</sub> | PS5 10.60 | Full speed at 4x, about 40 fps. |
| **Tom Clancy's Splinter Cell: Pandora Tomorrow**<br><sub>SLUS-20958</sub> | PS5 10.60 | Full speed at 4x, about 35 fps. |
| **Tony Hawk's Project 8**<br><sub>SLUS-21444</sub> | PS5 10.01 | Full speed at 4x, about 60 fps. With its 60 FPS patch. |
| **True Crime: Streets of LA**<br><sub>SLUS-20550</sub> | PS5 10.60 | Full speed at 4x, about 30 fps. |
| **Urban Reign**<br><sub>SLUS-21209</sub> | PS5 12.00 | Full speed at 6x, about 60 fps. |
| **The X-Files: Resist or Serve**<br><sub>SLUS-20179</sub> | PS5 12.00 | Short test at 4x: full speed, about 30 fps. |
| **X-Men Legends**<br><sub>SLUS-20656</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. 5–10 second freezes now and then. |
| **X-Men Legends II: Rise of Apocalypse**<br><sub>SLUS-21138</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **X-Men Origins: Wolverine**<br><sub>SLUS-21880</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **X-Men: Next Dimension**<br><sub>SLUS-20279</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **X-Men: The Official Game**<br><sub>SLUS-21107</sub> | PS5 12.00 | Full speed at 4x, about 60 fps with the 60 FPS patch (30 without). |
| **X2: Wolverine's Revenge**<br><sub>SLUS-20337</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |
| **Yu-Gi-Oh! Capsule Monster Coliseum**<br><sub>SLUS-20940</sub> | PS5 12.00 | Full speed at 4x, about 60 fps. |

## Known limitations

- **Some games are too heavy for 6x on a regular PS5.** [Tested games](#tested-games) shows what ran at full speed where, and [PERFORMANCE.md](PERFORMANCE.md) what to change for a slow game.
- **RetroAchievements is softcore only:** hardcore mode isn't available.
- **USB keyboard and mouse don't work on firmware 11.x and 12.00** yet. [The keyboard and mouse section](#usb-keyboard-and-mouse) has the details.
- **Restart needed for a few settings:** the renderer and MTVU only change when the game restarts.
- **Covers need the PS5SX2 Helper,** which gives the app `/data` before it starts. Without it, nothing downloads.

## Building from source

The PS5 layer lives in [`ps5/`](ps5/). [`ps5/README.md`](ps5/README.md) covers the layout, dependencies and build steps. Everything else in this repository is PCSX2, with the port's changes as commits on top.

## Credits

- **The PCSX2 Dev Team,** for PCSX2, which does all the emulating.
- **[Mihawk-99](https://github.com/mihawk-99),** for the Vulkan driver all of PS5SX2 runs on: a modified version of ps5vk, his native Vulkan driver for the PS5's GPU from [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), tweaked specifically for PS5SX2. PCSX2's hardware renderer, the upscaling and the 4K output all go through it. He also brings Mesa's RADV to the PS5 in [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa).
- **BlackBearReloaded,** for ps5-native-app-boilerplate.
- **John Törnblom,** for the ps5-payload-dev SDK.
- **[xlenore/ps2-covers](https://github.com/xlenore/ps2-covers),** for the covers the shelf downloads.
- **Gabriel Fonseca ([gbfonseca](https://github.com/gbfonseca)),** for the RetroAchievements groundwork PS5SX2's achievements are built on.
- **[RetroAchievements](https://retroachievements.org),** for the achievements and its rcheevos library.
- **The makers of the [PCSX2 HD Texture Packs](https://archive.org/details/pcsx2-hd-texture-packs),** for the packs the shelf downloads.
- **[PS Rewired](https://psrewired.com),** for keeping PS2 online games alive.
- **OnionHEN,** which the PS5SX2 Helper is based on.
- **drakmor,** for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), and **VoidWhisper,** for ShadowMount, which it's based on.
- **Project Nayuki's QR Code generator and Font Awesome Free:** see [`ps5/README.md`](ps5/README.md) for their licences; **Mbed TLS, libarchive** and the other libraries in [`ps5/third_party/`](ps5/third_party/README.md): see its README.

## Licence

PS5SX2 is GPL-3.0-or-later, like PCSX2. The licence text is [`COPYING.GPLv3`](COPYING.GPLv3). Third-party parts keep their own licences.

## Disclaimer

PS5SX2 is an independent project. It isn't affiliated with or endorsed by the PCSX2 team or Sony Interactive Entertainment.

"PlayStation", "PS2" and "PS5" are trademarks of Sony Interactive Entertainment. Game names belong to their owners.

Please use your own BIOS and your own games.

---

<p align="center">
  Made by swordpdf: Discord <b>sword.pdf</b> · X <a href="https://x.com/sword_pdf">@sword_pdf</a>
</p>
