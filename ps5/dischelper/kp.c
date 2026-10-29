/* kp.c (helper 171e, 2026-10-15, AI-assisted).
 *
 * 1. kp_quiet(): the console's shutdown watch. The tester's five KP runs (a kernel panic at restart or power-off) end with the shell's restart sequence
 *    and, in the klog, nothing of ours is known to be innocent: this payload opens /dev/cd0..3 four times a second, asks the drive over /dev/pass0
 *    every few seconds, reads and writes app.db, calls ShadowMount+ and writes log files, all of it up to the last instant while the console resets the drive
 *    (the SCE_UMD_SYSTEM_EVENT_REBOOT phases). ShadowMount+ stops its own work when the state SceSystemStateMgrInfo says SHUTDOWN_ON_GOING; so does this
 *    payload now, and in rest mode (suspend / standby) too.
 * 2. kp_log_files(): a test build writes no log files (the klog only).
 * 3. The :9020 answerer (flag kp_stub9020): OnionHEN's runtime supervisor pings its private loader on 127.0.0.1:9020 ("onion:ping", answer
 *    "OK onion_elfldr 9020"). On the tester's console that loader never comes up, so the supervisor kills it and sends it again through :9021 every
 *    15..40 s, for ever, and in both KP runs one of those kills and relaunches fell inside the restart. When nothing answers on :9020 for 15 s this
 *    thread answers the ping, which is all the supervisor's steady state asks for; it never takes the port from a loader that answers.
 * 4. kp_game_quiet() / kp_game_quiet_due() (171f): while PS5SX2 plays a game FROM A DRIVE the payload looks at each drive once per 3 s instead of four times a
 *    second (the main loop) plus once a second (the disc watcher), and the watcher stops its USB scan. The app says so: it writes
 *    /data/PCSX2/logs/disc-playing.txt ("<unix time> <device> play") when it takes the drive and every 2 s while the game plays, and removes it at the release.
 *    Anything unclear (no file, an old time, garbage) means "no game": the old behaviour. Only ever less activity; kp_quiet() is not touched.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "kp.h"

#ifndef KP_HOST_TEST
#include <ps5/klog.h>
typedef intptr_t kp_evf_t;
int sceKernelOpenEventFlag(kp_evf_t *ef, const char *name);
int sceKernelPollEventFlag(kp_evf_t ef, uint64_t bit_pattern, unsigned int wait_mode, uint64_t *result_pattern);
int sceKernelCloseEventFlag(kp_evf_t ef);
#endif

#ifndef KP_FLAGS_DIR
#define KP_FLAGS_DIR "/data/PCSX2/flags"
#endif
#ifndef KP_STUB_PORT
#define KP_STUB_PORT 9020
#endif
#ifndef KP_STUB_CHECK_S
#define KP_STUB_CHECK_S 5 /* the answerer looks every 5 s, and takes the port after three looks in a row (15 s) without an answer */
#endif
#ifndef KP_TEST_NOLOG
#define KP_TEST_NOLOG 0
#endif
#ifndef KP_GQ_STAMP
#define KP_GQ_STAMP "/data/PCSX2/logs/disc-playing.txt" /* written by PS5SX2 (ProsperoDiscDump.cpp, OrbisDiscDumpClaim / AliveThread / OrbisDiscDumpRelease) */
#endif
#ifndef KP_GQ_FRESH_S
#define KP_GQ_FRESH_S 5 /* a stamp older than this belongs to a game that is over (the app renews it every 2 s) */
#endif
#ifndef KP_GQ_HOLD_MS
#define KP_GQ_HOLD_MS 1500 /* game quiet lasts this long after the last look that found a fresh stamp: a stamp that is just being replaced is not a game that ended */
#endif
#ifndef KP_GQ_SLOW_MS
#define KP_GQ_SLOW_MS 3000 /* in game quiet each drive is looked at once per this long */
#endif
#ifndef KP_GQ_READ_MS
#define KP_GQ_READ_MS 1000 /* the stamp is read once per this long at most, whoever asks */
#endif
#define KP_GQ_KEYS 8

