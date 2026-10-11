# PS5SX2 Installer

A payload ELF, separate from the PS5SX2 Helper. Send it with the payload loader, like the helper.

- **Every time it's sent, it installs.** It checks the latest official release of `Swordpdf/PS5SX2` and downloads its `PS5SX2-*.zip` (1.0 used the test builds of `Swordpdf/PS5SX2TESTS`). The download's size and SHA-256 must match what GitHub publishes. The build then goes in with the rules below, and each step shows a notification.
- **The first copy sent after a boot stays running as the logger.** When PS5SX2 closes after a game, or after anything went wrong, that session's logs are sent to the log relay (see "Which sessions are sent"). A copy sent later in the same boot installs and exits, since the first copy holds `logger.lock`.
- **The no-log installer** (`PS5SX2Installer-nolog.elf`, `make ps5 NOLOG=1`) only installs, then exits. It has no logger in it, no report code and no relay address: nothing is kept or sent.

On consoles since 1.0 (2026-09-28), testers' included; 1.3 hasn't run on one yet.

## What it changes

| In the zip | On the console | Rule |
|---|---|---|
| `PPSA99203/…` | `/data/homebrew/PPSA99203/…` | Replaced. The old file goes into `backup/` first. `eboot.bin` is placed last. |
| `PCSX2/resources/…` | `/data/PCSX2/resources/…` | Replaced, with a backup. |
| `PCSX2/bios`, `games`, `memcards`, `savestates`, `textures`, `covers`, `cache`, `logs`, … | same | The folder is created if missing. Nothing is ever written inside. |
| `PCSX2/flags/…` | `/data/PCSX2/flags/` | First install: all of them. Later: only switches new in a build. Never one the user removed or moved to `flags-off/`. |
| `gs.ini`, `live.ini`, `settings/*.ini`, `patches`, `cheats`, anything else under `PCSX2/` | same | Added when missing. Replaced only if the file on the console is byte for byte one the installer put there (`manifest.txt`) or an earlier build's copy (`history/`). A file the user changed or deleted stays that way. |
| top-level texts (`SETUP.txt`, licences) | `/data/PS5SX2-Installer/release-notes/<tag>/` | Always written. |
| anything else | — | Skipped. |

It never deletes anything outside `/data/PS5SX2-Installer`.

**Staying safe if it stops half-way.** Every move is recorded in `journal.txt`, and the record is fsync'ed before the move happens. Each record carries a checksum.

If power is cut or the process dies, the next run undoes the half-done install. The undo checks file contents before touching anything:

- a file is moved away only if it is exactly the one the install put there;
- a backup comes back only if it is exactly the file that was there before;
- a file changed since the crash stays as it is.

The undo can run any number of times. A cut after the `COMMIT` record just finishes the install.

**Won't install when:**

- PS5SX2 is running (from `/data/PCSX2/pid.txt`; one older than the last boot is ignored);
- the installed build is the same as the release or newer (read from the build tag inside `eboot.bin`).

## Switches

Empty files in `/data/PS5SX2-Installer/`:

- `no-install`: only the logger runs.
- `no-log-upload`: no logger.
- `send-shelf-logs`: also send the sessions that only showed the shelf and ended normally (1.3 leaves them out).
- `reinstall`: install even when up to date. It is used up once (renamed `reinstall.used`).
- `tester-name.txt`: a name that appears on the reports.
- `upload-url.txt`: sends reports to another relay.

## Log reports

One text file per session is built from:

- the session's lines in `settings.log`;
- `boot.log`, `emulog.txt` and `stderr.log`, with the right set picked by `[boot] pid=N`;
  - they're in `/data/PCSX2/logs/` when that folder exists, and otherwise in `/data/PCSX2/` itself, as on a console
    set up from a release zip, which has no `logs/` folder. Both places are searched. 1.0 and 1.1 only looked in
    `logs/`, so their reports from such consoles had no logs, build "unknown" and "the shelf";
  - without a `settings.log` line, the build and the game come from the boot log;
