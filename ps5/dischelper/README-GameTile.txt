README-GameTile.txt (helper revision 171g): the game-tile contract between the shelf (PS5SX2) and the PS5SX2 Disc Helper.
Sections 2 to 9 of the shared contract v1, verbatim. TAB = 0x09, LF = 0x0A. Copyright (C) 2026 swordpdf; SPDX-License-Identifier: GPL-3.0-or-later.
The feature is OFF unless the empty file /data/PCSX2/flags/gametiles exists (flag gametile_off switches it off again). Cross-references to
sections 1, 10 and following belong to the contract file (sections: 1 switches, 10 what must stay as it is, 13 checks).

=======================================================================================================================

## 2. Directories and files: names, writers, readers, deleters

Folder D = `/data/PCSX2/tiles/` (mode 0777). Created by the HELPER (mkdirs + chmod 0777) at gt_init, only when enabled. The app MUST NOT create
D (no D = no helper = nothing is written). All files below D are mode 0666. Times are the console's clock (`time(NULL)`) on both sides.

| path | content | writer | reader | removed by, when |
|---|---|---|---|---|
| `D/request-<ms13>-<nnn>.txt.tmp` | a request being written | app | nobody | app: renamed to the final name at once; unlinked by the app on a write error. A leftover after a crash is harmless (ignored by the helper: it does not end in ".txt"). |
| `D/request-<ms13>-<nnn>.txt` | ONE press = one file (4.1) | app (rename of the .tmp) | helper | helper: renames it to `request.work`, parses, unlinks `request.work` (6.1). App: unlinks its OWN unanswered file (withdraw, 7). Helper at start: unlinks any request file with mtime older than 600 s unread. |
| `D/request.txt` | legacy form (same content, may hold several lines) | nobody in v1 | helper (taken AFTER all request-* files) | helper, as above |
| `D/request.work` | the request in work | helper | helper | helper (after parsing; a leftover is read again at the next tick = at-least-once; older than 600 s at start: dropped). The app MUST NOT touch it. |
| `D/state.txt` | what the helper knows (4.2) | helper (tmp + rename, whole file) | app (sheet, page) | nobody deletes it; it is overwritten. Valid ONLY while alive.txt is fresh. |
| `D/state.txt.tmp`, `D/alive.tmp` | helper-internal | helper | nobody | helper (rename, or unlink on failure) |
| `D/alive.txt` | heartbeat (4.3) | helper (tmp + rename) | app | nobody |

Helper-private (the app MUST NOT touch): `/data/PCSX2/disc-tiles/gametiles/next.txt` (ring pointer), `removing.txt` (pending removals),
`<id>.sig` (ledger, written by every build, NEVER deleted). Helper logs: `disctile.log`/klog lines starting `game tile` and `GameTile 1`.

Tile folder `/data/homebrew/PPSA992nn/` (mode 0777; a COPY of `/data/homebrew/PPSA99203`):
`eboot.bin` (real copy, read back, chmod 0777), `sce_module/*`, `game-tile.txt` (4.4), `disc-tile.txt` (4.4), `sce_sys/{param.json,icon0.png,pic0.dds}`
(sce_sys is made as `sce_sys.new` and renamed in LAST). Writer: helper. Readers: ShadowMount+, the shell, the tile's own app through /app0.
Removed by: the helper (`rm_tree`) on removal, on "damaged", and at start for an unfinished `sce_sys.new` folder of an earlier run.
During a refresh: `eboot.bin.new`, `sce_module/<f>.new` (renamed over). Also: `/data/whitelist.txt` gets the id appended (helper `whitelist_add`).