#define KP_STATE_SHUTDOWN_ON_GOING 100u
#define KP_STATE_SUSPEND_ON_GOING 300u
#define KP_STATE_MAIN_ON_STANDBY 500u
#define KP_STATUS_SHELLUI_SHUTDOWN_IN_PROGRESS 0x0000000000200000ULL
#define KP_WAITMODE_OR 2u

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int kp_flag(const char *name) {
    char p[256];
    struct stat st;
    snprintf(p, sizeof(p), KP_FLAGS_DIR "/%s", name);
    return stat(p, &st) == 0;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---------------------------------------------------------------- 1. the shutdown watch */
static kp_evf_t g_info = -1, g_status = -1;
static int g_info_open, g_status_open, g_cfg_read, g_cfg_off, g_quiet, g_open_said;
static unsigned g_state = 0xffffffffu;
static uint64_t g_next_open_ms, g_next_poll_ms;

static const char *state_name(unsigned s) {
    switch (s) {
    case 0: return "INVALID";
    case 10: return "INITIALIZING";
    case 100: return "SHUTDOWN_ON_GOING";
    case 200: return "POWER_SAVING";
    case 300: return "SUSPEND_ON_GOING";
    case 500: return "MAIN_ON_STANDBY";
    case 1000: return "WORKING";
    default: return "other";
    }
}

static int poll_locked(uint64_t t) {
    if (!g_info_open && t >= g_next_open_ms) {
        const int rc = sceKernelOpenEventFlag(&g_info, "SceSystemStateMgrInfo");
        if (rc >= 0) {
            g_info_open = 1;
        } else {
            g_next_open_ms = t + 5000;
            if (!g_open_said) { g_open_said = 1; klog_printf("[kp] cannot open SceSystemStateMgrInfo (rc 0x%08x): the shutdown watch is off; trying again every 5 s\n", (unsigned)rc); }
        }
    }
    if (!g_status_open && g_info_open && t >= g_next_open_ms) {
        if (sceKernelOpenEventFlag(&g_status, "SceSystemStateMgrStatus") >= 0) g_status_open = 1;
        else g_next_open_ms = t + 5000;
    }
    int quiet = 0;
    if (g_info_open) {
        uint64_t pat = 0;
        if (sceKernelPollEventFlag(g_info, UINT64_MAX, KP_WAITMODE_OR, &pat) == 0) {
            const unsigned st = (unsigned)(pat & 0xffffu);
            if (st != g_state) {
                klog_printf("[kp] system state %u (%s) -> %u (%s)%s\n", g_state, g_state == 0xffffffffu ? "first look" : state_name(g_state), st, state_name(st),
                            (st == KP_STATE_SHUTDOWN_ON_GOING || st == KP_STATE_SUSPEND_ON_GOING || st == KP_STATE_MAIN_ON_STANDBY) ? ": this payload goes quiet" : "");
                g_state = st;
            }
            quiet = st == KP_STATE_SHUTDOWN_ON_GOING || st == KP_STATE_SUSPEND_ON_GOING || st == KP_STATE_MAIN_ON_STANDBY;
        }
    }
    if (g_status_open) {
        uint64_t pat = 0;
        if (sceKernelPollEventFlag(g_status, UINT64_MAX, KP_WAITMODE_OR, &pat) == 0 && (pat & KP_STATUS_SHELLUI_SHUTDOWN_IN_PROGRESS)) {
            if (!quiet) klog_printf("[kp] SceSystemStateMgrStatus: the shell's shutdown is in progress: this payload goes quiet\n");
            quiet = 1;
        }
    }
    return quiet;
}

int kp_quiet(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_cfg_read) {
        g_cfg_off = kp_flag("kp_off_quiesce");
        g_cfg_read = 1;
        if (g_cfg_off) klog_printf("[kp] flag kp_off_quiesce: the shutdown watch is off\n");
    }
    if (!g_cfg_off) {
        const uint64_t t = now_ms();
        if (t >= g_next_poll_ms) { /* the state is looked at ten times a second at most, whoever asks */
            g_next_poll_ms = t + 100;
            g_quiet = poll_locked(t);
        }
    } else {
        g_quiet = 0;
    }
    const int q = g_quiet;
    pthread_mutex_unlock(&g_mu);
    return q;
}

/* ---------------------------------------------------------------- 2. the log files */
/* 171i: the flag is looked at again every 10 s (it was read once, when the payload started: a kp_log made after the helper was loaded, or after a
 * reboot that loaded the helper first, gave no disc-auto.log until the next load: tester 2026-10-26, "it did not create a disc-auto.log"). */
