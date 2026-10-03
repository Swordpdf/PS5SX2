# RetroAchievements integration

Implementation notes (AI-assisted).

The PS5 Vulkan build now connects the existing PCSX2/rcheevos runtime to a
native HTTPS downloader, persistent account storage and PS5 notifications.
This is an initial softcore implementation. A PS5 build and real service
interaction have not yet been verified on hardware.

## Account screen

On the game shelf, press **Triangle** to open **RetroAchievements**. Select
**Username** or **Password** with the D-pad and Cross. On the local keyboard:

- D-pad selects a character; Cross types it.
- Triangle deletes the last character.
- Square shows or hides the password, including while typing. Passwords start
  hidden and return to hidden after closing the account screen or signing out.
  Submitting a login keeps the password in the local field so a failed attempt
  can be corrected and retried without retyping it.
- Circle returns to the account form.

Select **Sign in** to authenticate, or **Sign out** to remove saved credentials.
The keyboard uses QWERTY rows and supports printable ASCII, including spaces, upper/lower
case and punctuation. Passwords containing other Unicode characters require a
future text-input implementation.

Login runs on a worker thread. Duplicate submissions and leaving the account
screen are blocked while it is in progress, so the game cannot start concurrently
with the temporary authentication client. Network timeouts finish failed requests.
The UI retains the password after submission and clears it when the panel
closes or the user signs out. It is never persisted to account storage.

The shelf remains available even when the game directory is empty. Account
entry stays local to the console; no credentials are submitted through the
HTTP LAN settings server or external browser.

## Storage and runtime

`/data/PCSX2/achievements-secrets.ini` contains username, login timestamp and
the token returned by RetroAchievements. It uses the existing
`INISettingsInterface` atomic file replacement; POSIX temporary files have mode
0600, and an existing credential file is restricted to that mode on startup.
The password is not saved. This file is separate from `gs.ini`, which the web
server includes in downloadable reports.

A login whose credential save fails is reported as failed. The account worker
restores the prior credentials and base username in memory; the old atomically
saved file remains intact. Logout reports storage failure if removal cannot be
persisted. A remembered account is displayed as saved, not authenticated, until
the runtime confirms its token.

Base and secret settings layers are installed before the shelf. Shelf login
uses `Achievements::Login()` with a temporary client. After the shelf closes,
its worker is joined; the CPU initialization then creates the game runtime and
performs token login through the existing PCSX2 code.

`ProsperoHTTPDownloader.cpp` implements the common downloader interface using
`libSceHttp2`, with an independent context from the cover downloader. It
supports HTTPS GET and form-encoded POST, preserves HTTP error response bodies,
bounds response size, and aborts/joins workers before destroying requests or
contexts. Worker results are published to private buffers; only the polling
thread invokes callbacks or updates the common request payload. Plain HTTP
URLs are rejected, and TLS verification options remain at the system defaults
without enabling a validation bypass.

The console login failure was recorded before any HTTP response, with native
send result `0x8095f00c`; this does not establish invalid credentials. The
console also printed the resolver timeout as milliseconds and reset the
oversized value to its default. That message did not establish the setter's
actual ABI: test build 3's assumed 1000 ms value was rejected with
`0x817b11fe`, and its configuration check prevented sending the login request.
Test build 4 leaves DNS timeouts at system defaults, initializes NetCtl
independently of cover downloads, and logs native initialization/configuration/
send results and elapsed time without request URLs, bodies, passwords or tokens.
Local transport checks cover use of default DNS settings and offline
initialization cleanup. The overall request deadline still applies.
Successful console login still needs verification.

Test build 5 adds Google's public GTS Root R4 certificate to the private SSL
context using `sceSslLoadCert`, after build 4 still returned `0x8095f00c`
before an HTTP response. The live server chain verified against this root with
OpenSSL. The DER certificate comes from
https://pki.goog/repo/certs/gtsr4.pem and has SHA-256 fingerprint
`349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d`.
The SSL data descriptor and CA-list ABI follow the public bindings in
https://github.com/SvenGDK/SharpProspero/blob/5dfdb4b0b166e83dc9ae275001afb695f12c7174/src/SharpProspero/Interop/Net/Http.cs.
Local tests check the certificate bytes, context cleanup and certificate-load
failure. Certificate and hostname checks remain enabled; certificate loading
and successful authentication still require console validation. (AI-assisted)

