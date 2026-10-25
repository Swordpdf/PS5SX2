/* switch.txt (2026-10-14, AI-assisted): one file, /data/PCSX2/switch.txt, with a single digit, for the notifications about the DISC TILES only.
 *   0 (the default) = no notification about the tiles (a tile is ready, a tile comes later, the payload's start-up line, "press its tile", the clean-up steps)
 *   1               = they are shown too
 * Everything else keeps its notifications whatever the file says (errors, copy and dump progress, "PS5SX2: starting ...", the helper's own popups, the
 * result of a reload). The payload writes the file with 0 when it is not there. It is read again when it changes (at most once a second), so editing it
 * over FTP takes effect at once, with no need to load the payload again. Logging is not affected: everything is always written to the logs.
 * Included by both disctile.c and daemon/disc_launch.c (the two parts of the payload), hence all static. */
#ifndef DISC_SWITCH_H
#define DISC_SWITCH_H

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SWITCH_DIR  "/data/PCSX2"
#define SWITCH_FILE SWITCH_DIR "/switch.txt"

static pthread_mutex_t g_switch_mu = PTHREAD_MUTEX_INITIALIZER;

/* writes "0" when the file is not there (an existing file is never touched) */
__attribute__((unused)) static void switch_ensure_file(void) {
    struct stat st;
    if (stat(SWITCH_FILE, &st) == 0) return;
    mkdir(SWITCH_DIR, 0777);
    FILE *f = fopen(SWITCH_FILE, "w");
    if (f) { fputs("0\n", f); fclose(f); }
}

/* 1 when switch.txt says 1 (its first digit; anything else, a missing file too, is 0) */
__attribute__((unused)) static int switch_tile_notices_on(void) {
    static int value;
    static time_t checked;
    pthread_mutex_lock(&g_switch_mu);
    const time_t now = time(NULL);
    if (checked == 0 || now != checked) {
        checked = now;
        int v = 0;
        FILE *f = fopen(SWITCH_FILE, "r");
        if (f) {
            char b[32] = "";
            if (fgets(b, sizeof(b), f)) {
                for (const char *p = b; *p; p++) {
                    if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
                    v = (*p == '1');
                    break;
                }
            }
            fclose(f);
        }
        value = v;
    }
    const int r = value;
    pthread_mutex_unlock(&g_switch_mu);
    return r;
}

#endif