int kp_log_files(void) {
    if (!KP_TEST_NOLOG) return 1;
    static int v = -1;
    static uint64_t next_ms;
    const uint64_t t = now_ms();
    if (__atomic_load_n(&v, __ATOMIC_RELAXED) < 0 || t >= __atomic_load_n(&next_ms, __ATOMIC_RELAXED)) {
        __atomic_store_n(&next_ms, t + 10000, __ATOMIC_RELAXED);
        __atomic_store_n(&v, kp_flag("kp_log"), __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&v, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------- 3. the :9020 answerer */
static int stub_connect(void) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(KP_STUB_PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno == EINPROGRESS) {
        struct pollfd p = {fd, POLLOUT, 0};
        if (poll(&p, 1, 1500) == 1) {
            int err = 0;
            socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) rc = 0;
        }
    }
    fcntl(fd, F_SETFL, fl);
    if (rc != 0) { close(fd); return -1; }
    return fd;
}

/* does something on :9020 answer OnionHEN's ping the way its private loader does? */
static int loader_answers(void) {
    const int fd = stub_connect();
    if (fd < 0) return 0;
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    char buf[64] = "";
    int ok = 0;
    if (send(fd, "onion:ping\n", 11, 0) == 11) {
        size_t got = 0;
        ssize_t r;
        while (got < sizeof(buf) - 1 && (r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0) {
            got += (size_t)r;
            if (memchr(buf, '\n', got)) break;
        }
        ok = got >= 20 && memcmp(buf, "OK onion_elfldr 9020", 20) == 0;
    }
    close(fd);
    return ok;
}

static int stub_listen(void) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(KP_STUB_PORT);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 8) != 0) {
        const int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static unsigned g_pings, g_others;

static void stub_serve(int c) {
    struct timeval tv = {1, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    char buf[64];
    const ssize_t n = recv(c, buf, sizeof(buf), 0);
    if (n >= 10 && memcmp(buf, "onion:ping", 10) == 0) {
        (void)!send(c, "OK onion_elfldr 9020\n", 21, 0);
        if (g_pings++ % 100 == 0) klog_printf("[kp] stub9020: answered OnionHEN's loader ping (%u so far)\n", g_pings);
    } else {
        (void)!send(c, "ERR stub\n", 9, 0); /* an ELF to run (its util daemon): this is no loader; its restart is "failed", as it was without us */
        if (g_others++ % 20 == 0) klog_printf("[kp] stub9020: a request that is no ping (%u so far): refused\n", g_others);
    }
    close(c);
}

static void *stub_thread(void *arg) {
    (void)arg;
    int misses = 0, lfd = -1, said_real = 0;
    for (;;) {
        if (lfd < 0) {
            if (loader_answers()) {
                if (!said_real) { said_real = 1; klog_printf("[kp] stub9020: a loader answers on :%d: staying out\n", KP_STUB_PORT); }
                misses = 0;
                sleep(KP_STUB_CHECK_S);
                continue;
            }
            said_real = 0;
            if (++misses < 3) { sleep(KP_STUB_CHECK_S); continue; }
            lfd = stub_listen();
            if (lfd < 0) {
                klog_printf("[kp] stub9020: cannot take :%d (errno %d): something holds the port and does not answer; trying again in %d s\n", KP_STUB_PORT, errno, 6 * KP_STUB_CHECK_S);
                misses = 0;
                sleep(6 * KP_STUB_CHECK_S);
                continue;
            }
            klog_printf("[kp] stub9020: nothing answered on :%d for %d s: this payload answers OnionHEN's loader ping from now on\n", KP_STUB_PORT, 3 * KP_STUB_CHECK_S);
        }
        struct pollfd p = {lfd, POLLIN, 0};
        const int pr = poll(&p, 1, 1000);
        if (pr <= 0) continue;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            klog_printf("[kp] stub9020: the listening socket died: looking again\n");
            close(lfd);
            lfd = -1;
            misses = 0;
            continue;
        }
        const int c = accept(lfd, NULL, NULL);
        if (c >= 0) stub_serve(c); /* the supervisor pings every 3 s; one client at a time is plenty. It keeps answering through a shutdown on purpose:
                                    * a silent port is what makes the supervisor kill and relaunch its loader */
    }
    return NULL;
}

/* ---------------------------------------------------------------- 4. game quiet (171f) */
/* A mutex of its own, never g_mu: kp_quiet() takes g_mu in every say() of every thread, and the stamp is a file on /data (one slow read must not stall the logs
 * or the shutdown watch). The file is read OUTSIDE the lock, by one thread at a time; the others use the cached answer. */
static pthread_mutex_t g_gq_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_gq_cfg_read, g_gq_cfg_off, g_gq_reading, g_gq_on;
static uint64_t g_gq_next_read_ms, g_gq_until_ms, g_gq_due_ms[KP_GQ_KEYS];
static char g_gq_text[160];

int kp_gq_parse(const char *text, long long now) {
    long long t = 0;
    char dev[128] = "", word[16] = "";
    if (!text) return 0;
    const int n = sscanf(text, "%lld %127s %15s", &t, dev, word);
    if (n < 1) return 0;
    if (n >= 3 && strcmp(word, "play") != 0) return 0; /* the app writes "play" only; any other word is not a game that plays from a drive */
    if (t < 0 || now < t || now - t > KP_GQ_FRESH_S) return 0; /* from the future, or old */
    return 1;
}

#ifdef KP_HOST_TEST
static unsigned g_gq_reads; /* the host test counts the looks at the stamp */
#endif

static int gq_read_stamp(char *buf, size_t max) {
#ifdef KP_HOST_TEST
    g_gq_reads++;
#endif
    FILE *f = fopen(KP_GQ_STAMP, "r");
    if (!f) return 0;
    const size_t n = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

int kp_game_quiet(void) {
    int read_now = 0, first = 0;
    uint64_t t = now_ms();
    pthread_mutex_lock(&g_gq_mu);
    if (!g_gq_reading && (!g_gq_cfg_read || (!g_gq_cfg_off && t >= g_gq_next_read_ms))) {
        g_gq_reading = 1;
        read_now = 1;
        first = !g_gq_cfg_read;
        g_gq_next_read_ms = t + KP_GQ_READ_MS;
    }
    pthread_mutex_unlock(&g_gq_mu);

    int off = 0, fresh = 0;
    char buf[160] = "";
    if (read_now) { /* the files are read here, outside the lock */
        if (first) off = kp_flag("gamequiet_off");
        if (!(first && off) && gq_read_stamp(buf, sizeof(buf))) fresh = kp_gq_parse(buf, (long long)time(NULL));
    }

    char said[160] = "";
    int say_on = 0, say_off = 0, say_cfg = 0;
    t = now_ms();
    pthread_mutex_lock(&g_gq_mu);
    if (read_now) {
        if (first) { g_gq_cfg_off = off; g_gq_cfg_read = 1; say_cfg = off; }
        if (fresh) {
            g_gq_until_ms = t + KP_GQ_HOLD_MS;
            snprintf(g_gq_text, sizeof(g_gq_text), "%s", buf);
            for (char *p = g_gq_text; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
            for (size_t k = strlen(g_gq_text); k && g_gq_text[k - 1] == ' '; k--) g_gq_text[k - 1] = '\0';
        }
        g_gq_reading = 0;
    }
    const int q = !g_gq_cfg_off && t < g_gq_until_ms;
    if (q != g_gq_on) {
        g_gq_on = q;
        if (q) {
            say_on = 1;
            snprintf(said, sizeof(said), "%s", g_gq_text);
            for (int k = 0; k < KP_GQ_KEYS; k++) g_gq_due_ms[k] = 0; /* every drive is looked at once at once, then once per KP_GQ_SLOW_MS */
        } else {
            say_off = 1;
        }
    }
    pthread_mutex_unlock(&g_gq_mu);
    if (say_cfg) klog_printf("[kp] flag gamequiet_off: the helper never goes game-quiet\n");
    if (say_on) klog_printf("[kp] game quiet ON (stamp: %s): each drive is looked at once per %g s\n", said, (double)KP_GQ_SLOW_MS / 1000.0);
    if (say_off) klog_printf("[kp] game quiet OFF: the drives are looked at as before\n");
    return q;
}

int kp_game_quiet_due(int key) {
    if (key < 0 || key >= KP_GQ_KEYS) return 1;
    if (!kp_game_quiet()) return 1;
    const uint64_t t = now_ms();
    pthread_mutex_lock(&g_gq_mu);
    const int due = t >= g_gq_due_ms[key];
    if (due) g_gq_due_ms[key] = t + KP_GQ_SLOW_MS;
    pthread_mutex_unlock(&g_gq_mu);
    return due;
}

#ifdef KP_HOST_TEST
static void __attribute__((unused)) kp_gq_test_reset(void) { /* the host test starts a case afresh (the flag is read at the first call again) */
    pthread_mutex_lock(&g_gq_mu);
    g_gq_cfg_read = g_gq_cfg_off = g_gq_on = 0;
    g_gq_next_read_ms = g_gq_until_ms = 0;
    memset(g_gq_due_ms, 0, sizeof(g_gq_due_ms));
    g_gq_text[0] = '\0';
    pthread_mutex_unlock(&g_gq_mu);
}
#endif

/* ---------------------------------------------------------------- start */
void kp_start(void) {
    klog_printf("[kp] helper test features: quiet at shutdown %s, log files %s, :9020 answerer %s, disc watcher %s, drive polling %s, game quiet %s\n",
                kp_flag("kp_off_quiesce") ? "OFF (flag kp_off_quiesce)" : "on", kp_log_files() ? "on" : "off (flag kp_log brings them back)",
                kp_flag("kp_stub9020") ? "on (flag kp_stub9020)" : "off", kp_flag("kp_nowatcher") ? "OFF (flag kp_nowatcher)" : "on",
                kp_flag("kp_nodrive") ? "OFF (flag kp_nodrive)" : "on", kp_flag("gamequiet_off") ? "OFF (flag gamequiet_off)" : "on");
    if (kp_flag("kp_stub9020")) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, stub_thread, NULL) != 0) klog_printf("[kp] the :9020 answerer could not be started\n");
        pthread_attr_destroy(&at);
    }
}
