// PS5SX2 (vk-285-73): PS5 notifications, sent from a worker thread so a caller (the pad thread, the
// CPU thread) never waits on the system. See ProsperoNotify.cpp.
#pragma once

#include <string>
#include <sys/stat.h>

// The kernel toast (sceKernelSendNotificationRequest): text only, what the port has used since the
// start ("PS5SX2: starting", "Now playing: ...").
void OrbisNotifyPlain(const char* text);

// Rich toasts posted from now on wait until `seconds` have passed (main-boot, as a game starts: the first two toasts of
// a game, sent in its first seconds, showed no picture). Rich toasts are also spaced 6.5 s apart. (2026-10-05, AI-assisted)
void OrbisNotifyHold(int seconds);

// System sounds a rich toast can ask for (its rawData.soundEffect; AI-assisted). Heard on a PS5 Pro with a test
// payload (2026-10-05): the trophy ding and the platinum trophy sound. "none" is silent; no sound id (nullptr)
// leaves the system's own choice, the usual notification sound.
#define ORBIS_TOAST_SOUND_TROPHY "psfx_trophy_toast"
#define ORBIS_TOAST_SOUND_PLATINUM "psfx_platinum_trophy_toast"
#define ORBIS_TOAST_SOUND_NONE "none"

// Notification channels (rawData.channelType). The PS5's notification settings and Do Not Disturb filter toasts
// by channel: "Trophies" follows the user's trophy-notification settings, like a real trophy; "ServiceFeedback"
// always shows. Both showed during a game on the Pro; "Downloads" (the SDK sample's) did not.
#define ORBIS_TOAST_CHANNEL_TROPHIES "Trophies"
#define ORBIS_TOAST_CHANNEL_SERVICE "ServiceFeedback"

// A rich toast (libSceNotification's sceNotificationSend with a JSON payload): a message, a second line and an
// icon, given as a PNG path the system UI can read (/data/... or /user/data/...) or an https URL, which the system
// UI fetches itself. sound: one of the ORBIS_TOAST_SOUND_* ids, or nullptr for the system's default; channel:
// nullptr for ORBIS_TOAST_CHANNEL_SERVICE. Falls back to the kernel toast when the library or the call fails.
void OrbisNotifyRich(const char* message, const char* sub_message, const char* icon, const char* sound = nullptr,
	const char* channel = nullptr);

// pr9n (AI-assisted): the same rich toast sent from the calling thread at once, without the queue or its pacing, for a
// last word before the app closes (an unlock that couldn't be sent, OrbisFlushBeforeExit). Waits for a send already in
// progress, then for its own (the relay through the ELF loader takes ~3 s).
void OrbisNotifyRichNow(const char* message, const char* sub_message, const char* icon, const char* sound = nullptr,
	const char* channel = nullptr);

// The picture for a rich toast (AI-assisted): a cached image by its path when the file is there, otherwise an
// https URL for the system UI to fetch, otherwise none, since a missing picture leaves an empty frame. Both kinds
// showed on the Pro; how a slow or failed fetch looks in the toast needs proper testing.
inline std::string OrbisToastIcon(const std::string& path, const std::string& url = {})
{
	struct stat st;
	if (!path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
		return path;
	if (url.rfind("https://", 0) == 0)
		return url;
	return {};
}