Read-only inputs of the helper outside D: `/data/PCSX2/cache/covers/<serial>.jpg` (written by the shelf's cover loader), `/data/homebrew/PPSA99203/*`.
Anything else in D is unknown to both halves and MUST be ignored.

---------------------------------------------------------------------------------------------------------------------------------------

## 3. Tile ids and slots

* Range: `PPSA99251 .. PPSA99260` (10 tiles, index s = 0..9). Reserved by this payload. Others: PPSA99203 = the shelf, PPSA99230 = Reload tile,
  PPSA99231..38 = disc tiles, PPSA99299 = smp_busy()'s probe id (never a tile), PPSA99261..99290 = reserved for growth (never 99299).
* Content id of tile s: `UP9000-PPSA992nn_00-PS5SX2DISCTILE<31+s>` (36 chars; 31..40, no clash with disc 01..08 / Reload 09 / ids 10..30);
  concept id = the digits of the id (`99251`); titleName = the cleaned title.
* Allocation (helper, main loop): ring from `next.txt` (index of the slot after the last one used, written at every allocation). A slot is
  usable when: state FREE; no folder `/data/homebrew/<id>` exists; and it is OURS (a ledger `<id>.sig` exists) or it is clean AND the shell
  database has no rows for it. Pass 1 takes only CLEAN slots (neither `/user/app/<id>` nor `/user/appmeta/<id>`), pass 2 accepts a slot with
  leftovers (the ghost rows are purged first). A folder in the range WITHOUT `game-tile.txt` (and without `sce_sys.new`) is FOREIGN: never used,
  never uninstalled, never purged. None usable = `failed:limit`.
* One slot per game path. A second `add` of a path that already has a slot (building, installing, ok) is a no-op.
* Slots survive a helper restart: adopted from the folders (state `installing` until the probe sees them registered, then `ok`), pending
  removals from `removing.txt`.

---------------------------------------------------------------------------------------------------------------------------------------

## 4. Formats (byte exact)

TAB = 0x09, LF = 0x0A. Writers use LF only; readers accept CRLF and ignore empty lines. UTF-8. `<ms13>` = unix time in milliseconds, 13 digits;
`<nnn>` = 3-digit zero-padded counter of the app process (000..999, wrapping) so that LEXICAL order = chronological order.

### 4.1 Request file (app -> helper): ONE FILE PER PRESS

Name: `request-<ms13>-<nnn>.txt`, e.g. `request-1760611234567-001.txt`. Written as the SAME name + `.tmp` (0666, fwrite + fclose checked, no fsync),
then `rename()`d. The app NEVER opens, appends to or rewrites an existing request file. Content (line 1 is exactly `v1`):

```
v1\n
add\t/data/PCSX2/games/God of War (USA).iso\tSCUS-97399\tGod of War\t/data/PCSX2/cache/covers/SCUS-97399.jpg\n
```
Other lines the helper understands:
```
add\t/mnt/usb0/PS2/Some Game.iso\t\tSome Game\n            (no serial: two TABs; no cover: 4 fields)
remove\t/data/PCSX2/games/God of War (USA).iso\n             (remove by game path)
remove\tPPSA99251\n                                          (remove by tile id: also for a game the shelf does not list any more)
```
Field rules (BOTH sides apply the same rule; the app refuses to offer/send what the helper would refuse):

| field | rule |
|---|---|
| path | absolute; starts with `/data/` or `/mnt/` and has at least one more byte; at most 511 bytes; no byte < 0x20, no 0x7F (so no TAB/CR/LF); no `..` as a whole component (a name like `..hidden` is fine); not a device. Normalized by BOTH sides before any comparison: `//` collapsed, trailing `/` stripped (the app also sends it normalized). Case-sensitive, exact. `/dev/...` and `/nfs/...` are NOT eligible (a disc has its own tile; NFS paths are virtual to PS5SX2's own process, the helper cannot stat them: C6). |
| serial | `[A-Za-z0-9._-]{1,15}` or EMPTY. The app sends empty when its serial does not fit. (The helper empties a serial with another character and cuts at 15.) |
| title | any UTF-8; the APP replaces TAB/CR/LF/control bytes by a space, trims, cuts at <= 120 bytes on a UTF-8 boundary, and never sends an empty one (the shelf's title). The helper cleans again (<= 127 bytes, invalid UTF-8 -> `?`, runs of spaces collapsed) and, if empty, derives it from the file name. |
| cover (5th, optional) | absolute path of a JPEG/PNG/BMP/GIF/TGA (stb_image; not WebP); at most 299 bytes; no `..`, no control byte. The app SHOULD send `CoverFinder::Best(game).path` when it is a non-empty regular `.jpg/.jpeg/.png` file (manual, beside the image, the drive's covers/, cache, OPL ART); else it sends 4 fields. Helper order: this file if it exists, else `cache/covers/<serial>.jpg`, else a title card, else PS5SX2's own art. |
| extra fields | up to 6 fields are split; more are ignored (forward compatible). |

The helper takes a request file only when it starts with the line `v1`; a file of another form is deleted unread (the shelf would then see
`Stuck`, 90 s). Limits: a file <= 8192 bytes, <= 32 request lines per file (a v1 shelf press has exactly one), queue of 16 (a 17th = `failed:busy`).

### 4.2 `D/state.txt` (helper -> app): whole file, tmp + rename, NO header line

One line per slot that holds a tile (index order PPSA99251..60), then one line per failed request (oldest first):

```
<id>\t<path>\t<serial>\t<title>\t<status>\n
```
```
PPSA99251\t/data/PCSX2/games/God of War (USA).iso\tSCUS-97399\tGod of War\tok\n
PPSA99252\t/mnt/usb0/PS2/Some Game.iso\t\tSome Game\tinstalling\n
PPSA99253\t/data/PCSX2/games/Okami.iso\tSLUS-21309\tOkami\tremoving\n
-\t/mnt/usb1/PS2/Gone.iso\t\tGone\tfailed:nogame\n
```
* status: `installing` (slot BUILDING or INSTALLING) | `ok` | `removing` | `failed:<reason>`. Reasons (a closed set, no TAB, no colon inside):
  `nogame badpath limit nospace build disabled busy damaged`.
* A failed request has id `-`; it stays 10 minutes (600 s), at most 6 such lines (oldest dropped). The file is rewritten without it at the next flush.
* The file may be EMPTY (0 bytes) = helper up, no tiles. A MISSING file while alive.txt is fresh is read as empty.
* Written at helper start (after adopting) and at the end of every one-second tick in which something changed (<= 1 s after the change). Not written while the console shuts down or rests (kp_quiet).
* Reader rules (app): split on LF, strip CR, ignore blank lines and lines starting with `#` or equal to `v1` (tolerated, never written); a line
  needs >= 5 TAB fields (extra columns ignored); a line with an UNKNOWN status word is IGNORED (not treated as busy); match to a game by the
  normalized PATH only (the id is for display and for `remove` by id). If several lines have the same path: ok > installing > removing > failed.
  Read at most 65536 bytes.

### 4.3 `D/alive.txt` (helper -> app): the helper is there

```
1760611234 1234 gametile 1\n          <unix time> <pid> gametile <contract version>
```
Written at the first tick after init and every 20 s while the helper runs (not while the console shuts down). The app calls the helper
PRESENT when the file exists, line 1 has the four fields with word `gametile` and version `1`, and `now - mtime <= 60` s (a negative age counts as 0).
Anything else (missing, older than 60 s, other word/version, unreadable) = ABSENT: the shelf shows `No helper` (section 8), ignores state.txt, and
writes NO request. alive.txt is never used for anything else. A helper with `gametiles` off writes none.

### 4.4 Tile folder markers (helper -> the tile's app, read from /app0 BEFORE the jailbreak)

`game-tile.txt` (exactly as `gt_marker_write` makes it; mode 0777):
```
/data/PCSX2/games/God of War (USA).iso\n
serial=SCUS-97399\n
title=God of War\n
tile=PPSA99251\n
rev=1\n
```
Line 1 = the normalized game path (spaces kept; only CR/LF trimmed; the app reads line 1 only for the path). The app reads `tile=<id>` (6-12
alnum) for the folder search; unknown or missing keys are ignored. A line 1 that fills the app's 1600-byte buffer without a LF = invalid.
`disc-tile.txt` = the single line `game\n` (5 bytes). Purpose: every existing "am I a tile" test in main-boot.cpp becomes true, and an app that does
not know game tiles (an old eboot copy) simply opens the shelf (`IsDrivePath("game")` is false). The helper writes BOTH, `game-tile.txt` first.
What main-boot does with each: section 9.

---------------------------------------------------------------------------------------------------------------------------------------

## 5. Who does what, in order (sequences)

### 5.1 ADD (shelf open)
1. Sheet opens: the app polls once (forced): alive.txt + state.txt. The row value comes from them (section 8).
2. Cross on the row, helper PRESENT, row can-Add: the app writes the request file (4.1), starts tracking it (`Asking`, "Adding...").
3. <= 1 s later (3 s if a drive game plays, <= 15 s worst case) the helper takes ONE request file per tick: rename -> `request.work`, parse,
   unlink. The file vanishes from D = TAKEN. The app sees it at its next poll (1 s while pending) -> `Waiting`.
4. The same tick: the request enters the queue; `gt_queue_run` handles one entry per tick (never past an earlier entry for the same path).
   When no build/refresh runs and no disc tile/Reload work runs: slot picked, state `installing` written within the second. Otherwise it waits
   (<= tens of seconds; no state line yet; the shelf shows `Adding...` up to 90 s from the take, then `Stuck`).
5. The build thread (3-5 s; slow way up to ~17 s): copy eboot + modules, `game-tile.txt`, `disc-tile.txt`, param.json, art, rename `sce_sys` in,
   `<id>.sig`, `whitelist_add`, scan request to ShadowMount+. The state stays `installing`.
6. While PS5SX2 (or any app) runs, ShadowMount+ takes nothing in. The state stays `installing` as long as that lasts: THERE IS NO TIME-OUT ON IT.
7. PS5SX2 is closed. ShadowMount+ finalizes the stopped app (~27 s), scans, the shell registers the tile (~5 s): about 30 s (25-40 s) after the
   close the tile is on the home screen. The helper probe (one tile per 2 s) sees rows in the shell database and ShadowMount+ `games/info` 200 ->
   state `ok` (and `visible=1` again unless `gametile_nohide`). The next time the shelf opens it shows `On`.

### 5.2 REMOVE (shelf open)
1. Cross (or Triangle) on an `On` row: request `remove<TAB><path>`. Taken as above.
2. The helper sets the slot REMOVING at once (state `removing`), writes `removing.txt`, hides the tile's rows (`visible=0`, unless `gametile_nohide`).
   The folder stays; the tile still works if pressed until the removal runs.
3. The removal runs only when no app is running: the main loop looks every 3 s; two idle looks in a row start the removal thread (an unreadable
   process table: ten looks = 30 s, then it tries anyway). The thread: folder removed, `POST /api/v1/games/uninstall`, 409 `game_active` retried
   (0.4 s, then slower). It NEVER closes or kills a running app. The shelf shows `Removing...` for the whole session.
4. After the uninstall went through: leftover shell rows are purged at 1.5 s, 10 s, 30 s; when none is left (or 90 s) the slot is released, the line
   leaves state.txt. Visible effect: gone from the home screen about 30 s after PS5SX2 is closed.

### 5.3 Other cases
* ADD of a game whose tile is `removing`: the add waits (Waiting, "Adding...") until the removal is finished (after PS5SX2 is closed), then the
  tile is built again. Other requests behind it are NOT blocked (queue walks past it). REMOVE of a game that is still BUILDING waits for the build.
* RETRY after `failed:*`: the app asks again; when the helper reads the line it forgets that path's old failed line (H3). A new failure is a new line.
* HELPER RESTART: pending removals resume; tiles are adopted (`installing` -> `ok` within a few seconds); request files older than 600 s are dropped.
* CONSOLE RESTART with tiles installed: tiles are PERSISTENT (like the Reload tile); the helper re-adopts them. NOT PROVEN on a console (KP, 14).
* PS5SX2 UPDATE (installer replaces PPSA99203): each tile's eboot copy is stale; the helper (no app running, one tile per 60 s) replaces
  `eboot.bin` + `sce_module` in place and rewrites `<id>.sig` (unless `gametile_norefresh`). Unobserved on a console.
* The person deletes a game tile from the home screen's own menu: ShadowMount+ registers it again at its next scan; after three empty looks (a
  minute apart) the state flips back to `installing`.
* ISO file moved/deleted/USB renumbered: the tile stays; pressing it gives the "not found" notification and the shelf. Removal by tile id (4.1) works without the game.

---------------------------------------------------------------------------------------------------------------------------------------

## 6. Helper rules the app relies on

6.1 One request file per second (name order, request.txt last); nothing is taken while the queue holds more than 8 entries; `request.work` left by a crash is processed first.
6.2 Every request is idempotent: add of an existing tile = no-op; remove of an unknown path/id = no-op (state unchanged).
6.3 `state.txt`/`alive.txt` only via tmp + rename. Nothing is read, written or asked while `kp_quiet()` (console shutting down/resting); no thread starts then; the build thread checks between its steps.
6.4 SQLite only on the main loop. At most one build/refresh thread; a build never starts while a disc tile build, the staging or the Reload tile runs (the disc side is NOT made to wait for a game tile).
6.5 A tile of somebody else under an id of the range is never used, uninstalled or purged (ledger rule, section 3).
6.6 Disc code: only three edits in disctile.c (include, `gt_tick(now)` before `usleep(POLL_US)`, PAYLOAD_REV 171g). No disc function calls a game-tile function; the game-tile code calls only disc statics that already have two or more call sites (asm and import gates, section 13).
6.7 Free space: a build needs >= 192 MB free on /data (`failed:nospace`); each tile costs ~47 MB.
6.8 Cadence: `gt_tick` once per wall-clock second; once per 3 s while a game plays from a drive (game quiet); heartbeat 20 s.
6.9 Heal-only: with the feature off and a ledger present, `gt_heal_tick` still cleans the stale `/mnt/sandbox/PPSA992nn_NNN` of played game tiles (they keep ShadowMount+ busy for the DISC tiles); `gametile_noheal` stops it.

---------------------------------------------------------------------------------------------------------------------------------------

## 7. Timing rules and time-outs (one table; values are final)

| who | name | value | meaning |
|---|---|---|---|
| app | kTakeS | 15 s | own request file still in D after 15 s (age from the write) = `No answer`, then the app unlinks its own file (withdraw). ENOENT on the unlink = it was taken just now = `Waiting`. |
| app | kAppearS | 90 s | file taken, no state line for the path after 90 s from the take = `Stuck` (add). For a remove: the line is still `ok`/`installing` 90 s after the take = `Stuck`. |
| app | installing / removing | NO LIMIT | while the helper is present the shelf shows `Installing...`/`Removing...` for as long as the state says so (the shelf is the very thing that holds them). The 240 s / 120 s limits of tiles_app.md are DELETED. |
| app | kRetryGraceS | 5 s | after a retry, a `failed` line with the same text as the one present when the retry was asked is ignored for 5 s after the take (covers the instant between "taken" and the helper's flush). |
| app | helper present | 60 s | alive.txt mtime age (4.3). |
| app | kShutdownWithdrawS | 3 s | App::Shutdown (and the page's own requests: one process-wide registry of the files this process wrote) unlinks every own request file still in D and older than 3 s. |
| app | poll | 1.0 s while an ask is pending or the shown row is busy, else 3.0 s; only with the sheet open (row shown) or an ask pending; skipped while a texture pack unpacks/installs/removes (except the forced read at OpenSheet) | reads alive.txt + state.txt, stats own files |
| helper | request tick | 1 s (3 s game quiet) | one request file per tick |
| helper | queue | 16 entries, 1 step per tick | `failed:busy` when full |
| helper | stale request files | 600 s (by mtime), at helper start only | dropped unread |
| helper | failure lines | 600 s, max 6 | then gone from state.txt |
| helper | alive | every 20 s | |
| helper | probe | one INSTALLING tile per 2 s; rows>0 and `games/info` 200 -> `ok`; no rows and no app running -> scan again (+8 s each, 6 tries, then +30 s) | |
| helper | ok re-check | one tile per 60 s; 3 empty looks -> `installing` again | |
| helper | removal gate | looks every 3 s, 2 idle looks (>= 6 s) start the thread; thread loops every 2 s while an app runs | |
| helper | uninstall | 409 retries every 0.4 s (50 tries), then 2 s / 5 s / 20 s; 5xx gives up after 40 tries | |
| helper | ghost rows | purge at 1.5 s, 10 s, 30 s after the uninstall; slot released at rows == 0 or 90 s | |
| helper | sandbox heal | look every 3 s; stale for 6 s with no app; sandbox age >= 10 s; retry 20 s (120 s after 4 failures) | |
| helper | folder / eboot re-check | every 60 s, only when no app runs; a failed refresh waits 600 s | |
| system | what the person sees | tile 25-40 s (typ. 30 s) after PS5SX2 is CLOSED (backgrounded is not enough); build 3-5 s; removal: row hidden at once, tile gone ~30 s after the close | one tester log: 27 s from process exit to ShadowMount+ finalizing + ~5 s |

---------------------------------------------------------------------------------------------------------------------------------------

## 8. State words -> what the shelf shows (sheet row and settings page use the SAME text)

Row label `Home screen tile` under a header `Home screen` (after the Shelf group, Settings tab of a game sheet only). The row is listed for every
eligible game (not the all-games sheet, not the copy-in-progress / "disc in the drive" entries, not an ELF, not a damaged image, path eligible
per 4.1 including /data|/mnt) whenever `tiles_dir` is set. Value text per phase (enum 0..9, 9 appended):

| phase | when | value | help line (English; the sheet is English-only) | actions |
|---|---|---|---|---|
| 0 Off | helper present, no ask, no state line | `Off` | `Puts <title> on the PS5 home screen as its own tile: press it there and this game starts, no shelf. The tile appears about 30 seconds after you close PS5SX2.` | Cross/Left/Right = add |
| 1 Asking | own file in D, age <= 15 s | `Adding...` (ask=add) / `Removing...` (ask=remove) | `Asked the helper.` | none |
| 2 Waiting | file taken, no line yet (add) / taken and an old tile is being removed | `Adding...` | `The helper has the request. A tile that was removed a moment ago is finished first.` (removing) or `The helper has the request.` | none |
| 3 Installing | line `installing` | `Installing...` | `The tile is built. It appears on the home screen about 30 seconds after you close PS5SX2: the console adds it only when no game is running, and PS5SX2 counts as one, also in the background.` | none |
| 4 On | line `ok` | `On` | `On the home screen. Cross takes it off again (it goes away about 30 seconds after you close PS5SX2). If the game runs from the tile, close it first.` | Cross/Triangle = remove |
| 5 Failed | line `failed:<reason>` | `Failed` | `Failed: <reason text below>` | Cross = retry (add) |
| 6 Removing | line `removing`, or ask=remove taken | `Removing...` | `The tile is hidden now and is taken away about 30 seconds after you close PS5SX2.` | none |
| 7 No answer | own file untaken after 15 s | `No answer` | `The helper did not take the request within 15 seconds. Load the newest PS5SX2 Disc Helper (171g), then press Cross to try again.` | Cross = retry |
| 8 Stuck | taken but no result in 90 s | `Stuck` | add: `The helper took the request but the tile did not show up in time. Press Cross to try again; disctile.log (flag kp_log) says why.` remove: `The tile is still on the home screen. If the game runs from it, close it first, then try again.` | add: Cross; remove: Cross/Triangle |
| 9 No helper | alive.txt absent (4.3), or the state is unknown | `No helper` | `The helper that makes game tiles is not running. Create the file gametiles in /data/PCSX2/flags, then load the newest PS5SX2 Disc Helper (171g).` | none; Cross shows the status line `Load the PS5SX2 Disc Helper 171g first` and writes nothing |

Reason texts (`failed:<reason>`; an unknown reason is shown as `Failed (<reason>)`, escaped on the page):
`nogame` = `The helper cannot find the game file (is its drive connected?).`  `badpath` = `The helper cannot use this path (only files under /data or on a drive under /mnt).`
`limit` = `All 10 game tiles are in use: take one off first.`  `nospace` = `Not enough free space on /data (about 200 MB are needed).`
`build` = `Building the tile failed (the helper's log has the details).`  `disabled` = `Game tiles are switched off in the helper (flag gametile_off, or gametiles missing).`
`busy` = `The helper's request queue is full: try again in a minute.`  `damaged` = `The tile's folder was damaged and has been taken down: add it again.`

Other shelf texts: status line after a press `Asked the helper to add <title>` / `... to take off <title>`; busy press `Still working: wait for the helper`;
own-tile guard `This is the tile this game was started from: take it off from the main app`; error `Couldn't ask the helper: <errno text>`;
when a tile becomes On while the sheet is open: status line `Tile ready: <title> is on the home screen` + a soft sound, NEVER a system popup
(no popups for tile mechanics, feedback-tile-notifications). Colours: On = the own colour; Failed/No answer/Stuck/No helper = warning red-orange; else mid.
Phase 9 dominates phases 1-8 and 0 while the helper is absent (a tracked ask keeps its record and resolves when the helper returns).
`can` (also in the page's JSON): Off/Failed/No answer/Stuck(add) -> `add`; On/Stuck(remove) -> `remove`; busy phases and No helper -> `none`.

Tile-start notifications (main-boot, translated via fe_i18n): missing game = `PS5SX2: the game of this home screen tile was not found (%s). Is its drive connected? The shelf opens instead.`
(%s = file name); invalid marker = `PS5SX2: this home screen tile doesn't name a game PS5SX2 can start. The shelf opens instead.`

OPTIONAL (MAY, after everything else, NOT part of acceptance): "Add and close PS5SX2". While phase is 2 (taken), 3 or 6, a second action on the row
closes PS5SX2 through a new `AppConfig::close_app` callback -> `orbis_exit_quietly(0)` in main-boot (no "no game" popup). Never offered while phase 1 (the file is
not taken yet). Needs new plumbing; without it the texts above carry the message.

---------------------------------------------------------------------------------------------------------------------------------------

## 9. Tile start (main-boot.cpp): markers and what to do with each, in order

1. Before the jailbreak, as today: read `/app0/disc-tile.txt` -> `tile_drive`, `s_tile_marker_seen`, `s_tile_marker_drive`, `s_tile_marker_st`. A game tile gives `tile_drive == "game"`. The reload branch (`"reload"`) is untouched.
2. NEW, right after, INDEPENDENT of step 1 (not guarded by `tile_drive.empty()`): read `/app0/game-tile.txt` (<= 1599 bytes). If it opens:
   `s_tile_is_game = true`; path = line 1 (`fe::gametile::ParseMarker`); `s_tile_game_id` = the `tile=` value; `s_tile_game_st` = its fstat; log
   `[boot] game tile marker (/app0/game-tile.txt before the jailbreak): <path> (tile id <id>)`. An empty/unreadable first line: the game path is `(unreadable)` (-> "invalid" later).
   If step 1 found no `disc-tile.txt` (`s_tile_marker_drive` empty) set `s_tile_marker_seen = true` and `s_tile_marker_drive = "game"` so that every
   `s_tile_marker_seen && !s_tile_marker_drive.empty()` test (leave plan, relaunch closing title, eboot watcher off, OrbisDiscNoteTile, open-shelf note) is true.
   `s_tile_marker_drive` MUST NOT be set to the game path (it is compared with disc markers; C9).
3. The cover prefetch (before the jailbreak) is skipped when `s_tile_is_game`.
4. After `open_shelf` is known: `if (s_game_path.empty() && !open_shelf && s_tile_is_game)`: `CheckLaunchPath` (absolute, no `..`, not /dev, an image name, not .elf, regular file, size > 0);
   for a `/mnt/` path retry 5 x 400 ms (a USB drive that is late); if good: `s_game_path = path`, `s_game_tile_started = true`, log `[tile] game tile: <path> starts`,
   `orbis_eventf`; if not: log, notification (missing vs invalid, section 8), the shelf opens. No serial fall-back search, no USB renumbering in v1.
5. `disc-launch.txt` block: `else if (s_game_tile_started) { log; }` before the file is read: the file is left for the next normal start.
6. Failed start: `if (frontend_ran || s_game_tile_started) orbis_restart_to_menu();`.
7. L1+R1+Touch: `orbis_own_tile_dir()` dispatches to the game search first when `s_tile_is_game`: candidates = `/data/homebrew/<tile= id>` when it is in PPSA99251..60, else the ten ids; a candidate counts when its `game-tile.txt`
   has the same inode + size + mtime as `s_tile_game_st` or the same first line; exactly one must count (never guessed) -> the closing title is that folder's name. Disc-tile code is untouched.
8. `orbis_own_tile_game()` (non-static) returns the game path when `s_tile_is_game`, else "" (feeds `AppConfig::own_tile_game`: the sheet and `/api/tile` refuse to REMOVE the tile this process runs from).
9. Nothing else changes for a disc tile or a normal start: `tile_game` is empty, `s_tile_is_game` and `s_game_tile_started` are false. `lastgame.txt` is NOT written at a tile start in v1 (C25).
