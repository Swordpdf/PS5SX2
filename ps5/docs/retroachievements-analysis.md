# RetroAchievements on PS5SX2

Integration analysis and proposal (AI-assisted).

Analyzed baseline: commit `d96a703`. This document describes the existing code
and a proposed implementation. It does not enable achievements or implement
login.

For the implementation status following this analysis, see
[RetroAchievements integration](retroachievements.md).
What PS5SX2 1.9 ships is described there, under "In PS5SX2 1.9".

## Conclusion

Integration is feasible by reusing `pcsx2/Achievements.cpp` and the rcheevos
library already compiled by the PS5 build. Achievement detection and submission
logic already exist; the missing pieces are platform services and an account
interface in the PS5 frontend. Enabling `EmuConfig.Achievements.Enabled` alone
does not address these gaps.

The first increment should support login from the main screen, token
persistence, game identification and softcore achievements, with console
notifications. Hardcore requires a separate validation stage.

## Existing port support and blockers

| Area | Repository evidence | Consequence / required change |
| --- | --- | --- |
| Library | `ps5/coreorbis/Makefile.vk` includes `Achievements.cpp` and `RCHEEVOS_SRCS` | Reuse the existing client without copying its logic into the frontend. |
| Activation | `ps5/coreorbis/main-boot.cpp` forces `EmuConfig.Achievements.Enabled = false` | Enable through the settings layer only after installing the necessary services; loading settings can overwrite direct changes to `EmuConfig`. |
| Core networking | `ps5/coreorbis/orbis-shims/stubs.cpp`: `HTTPDownloader::Create()` returns `nullptr` | Implement a real PS5 backend for GET and POST. Currently, `Achievements::CreateClient()` fails. |
| Cover networking | `ps5/frontend/fe_ps5.cpp`: `Http` uses `libSceHttp2` | Provides an API reference, but only implements GET, and its context is aborted/destroyed when leaving the game library. |
| Persistence | `main-boot.cpp` uses settings based on `MemorySettingsInterface` | `MemorySettingsInterface::Save()` returns an error; login requires explicit persistence. |
| Secrets | The port does not install `Host::Internal::SetSecretsSettingsLayer()` | The login callback accesses this layer to save the token. Install it before any login, including on the main screen. |
| Host | `Makefile.vk` compiles `tests/ctest/core/StubHost.cpp` | `CommitBaseSettingChanges()`, achievement callbacks and `RunOnCPUThread()` are empty. The UI cannot rely on them without implementing PS5 services. |
| UI | `ps5/coreorbis/orbis-shims/ProsperoUI.cpp` | OSD and `ImGuiFullscreen::AddNotification()` are empty. The existing unlock notification is lost. |
| PS5 notifications | `ProsperoNotify.cpp` / `ProsperoNotify.h` | `OrbisNotifyRich()` and `OrbisNotifyPlain()` already provide a queue and worker; they can display unlocks without blocking the emulated CPU. |
| Main screen | `ps5/frontend/fe_app.cpp` / `fe_app.h` | The game library handles its own drawing and input; the Qt login dialog is not used on PS5. |

The HTTPS observations before/after the jailbreak in `main-boot.cpp` and
`fe_ps5.cpp` make it necessary to test networking during emulation as well as
on the main screen. Downloading a cover does not prove that login and unlock
POST requests work at that stage.

## How an achievement is detected and submitted

1. **Authentication:** `Achievements::Login()` calls
   `rc_client_begin_login_with_password()`. The callback obtains `user->token`,
   saves the username/timestamp in base settings and the token in the secrets
   layer. The password does not need to be persisted. On subsequent startups,
   `Achievements::Initialize()` uses `rc_client_begin_login_with_token()`.
2. **Identification:** `GameChanged()` → `IdentifyGame()` → `GetGameHash()` reads
   the disc's ELF. It computes an MD5 of the executable name without the `;...`
   suffix, followed by up to 64 MiB of its bytes. It does not use the cover
   title, serial or CRC alone as the RA identifier.
   `rc_client_begin_load_game()` loads the set associated with the hash and
   the account state.
3. **Evaluation:** `VMManager::VSyncOnCPUThread()` calls
   `Achievements::FrameUpdate()`. After the ELF boots, the core runs
   `rc_client_do_frame()`. `ClientReadMemory()` provides exposed EE RAM and
   scratchpad in the memory map expected by RA. Do not move this evaluation
   to a UI timer or networking worker.
