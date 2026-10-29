DiscTile.elf (test 6) - a PS2 disc in the PS5's drive gets its own tile on the home screen, and the "Unsupported Disc" tile is hidden
=====================================================================================================================================
Works with PS5SX2 build vk-285-169 (the eboot in PS5SX2-vk-285-168-test.zip) and DiscLaunch.elf from the same zip. Do not mix with older builds: the tile needs the new start-up code.

WHAT IT DOES
- While a PS2 disc is in a drive (/dev/cd0..3), it makes a second copy of PS5SX2 under another title ID (/data/homebrew/PPSA99231 .. PPSA99238),
  named after the game, with the game's cover/art, and asks ShadowMount+ (127.0.0.1:10101) to register it. The tile appears on the home screen.
- Clicking the tile starts PS5SX2 with no shelf: from the dump in games/ ("Title (SERIAL).iso") when it is whole, else straight from the disc.
  Settings are the ones saved under that game's name.
- The PS5's own "Unsupported Disc" tile (NPXS40144/40148/40149/40150) is hidden (visible=0 in app.db) while our tile is there, checked every 2 s,
  and shown again when the disc comes out or the payload stops. Nothing is deleted.
- When the disc comes out, the tile is unregistered and its folder removed.

NEEDS
1. ShadowMount+ loaded (its API answers on 127.0.0.1:10101). Without it you get the notification "No tile: ShadowMount+ did not answer".
2. PS5SX2 installed at /data/homebrew/PPSA99203 (the tile copies it by hard link, so no extra space).
3. /data/whitelist.txt gets the tile's title ID added, so PS5SX2's helper gives the tile root (as for PPSA99203). Check that this is the file your
   ps5sx2-bot.elf reads.
4. Art: a SteamGridDB API key as the only line of /data/PCSX2/steamgriddb.key. Without it the tile uses PS5SX2's own icon. Covers already in
   /data/PCSX2/cache/covers/<serial>.jpg are used too.

FLAGS (empty files in /data/PCSX2/flags/)
  disctile_nohide      leave the Unsupported Disc tile alone
  disctile_nodownload  do not ask SteamGridDB
  disctile_nonudge     do not touch app.db's change counters after hiding

LOG: /data/PCSX2/disc-tiles/disctile.log and klog lines "[disctile]". Please send it after a run (disc in, tile click, disc out, disc in again).

NOT TESTED ON A CONSOLE YET (host tests only, on two testers' app.db files)
- Whether the home screen shows the new tile and how fast; whether the SMP scan call registers it.
- Whether hiding the Unsupported Disc tile with visible=0 takes effect while the home screen is running.
- The real HTTPS download from SteamGridDB.
- PS2 CDs from USB.
Safety: app.db is backed up to /data/PCSX2/backup/ before it is changed.
