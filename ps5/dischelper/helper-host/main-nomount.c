/* NO-DATA-MOUNT VARIANT (2026-10-14, AI-assisted): main.c without the /data mount thread. ShadowMount+ 1.7-beta5 and later mounts /data into the
 * sandbox of every title it manages (PS5SX2 and the tiles) when it starts and takes it off again when the game stops ("[FAKELIB] directory mounted /
 * unmounted: /data -> /mnt/sandbox/<ID>_000/data" in its debug log), so the helper's own nullfs mount (mount.c) lies on top of it as a second layer
 * and is never taken off: the sandbox then cannot be removed after the game, and ShadowMount+ stays "busy". With ShadowMount+ 1.7beta3, which does
 * not mount /data, use the normal main.c. Original header follows.
 *
 * The PS5SX2 helper: the /data mount and the app jailbreak from onionHEN,
 * combined with the disc payload (build 170: the tile maker and the disc
 * watcher, both embedded) — one ELF. The installer stays in the bot
 * (PS5SX2Payload.elf); this is the helper that replaces PS5SXHelper.elf
 * and the standalone PS5SX2-Disc.elf together.
 *
 * The host carries the rights the disc payload needs (authid
 * 0x4800000000010003 and full caps: /dev/cd*, /dev/pass*, app.db,
 * notifications), the resident lock, the /data mount watcher, the
 * onionHEN-style jb watcher (built-ins PPSA99203, PPSA99303 and the eight
 * tile slots PPSA99231..PPSA99238, plus /data/whitelist.txt), and the
 * disc payload thread (disctile.c's loop + the watcher thread inside it).
 *
 * Do NOT send the standalone PS5SX2-Disc.elf (or the old DiscLaunch.elf /
 * DiscTile.elf) together with this one: both would act on every disc.
 */
#include "jb.h"
#include "progress.h"

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

/* disctile.c (build 170), already trimmed to a resident thread. */
void *disc_payload_thread(void *arg);

void payload_progress(const char *text)
{
    klog_printf("payload: %s (pid=%d)\n", text, (int)getpid());
    int fd = open("/data/PCSX2/logs/payload-init.log",
                  O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0666);
    if (fd < 0)
        return;
    char line[300];
    int n = snprintf(line, sizeof(line), "%ld pid=%d %s\n", (long)time(NULL),
                     (int)getpid(), text);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n
                                                  : sizeof(line) - 1;
        (void)!write(fd, line, len);
        fsync(fd);
    }
    close(fd);
}

static int acquire_resident_lock(void)
{
    int fd = open("/data/PS5SXHelper.lock", O_CREAT | O_RDWR | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        klog_printf("helper: cannot open instance lock: %s\n",
                    strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        klog_printf("helper: another copy is resident: exiting\n");
        payload_progress("another copy is resident: exiting");
        close(fd);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    /* Rename ourselves in the process list (elfldr hardcodes argv[0] =
     * "payload.elf" for raw ELF sends). */
    static char proc_name[] = "ps5sx2daemon.elf";
    (void)argc;
    if (argv != NULL && argv[0] != NULL)
        argv[0] = proc_name;

    payload_progress("helper started (jb + disc payload; NO /data mount thread: ShadowMount+ does the /data mount)");

    const int lock_rc = acquire_resident_lock();
    if (lock_rc != 0)
        return lock_rc == 1 ? 0 : 1;
    payload_progress("resident lock acquired");

    /* The disc payload's rights: /dev/cd*, /dev/pass*, app.db, notifications
     * (what the standalone PS5SX2-Disc.elf's main would set). */
    {
        uint8_t all[16];
        memset(all, 0xff, sizeof(all));
        (void)kernel_set_ucred_authid(getpid(), 0x4800000000010003ull);
        (void)kernel_set_ucred_caps(getpid(), all);
    }
    payload_progress("credentials set (disc payload)");

    /* no /data mount thread in this build (see the header): ShadowMount+ 1.7-beta5 and later mounts /data into the sandboxes itself */
    payload_progress("no /data mount thread in this build (ShadowMount+ mounts /data into the sandboxes itself)");

    {
        pthread_t t;
        if (pthread_create(&t, NULL, payload_jb_thread, NULL) == 0) {
            (void)pthread_detach(t);
            payload_progress("jb thread started (built-ins 99203, 99303, "
                             "99231-99238 + whitelist.txt)");
        } else {
            payload_progress("jb thread FAILED");
        }
    }

    {
        /* the disc payload (disctile.c's loop; it starts the disc watcher
         * thread itself with a 2 MB stack) */
        pthread_attr_t attr;
        pthread_t t;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4 << 20); /* tile.c's art is on it */
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, disc_payload_thread, NULL) == 0) {
            payload_progress("disc payload thread started");
        } else {
            payload_progress("disc payload thread FAILED");
        }
        pthread_attr_destroy(&attr);
    }

    klog_printf("helper: resident: jb daemon and disc payload started (no /data mount)\n");
    payload_progress("resident ready");

    for (;;) {
        pause();
    }
}
