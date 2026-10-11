// PS5SX2 (2026-10-05, AI-assisted): the notification relay, a payload that sends one rich toast for the app.
//
// PS5SX2's own process can't load libSceNotification on firmware 11.40 or 12.00: sceKernelLoadStartModule answers
// 0x80020063 (rtld: "syscall load_prx failed due to 0x00000063"), the same refusal libSceKeyboard and libSceMouse
// get. A payload started by the console's ELF loader runs in a system process, where the library loads (the test
// payloads on the Pro, 2026-10-05). So ProsperoNotify.cpp embeds this program, writes the toast's JSON over
// g_payload's marker, and sends the whole file to the loader on 127.0.0.1:9021; the loader runs it as its own
// process, which posts the toast and exits.
//
// Built with the ps5-payload-dev SDK by Makefile.vk (it links libSceNotification) and embedded with .incbin.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdbool.h>
#include <unistd.h>

int sceNotificationSend(int userId, bool isLogged, const char* payload);

// The app looks for this marker in the file and writes the JSON over it, NUL-terminated. The array is initialized,
// so it lies in .data and its whole size is in the file: the room the app may fill. volatile: the compiler must
// read what the app wrote, not the marker it sees here.
__attribute__((used)) volatile char g_payload[16384] = "PS5SX2-NOTIFY-RELAY-JSON-V1";

int main(void)
{
	if (g_payload[0] != '{')
		return 1; // sent without a toast: nothing to do
	// 0xFE: SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM. Logged: a toast that isn't never shows (tested on the Pro).
	const int rc = sceNotificationSend(0xFE, true, (const char*)g_payload);
	// The library posts the toast from its own thread. The first relay returned at once: the exiting process
	// unloaded libSceNotification under that thread (SIGSEGV, a core dump, no toast; the Pro, 2026-10-05). So it
	// waits, then ends with _exit(), which stops every thread before anything is unloaded.
	sleep(3);
	_exit(rc == 0 ? 0 : 2);
}