4. **Unlock:** in `3rdparty/rcheevos/src/rc_client.c`,
   `rc_client_award_achievement()` updates local state and prepares submission.
   The client also generates `RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED`, which
   PCSX2 forwards to `HandleUnlockEvent()`.
5. **Submission:** `rc_client_award_achievement_server_call()` supplies the
   username, token, achievement ID, hardcore mode and game hash to the API
   builder. `src/rapi/rc_api_runtime.c` constructs `awardachievement`, including
   a signature and, on retries, the time since the unlock. The port should
   leave this construction to the existing library.
6. **Transport:** `Achievements::ClientServerCall()` forwards the request to
   `HTTPDownloader`, which must return the status and body to the rcheevos
   callback. The client handles responses, updates scores and schedules
   retries for recoverable failures.
7. **Presentation:** `HandleUnlockEvent()` updates the summary and requests
   a popup and sound. This presentation is independent of server confirmation.

The local unlock event can occur before the network response. The UI must not
claim synchronization solely because it displayed a popup. Unofficial
achievements and spectator mode have paths that do not award achievements on
the server. Existing in-memory retries also do not constitute a durable queue
that survives application shutdown.

## Main-screen login

Add a visible **RetroAchievements** item to the game library, accessible with
the controller even when no games are available. One option is Triangle,
currently unused in the main input branch of `fe_app.cpp`, retaining Square
for settings and X/Options for launching a game.

Proposed behavior:

- No account: `RetroAchievements — Sign in`.
- On opening: a panel with username, masked password, `Sign in` and `Cancel`.
- Text entry: use the console's native keyboard if available in the SDK;
  investigate symbols/ABI before integrating. The current port does not
  implement `sceIme`/`ImeDialog`. A keyboard drawn by the frontend is an
  alternative for entering credentials directly with the controller.
- During the request: display `Signing in…`, prevent duplicate submissions
  and keep the frontend responsive. Also define behavior when cancelling or
  starting a game while authentication is pending.
- Success: display the authenticated username and allow `Sign out`.
- Failure: distinguish invalid credentials, unavailable networking and write
  errors. A username saved in a file does not prove successful authentication.
- Token rejected later: return to `Sign in again` without interrupting normal
  emulator operation.

The existing web server and QR code could provide a supplementary account
screen. Opening the RetroAchievements website and signing in there does
**not** provide `rc_client` with the emulator's session. Any supplementary form
must call the application's account service. The current local page uses HTTP;
do not send the RA password over the LAN by default. Prefer local input on
the console for the first increment.

## Proposed architecture

### HTTPS transport independent of cover downloads

Create `ps5/coreorbis/orbis-shims/ProsperoHTTPDownloader.cpp`, implement the
interface in `common/HTTPDownloader.h` and replace the empty factory in
`stubs.cpp`. Register the source in `Makefile.vk` and check the link scripts.

- GET for images/data and POST with `application/x-www-form-urlencoded` for
  API calls generated by rcheevos.
- A dedicated context whose lifetime covers library login and the running
  game; do not share the cover downloader's `g_http`.
- Blocking I/O only in workers. Complete requests and callbacks through
  downloader polling in the client's owning context.
- DNS/connection/send/read timeouts, response size limits, cancellation and
  shutdown that ensure requests and memory are released.
- Preserve HTTP error response bodies, which the library needs to explain
  failures. Map timeouts to `HTTP_STATUS_TIMEOUT`, already recognized by the
  core as a recoverable failure.
- Ensure workers finish before destroying requests/contexts; do not retain
  callbacks pointing to a destroyed client.
- Use TLS with certificate and hostname verification. Do not solve networking
  problems by disabling verification.
- Send a dedicated User-Agent with a numeric PS5SX2 version. The official
  guide uses this field for client support negotiation.

### Accounts, settings and threads

Install base and secrets settings **before** running the frontend. Currently,
these layers are installed in `main-boot.cpp` after game selection. Separate
persistent account configuration from per-game rendering settings.

Proposed files: `/data/PCSX2/achievements.ini` for non-secret state and
`/data/PCSX2/achievements-secrets.ini` for the token, with restricted permissions
and atomic writes. Do not put the token in `gs.ini`:
`fe_web.cpp::ApiReport()` includes that file in reports. Do not include
passwords/tokens in logs, web responses, URLs, reports or commits. Handle
persistence failures explicitly; the current callback marks success before
checking the write.