- a GPU hang dump, if one was written during the session;
- `gs.ini`, `live.ini` and the game's own settings;
- the switch names;
- the end of the helper's log.

IPv4, IPv6 and MAC addresses and the settings page's token are removed, and the game list isn't included. Reports wait in `outbox/`, at most 20 files or 60 MB, until the relay takes them.

**No notifications from the logger (1.3).** 1.2 showed one when the logger started ("log upload is on") and one after every upload; both are gone, and `installer.log` records what the logger does. So nothing on the screen tells a tester that logs are uploaded: tell them when they get this ELF, and point those who don't want it to the no-log one (or the `no-log-upload` switch). The logger still shows a notification when it can't run, and when `no-log-upload` switches it off while it runs.

**Which sessions are sent (1.3).** A session that only showed the shelf and ended normally is not sent: no game was started, nothing went wrong, and there is no tester note. Everything else is:

- a game was started (from `settings.log`'s `game start:` line, or the boot log's `[boot] game:`);
- a crash, a GPU hang, or a game that didn't start, in `settings.log` or in the boot log, or a GPU hang dump written during the session;
- an empty game list (`0 disc image(s)` in the boot log);
- a tester note;
- a session whose logs weren't found at all. It is sent as `no-logs`, with the game "unknown". This happens after a very early crash, or when the logs were rotated away.

The next report's header counts the sessions left out since the last one. The `send-shelf-logs` switch sends them all.

**The relay.** `worker/` is a Cloudflare Worker that posts each report to a private Discord channel. The webhook URL is a Worker secret, never in the ELF, and the Worker rate-limits per console and per network address. It runs at `https://ps5sx2-logs.ps5sx2.workers.dev` (swordpdf's Cloudflare account), and `make ps5` builds that address into the ELF. `make ps5 RELAY_URL=` builds an ELF that only keeps reports in `outbox/`; `upload-url.txt` points one console somewhere else.

To set up a relay:

1. Discord: the log channel's settings, then Integrations, Create Webhook. Copy its URL.
2. On Windows, run `worker\deploy.cmd` (needs Node.js). It signs in to Cloudflare, deploys the Worker and asks for the webhook URL. Elsewhere: `cd worker && npx wrangler login && npx wrangler deploy && npx wrangler secret put DISCORD_WEBHOOK_URL`.
3. Build with `make ps5 RELAY_URL=https://ps5sx2-logs.<account>.workers.dev/v1/logs`.

The rate limits can't be set from the Cloudflare dashboard, so make changes with wrangler (running `deploy.cmd` again also swaps in a new webhook).

## Build and test

```
./fetch-deps.sh          # Mbed TLS 3.6.7 and miniz 3.1.1, SHA-256 checked, into $DEPS
make ps5                 # build/ps5/PS5SX2Installer.elf (payload SDK in $PS5_PAYLOAD_SDK)
make ps5 NOLOG=1         # build/ps5-nolog/PS5SX2Installer-nolog.elf: installs only, no logger
make test                # Linux build with ASan/UBSan against a local HTTPS server standing in for GitHub
```

The tests cover:

- a fresh console, and one laid out like the real console listing;
- 112 then 113, with user edits, deletions and parked switches;
- malicious or broken zips, and GitHub errors;
- a crash at every step of an install, of a first install, and of the undo itself;
- lost or cut-off journal records, and the commit window;
- other and full drives (an `LD_PRELOAD` shim fakes EXDEV and ENOSPC);
- links in the work folder, and a stale `pid.txt`;
- the logger: redaction, short sessions, and relaunches between checks;
- which sessions are sent (shelf only, empty game list, crashes in either log, tester notes, no logs, the switch), and no notification at start or when sending;
- the no-log build: it installs, then leaves nothing behind and sends nothing.

The CA list (`ca/cacert.pem`) is Mozilla's, as bundled in Node.js; `ca/make-cacert.sh` rebuilds it. `history/` holds the config files and switches that test builds before 112 shipped.