On the console, build 5 rejected the DER CA load with `0x8095f005` before
creating an HTTP context. Cleanup then dereferenced a null downloader in
`Achievements::DestroyClient`, causing a crash. Build 6 makes client cleanup
safe after partial initialization and tries the same public certificate in
PEM format (without the trailing C-string terminator). This format is a console
compatibility experiment; it does not bypass TLS verification. The cleanup
function was checked with null, partially created and fully created resources
using AddressSanitizer and UndefinedBehaviorSanitizer (leak scanning disabled
because the sandbox disallows ptrace). Console certificate loading and login
still need verification. (AI-assisted)

Console validation of build 6 confirmed CA loading returned `0`, the login
POST returned HTTP `200` in 598 ms, and the user reported the account name
appearing on the shelf without an error. Together with the shelf state only
being saved after successful login and credential persistence, this confirms
the password-login path on this console. Reconnection after restart, game
identification and achievement unlock submission remain to be tested on
hardware. (AI-assisted)

Frame evaluation, memory reads, PS2 ELF hashing, request signing and award
submission remain in PCSX2/rcheevos. Notification availability is separated from
ImGui initialization for this build, allowing game summaries, unlocks and
disconnect/reconnect messages to use `OrbisNotifyRich()` without an ImGui
context. Badge icons are omitted pending shell validation. Unrelated emulator
OSD messages retain the existing behaviour.

Hardcore and ImGui overlays are forcibly disabled when core settings load.
Notification sounds are also disabled in this increment. Local unlock popups
are not proof that the server has acknowledged an award; reconnect messages
come from the runtime's existing handling of pending submissions.

## Local checks

```sh
bash ps5/coreorbis/tests/achievements/test.sh
CXX=clang++ CC=clang bash ps5/frontend/host/build-host.sh
```

The test script uses UBSan and synthetic data. It exercises the real common
downloader/native adapter with simulated console APIs, the controller form,
and the real account worker/INI persistence with simulated authentication.
It covers HTTP error bodies, HTTPS-only credential transport, callback thread,
timeouts, abort/join at shutdown, duplicate login, saved-account state,
permission bits, no persisted password, failed-save rollback, logout,
notification title/description mapping and filtering of unrelated OSD.
Transport and panel tests were also run with ASan/LeakSanitizer.

The host harness offers `--achievements-preview`, a UI-only account fixture
which never contacts RetroAchievements. Example:

```sh
VK_ICD_FILENAMES=/path/to/vk_swiftshader_icd.json \
  ps5/frontend/host/fe_host --data /tmp/ps5-ra-data --out /tmp/ps5-ra-shots \
  --achievements-preview "wait 0.5" "press triangle" "press cross" "shot keyboard"
```

The frontend built locally and its shelf, account form and keyboard were
visually checked using SwiftShader. Modified core/UI sources passed local
syntax checks with the PS5 feature macro. The complete signed eboot was not
built: the PS5 SDK and external driver dependencies are not configured in this
environment.

## Required console validation

Verify the SDK imports and ABI, especially `sceHttp2AddRequestHeader`, native
timeout/abort behaviour, TLS trust and hostname validation, and HTTPS after
the jailbreak. Then test actual login, restart/token login, expiry, game hash
matching for ISO/CHD, and a legitimate achievement reflected in the RA account.
Test connection loss during execution and pause, resume and application exit.
Do not advertise hardcore support until the port's settings/cheats/save-state
restrictions and client identification have been validated.

The account service is only exposed on the shelf. Runtime account changes or
a future web login endpoint need real CPU-thread dispatch; the current generic
`Host::RunOnCPUThread()` stub is still empty. No web login endpoint was added.
The User-Agent is currently `PS5SX2/1.0 (PS5)`; advance its numeric version as
the integration is released.

References: [rc_client integration](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration)
and the [SDK HTTP2 sample](https://github.com/ps5-payload-dev/sdk/blob/master/samples/http2_get/main.c).