`Achievements::Login()` waits in `WaitForAllRequests()` and holds the
achievements lock. It can use a temporary client when the system is inactive,
which is useful for authentication before starting a game. Do not call it
inside `App::Update()`/`Build()` or the rendering loop. Run it in a controlled
account context and provide the UI with a copy of the result. Prevent VM
initialization from running concurrently with this flow, or wait for its
completion when transitioning to the game.

During core initialization, reuse the username/token and let the VM own the
emulation client. During gameplay, dispatch account/settings changes through
a queue actually processed by the CPU thread. An empty
`Host::RunOnCPUThread()` does not perform this work. Since the build uses
`RC_NO_THREADS=1`, do not rely on internal rcheevos locks to justify concurrent
access.

Implement `Host::OnAchievementsLoginRequested()`,
`OnAchievementsLoginSuccess()`, `OnAchievementsRefreshed()` and
`OnAchievementsHardcoreModeChanged()` to publish copied, synchronized state.
Avoid returning client pointers or strings to the UI that may be invalidated
by logout/game changes. Guard state reads with `Achievements::GetLock()` and
check `IsActive()` before calling getters that assume an existing client.

Real PS5 services should live in platform files; avoid turning
`tests/ctest/core/StubHost.cpp` into a generic production implementation.
Adjust build symbol selection/guards to avoid duplicate definitions and
preserve desktop tests.

### Notifications without ImGui

Forward unlocks to the `OrbisNotifyRich(title, description, icon)` queue; its
worker already falls back to a simple notification. Start with title and
description. Displaying badges from a URL/local path requires real PS5 API
testing; do not assume the shell accepts every cache path.

Audit all events in `Achievements.cpp`, including login, game loading,
completion, errors and reconnection. The `ImGuiFullscreen::AddNotification()`
adapter is one possible integration point, but a dedicated platform callback
can distinguish event types and options more precisely.

Do not call `DrawAchievementsWindow()`, `DrawGameOverlays()` or
`DrawPauseMenuOverlays()` without an ImGui context. These overlays access
`ImGui::GetIO()` directly. Keep `Overlays` and `LBOverlays` disabled for the
first increment and check callers. Separating UI rendering from achievement
evaluation allows the runtime to work with the current frontend.

## Implementation order and validation criteria

1. **Infrastructure:** downloader, persistence and layer initialization before
   the library. Test GET/POST, 4xx/5xx responses, timeouts, cancellation and
   shutdown with pending requests using a simulated server.
2. **Account:** main-screen item and login panel. Test incorrect passwords,
   repeated submissions, cancellation, write errors, restart with a token,
   expiration and logout. Confirm credentials are absent from reports/logs.
3. **Game:** enable `Achievements/Enabled` through the settings layer, keep
   hardcore disabled, compare ISO and CHD hashes with the desktop version,
   and test games without achievement sets and disc changes. No BIOS/images
   are required in the repository.
4. **Achievements:** reproduce conditions with synthetic memory/a fake server
   in tests; on the console, use a legally obtained game and confirm both the
   local event and the actual account record. Test loss and restoration of
   networking during gameplay and pause without stalling audio, controller
   input or emulation.
5. **Presentation:** confirm rich/fallback toasts, long/UTF-8 text and
   transitions from the library to gameplay without destroying account/network
   services.
6. **Hardcore, later:** audit save states, cheats/patches, memory manipulation
   and other port options while preserving existing core restrictions. Verify
   PS5SX2 identification/validation with RetroAchievements before announcing
   hardcore support. Do not assume the port inherits another client's
   validation solely because it uses PCSX2 code.

This analysis was based on reading the code and consulting the official guide.
No build, real login, achievement submission or PS5 hardware test was performed
at this stage. Networking after jailbreak, the native keyboard and shell badges
remain points requiring console validation.

## External reference

The [official rc_client integration guide](https://github.com/RetroAchievements/rcheevos/wiki/rc_client-integration)
confirms the separation between runtime, networking and UI, password login
followed by token reuse, and client identification through the User-Agent.
The hash, submission, callback and missing-service details above were checked
against the vendored version in this repository, which may differ from the
latest version covered by the guide.
