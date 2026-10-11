# Slow game? Try this before you report it

PS5SX2 runs many games at full speed, but some games are heavier than others, and a regular PS5 or Slim has less graphics power than a PS5 Pro. Most slow games get better with a few settings. Try these first: it takes about five minutes. If the game is still slow afterwards, your report will tell us exactly where to look.

## First: is it slow, or is it broken?

- **Slow:** the game runs, but the frame rate drops, the sound stutters, or everything moves in slow motion. This guide is for that.
- **Broken:** the game won't start, crashes, shows a black screen, or has missing or garbled graphics. Settings rarely fix that. Report it with your logs ([see the end](#still-slow-report-it-like-this)).
- **Many PS2 games run at 30 fps**, or 25 on European (PAL) discs. A steady 30 is how those games were made, not a slowdown. Some games have a 60 FPS patch: switch it on in the settings page's Patches section.

## Step 1: read the info box

The box in the top right corner shows something like this:

```
42 FPS EE99 GS61 VU70
640x448 > 2560x1792
```

- **FPS** is the frame rate.
- **EE, GS and VU** show, in percent, how busy each of PS5SX2's three main threads was over the last second:
  - **EE** runs the PS2's main processor: the game's own code.
  - **GS** runs the PS2's graphics chip: everything that gets drawn, and the work handed to the PS5's GPU.
  - **VU** runs the PS2's vector unit, the game's 3D geometry, when MTVU is on (it is by default). With MTVU off it shows 0, and that work counts under EE.
- **The second line** is the game's own picture size, then the size it's drawn at.

If you don't see the box, go to the settings page, **On screen**, and set **Info box** to **FPS + load**. The **FPS graph** in the same section shows the last minute, which makes dips easy to spot.

Go to a spot where the game is slow and look at the three numbers. **The one at or near 100 is what's holding the game back.**

## Step 2: change the settings for that game

Open the game's settings in either of these ways:

- On the shelf, select the game and press **Square**. The sheet opens on **This game**; L1 / R1 switch between it and All games.
- Use the settings page:
  - scan the QR code on the shelf with your phone;
  - or, in a game, hold **L2 + D-pad Down** for 2 seconds to open it in the PS5's web browser. The game keeps running behind it.

Most changes apply straight away, so you can watch the info box while you try them. **MTVU** and **Renderer** only change when the game restarts.

Change one thing at a time and note the FPS after each change.

| What you see | What it means | What to try, in this order |
|---|---|---|
| **EE** at 95–100 | The PS5's CPU can't keep up with the game's code | 1. **EE cycle rate** 100% (the default is 130%), then 75%.<br>2. Turn off the game's **60 FPS patch**.<br>3. **EE cycle skip**: Mild. Try Moderate or Max only if Mild isn't enough: they can make the game itself run slower.<br>4. **MTVU** on, if you turned it off. |
| **GS** at 95–100 | Graphics are the limit | 1. **Resolution** down one step (6x → 5x → 4x → 3x).<br>2. **Blending accuracy**: Basic, or Min.<br>3. **Anisotropic filtering**: Off.<br>4. **Mipmapping**: Off.<br>5. **Display filter**: Classic.<br>6. Games that read the picture back from the GPU (Guitar Hero II and III, OutRun 2006): **GPU readbacks** set to Don't wait. If effects flicker, set it back. |
| **VU** at 95–100 | The game's 3D geometry | Turn off the **60 FPS patch**. Few other settings help here: report it ([see below](#still-slow-report-it-like-this)). |
| **Nothing near 100**, but the frame rate is still low | The emulated PS2 itself runs out of time, often with a 60 FPS patch on | **EE cycle rate** up: 180%, then 300%. GTA San Andreas and Vice City need 180% and 300% with their 60 FPS patches. Some games don't like it: if the game speeds up or glitches, go back. |

## What else costs speed

- **Resolution** is the biggest one. 6x is about 4K.
  - On a regular PS5 or Slim, heavy games often need 4x or 3x.
  - A Pro runs many games at 6x.
  - The [Tested games](README.md#tested-games) list on the front page says what ran at full speed, and on which console.
- **60 FPS patches** double the work for the CPU and the GPU. A patched game may need a lower resolution or a different EE cycle rate.
- **The software renderer** is much slower than the hardware one. Use it only for a game the hardware renderer draws wrong.
- **HD texture packs** can stutter while textures load the first time. To check whether a pack is the cause, turn **Texture replacements** off for that game.
- **Widescreen patches** show more of the scene, so there's a little more to draw.
- **The network adapter** costs nothing unless the game goes online.

## Step 3: try Recommended

These games have settings tuned on a PS5 Pro during development. Open the game on the settings page and press **Recommended** (it asks for a second tap):

- Castlevania: Lament of Innocence
- God of War
- Gran Turismo 4
- Kingdom Hearts Final Mix (English patch)
- The Lord of the Rings: The Fellowship of the Ring, The Two Towers and The Return of the King
- Need for Speed: Most Wanted (Black Edition)
- Oni
- Ratchet & Clank (PAL)
- Ratchet & Clank 3

## Still slow? Report it like this

"Game broken" with a log attached can't be acted on. A report like this one can:

```
Game: Ratchet & Clank 3 (SCUS-97353, USA)
Console: PS5 Pro, firmware 11.40
Where: first level, right after the ship lands (I have a save there)
Settings: 4x, MTVU on, EE cycle rate 130%, Blending Basic, 60 FPS patch off
Info box there: 41 FPS EE99 GS60 VU70
Tried: EE cycle rate 100% -> 48 FPS; 3x -> no change; EE cycle skip Mild -> 52 FPS but the game runs fast
Logs: attached
```

**Getting the logs:**

1. Play the slow part for at least a minute: `settings.log` gets one performance line per minute.
2. On the settings page, press **Download logs**. They're also in `/data/PCSX2/logs/`: `boot.log`, `emulog.txt` and `settings.log`.

Logs from slow games are the most useful ones: PS5SX2's built-in profiler writes down where the time went, and the per-minute lines show the frame rate, the speed and how busy each thread was.
