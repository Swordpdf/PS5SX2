/* kp.h (helper 171e, 2026-10-15, AI-assisted; game quiet added in 171f, 2026-10-16): what the helper does while the console shuts down, the switch for its log
 * files, an optional answerer for OnionHEN's private-loader ping, and how little it touches the drives while a game plays from one. See kp.c. Everything here
 * is cheap and safe to call from any thread. */
#ifndef KP_H
#define KP_H

/* 1 while the console is shutting down, going to rest mode or suspending (SceSystemStateMgrInfo, the way ShadowMount+ reads it): the payload then
 * opens no device, writes no file, asks nobody (ShadowMount+, the shell's notifications, app.db) and only waits. Back to 0 when the console works again.
 * The flag kp_off_quiesce (an empty file in /data/PCSX2/flags, read at the first call) turns this off. */
int kp_quiet(void);

/* 1 when the payload writes its log files (disctile.log and the copies of the kernel and ShadowMount+ logs, disc-auto.log). A KP test build
 * (-DKP_TEST_NOLOG=1) writes none unless the flag kp_log exists; its lines go to the klog only. Every other build always writes them. */
int kp_log_files(void);

/* An empty file /data/PCSX2/flags/<name>. */
int kp_flag(const char *name);

/* 171f, "game quiet": 1 while PS5SX2 says a game plays FROM A DRIVE (not a copy): the app writes /data/PCSX2/logs/disc-playing.txt ("<unix time> <device> play"
 * at the claim of the drive and every 2 s while it plays, taken away at the release; a game whose process just ends leaves it to age out). The stamp is read at most
 * once a second and the answer is cached; a stamp
 * older than 5 s, a missing, empty or garbled one, a time in the future, or the flag gamequiet_off (an empty file in /data/PCSX2/flags, read at the first call)
 * all mean 0: the helper then behaves as it always did. It only ever REDUCES what the payload does, and it never touches kp_quiet(). */
int kp_game_quiet(void);

/* In game quiet: 1 when it is this key's turn (once per 3 s; the keys are the drive polls of the main loop 0..3 and the disc watcher 4), so that each drive is looked
 * at once per 3 s instead of four times a second. Outside game quiet always 1; a key outside 0..7 gives 1. */
int kp_game_quiet_due(int key);

/* The stamp's text (pure, for the host test): 1 when the line "<time> [<device> [play]]" is no older than 5 s and not from the future, else 0. */
int kp_gq_parse(const char *text, long long now);

/* Once at the start: says in the klog which of the test features are on, and starts the :9020 answerer when the flag kp_stub9020 exists. */
void kp_start(void);

#endif
