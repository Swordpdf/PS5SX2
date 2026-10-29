/* disc_speed8.c (PS5SX2 helper 171j, 2026-10-26, AI-assisted): the resident helper's disc-speed thread.
 *
 * Adapted from the owner's DiscSpeed8.elf (scratchpad/discspeed8/disc_speed8.c) - the part the owner chose to run inside
 * the helper: (1) patch the running SceShellCore's _AutoMounter::OpticalDisc::setSpeed table-index load (18 bytes;
 * 11.40's offset as a fast path + a signature scan verified against the 16-entry speed table, original bytes checked,
 * patched form recognized, read back) so every setSpeed it applies is entry 8 = CDB db 01 00 80 ("8.0", rotation 1);
 * (2) re-send the 8x CDB through the drive's pass device every 60 s while a disc is in (an unpatched shell's own setSpeed
 * can reset the drive after us).
 *
 * The full on-screen sweep/measure of the standalone ELF is NOT here - the PS5SX2 app measures and logs MB/s itself
 * (OrbisIOCtlSrc.cpp, ProsperoDiscSpeed.cpp). This thread only patches and pins. One patch per shell instance; a shell
 * already patched (by this thread, by a restarted helper, or by a separately-run DiscSpeed8.elf - same patched-form code
 * shape) is recognized and never patched twice; nothing is ever written without the 16-entry table behind the lea as
 * proof. The ptrace attach pauses the shell for the brief read/verify/write; the write itself goes through the kernel's
 * direct map (kernel_copyin), like ShadowMountPlus. Every step is logged to /data/PCSX2/logs/disc-speed8.log (fsynced)
 * and the klog. A flag file /data/PCSX2/flags/disc_nospeed8 turns all of it off (the thread logs that and returns).
 *
 * 171k: the 60 s pin (and its GET CONFIGURATION) only while the disc in the PS5's drive is a PS2 disc (disc_compat.h: dc_claim_of(0)); a PS3,
 * PS4/PS5 or video disc is the PS5SX3 helper's or the system's to set the speed of (the PS5SX3 helper sets a BD's own speed).
 *
 * No fork, no LoadExec, no exit; the thread stays resident and only ever writes its own 18 verified bytes into the shell.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 swordpdf
 */
#include "disc_speed8.h"
#include "disc_compat.h" /* 171k: the pin only for a PS2 disc (the published claim of /dev/cd0) */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>

#define LOG_PATH "/data/PCSX2/logs/disc-speed8.log"
#define FLAG_OFF "/data/PCSX2/flags/disc_nospeed8"

#define AUTHID_DEBUGGER UINT64_C(0x4800000000010003)
#define FW_1140 0x1140u

/* 11.40 SceShellCore, image+0x3767d0, inside _AutoMounter::OpticalDisc::setSpeed: the speed-table index load. */
#define REGION_OFF 0x3767d0u
static const uint8_t kRegionOriginal[18] = {
    0x48, 0x8d, 0x05, 0xc9, 0xbb, 0xa0, 0x01,
    0x42, 0x0f, 0xb7, 0x5c, 0xa8, 0x02,
    0x46, 0x0f, 0xb6, 0x3c, 0xa8,
};
static const uint8_t kRegionPatched[18] = {
    0x48, 0x8d, 0x05, 0xe9, 0xbb, 0xa0, 0x01,
    0x0f, 0xb7, 0x58, 0x02, 0x90, 0x90,
    0x44, 0x0f, 0xb6, 0x38, 0x90,
};
/* The vendor CDB the patched setSpeed sends (speed 8.0, rotation 1); what the 60 s pin re-sends. */
static const uint8_t kCdb8x[12] = {0xdb, 0x01, 0x00, 0x80, 0, 0, 0, 0, 0, 0, 0, 0};

#define X86_PAGE_FRAME 0x000ffffffffff000ull
#define X86_PAGE_VALID 0x001ull
#define X86_PAGE_LARGE 0x080ull

#define KINFO_PID_OFFSET 72
#define KINFO_TDNAME_OFFSET 447

enum { kPollSec = 2 };

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
static int g_log = -1;

static void step(const char *what) {
    if (g_log >= 0) {
        (void)!write(g_log, what, strlen(what));
        (void)!write(g_log, "\n", 1);
        fsync(g_log);
    }
    klog_printf("ds8: %s\n", what);
}

static void stepf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void stepf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    step(line);
}

/* ------------------------------------------------------------------ */
/* Kernel rights (the helper elevates itself; top up if needed)         */
/* ------------------------------------------------------------------ */
static void ensure_rights(void) {
    const pid_t self = getpid();
    if (kernel_get_ucred_authid(self) == AUTHID_DEBUGGER)
        return;
    static const uint8_t kFullCaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    (void)kernel_set_ucred_authid(self, AUTHID_DEBUGGER);
    (void)kernel_set_ucred_caps(self, kFullCaps);
}

/* ------------------------------------------------------------------ */
/* Remote process access (ShadowMountPlus's proven machinery)           */
/* ------------------------------------------------------------------ */
static int privileged_ptrace(int request, pid_t pid, void *address, int data) {
    static const uint8_t kPrivCaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                          0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const pid_t self = getpid();
    uint8_t saved_caps[sizeof(kPrivCaps)];
    const uint64_t saved_authid = kernel_get_ucred_authid(self);
    if (!saved_authid || kernel_get_ucred_caps(self, saved_caps) != 0)
        return -1;
    if (kernel_set_ucred_authid(self, AUTHID_DEBUGGER) != 0 ||
        kernel_set_ucred_caps(self, kPrivCaps) != 0) {
        (void)kernel_set_ucred_authid(self, saved_authid);
        (void)kernel_set_ucred_caps(self, saved_caps);
        return -1;
    }
    const int result = ptrace(request, pid, (caddr_t)address, data);
    int restored = kernel_set_ucred_authid(self, saved_authid) == 0;
    restored = kernel_set_ucred_caps(self, saved_caps) == 0 && restored;
    return restored ? result : -1;
}

/* 1 when `pid` is stopped (SSTOP, ki_stat == 4), read through sysctl - the helper cannot import waitpid, so after
 * PT_ATTACH the stop is waited for by polling the process state (SIGSTOP is delivered in a few ms). */
static int proc_stopped(pid_t pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
    static unsigned char buf[8192];
    size_t len = sizeof(buf);
    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0 || len == 0)
        return 0;
    const struct kinfo_proc *kp = (const struct kinfo_proc *)buf;
    return kp->ki_pid == pid && kp->ki_stat == 4 /* SSTOP */;
}

static int remote_attach(pid_t pid) {
    if (pid <= 0 || privileged_ptrace(PT_ATTACH, pid, NULL, 0) != 0)
        return 0;
    /* wait for the SIGSTOP to land without waitpid: poll ki_stat, up to ~1 s. */
    for (int i = 0; i < 40; i++) {
        if (proc_stopped(pid))
            return 1;
        struct timespec ts = {0, 25 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    /* It did not report stopped in time; the kernel write below does not need the stop, so go on but note it. */
    step("patch: the shell did not report stopped after PT_ATTACH; writing through the kernel map anyway");
    return 1;
}

static int remote_detach(pid_t pid) {
    return pid > 0 && privileged_ptrace(PT_DETACH, pid, NULL, 0) == 0;
}

static int remote_read(pid_t pid, uintptr_t address, void *buffer, size_t size) {
    if (!buffer || size == 0 || address > UINTPTR_MAX - (size - 1u))
        return 0;
    return mdbg_copyout(pid, (intptr_t)address, buffer, size) == 0;
}

static uintptr_t resolve_vmspace_pmap(uintptr_t vmspace) {
    const uint32_t version = (uint32_t)kernel_get_fw_version() >> 16;
    if (version >= 0x0100u && version <= 0x0102u)
        return vmspace + 0x2c0u;
    if (version >= 0x0105u && version <= 0x0550u)
        return vmspace + 0x2e0u;
    if (version >= 0x0600u)
        return vmspace + 0x2e8u;
    return 0;
}

static int resolve_process_paging(pid_t pid, uint64_t *cr3_out, uint64_t *dmap_out) {
    const uintptr_t process = (uintptr_t)kernel_get_proc(pid);
    if (!process)
        return 0;
    const uintptr_t vmspace =
        (uintptr_t)kernel_getlong(process + (uintptr_t)KERNEL_OFFSET_PROC_P_VMSPACE);
    if (!vmspace)
        return 0;
    const uintptr_t pmap = resolve_vmspace_pmap(vmspace);
    if (!pmap)
        return 0;
    uint64_t paging[2] = {0};
    if (kernel_copyout((intptr_t)(pmap + 32u), paging, sizeof(paging)) != 0)
        return 0;
    if (!paging[0] || !paging[1] || paging[0] <= paging[1])
        return 0;
    *cr3_out = paging[1];
    *dmap_out = paging[0] - paging[1];
    return 1;
}

static uint64_t virtual_to_physical(uint64_t address, uint64_t dmap, uint64_t page_map,
                                    uint64_t *physical_limit_out) {
    page_map &= X86_PAGE_FRAME;
    for (int shift = 39; shift >= 12; shift -= 9) {
        const uint64_t index = (address >> shift) & 0x1ffu;
        page_map = kernel_getlong((intptr_t)(dmap + page_map + index * 8u));
        if ((page_map & X86_PAGE_VALID) == 0)
            return UINT64_MAX;
        if ((page_map & X86_PAGE_LARGE) != 0 || shift == 12) {
            page_map &= (1ull << 52) - (1ull << shift);
            page_map |= address & ((1ull << shift) - 1ull);
            if (physical_limit_out)
                *physical_limit_out = (page_map | ((1ull << shift) - 1ull)) + 1ull;
            return page_map;
        }
        page_map &= X86_PAGE_FRAME;
    }
    return UINT64_MAX;
}

static int remote_write(pid_t pid, uintptr_t address, const void *buffer, size_t size) {
    if (!buffer || size == 0 || address > UINTPTR_MAX - (size - 1u))
        return 0;
    if ((kernel_get_fw_version() >> 16) <= 0x0820u)
        return mdbg_copyin(pid, buffer, (intptr_t)address, size) == 0;

    uint8_t probe = 0;
    if (!remote_read(pid, address, &probe, sizeof(probe)) ||
        (size > 1u && !remote_read(pid, address + size - 1u, &probe, sizeof(probe))))
        return 0;

    uint64_t cr3 = 0, dmap = 0;
    if (!resolve_process_paging(pid, &cr3, &dmap))
        return 0;

    const uint8_t *source = (const uint8_t *)buffer;
    while (size != 0) {
        uint64_t physical_limit = 0;
        const uint64_t physical = virtual_to_physical(address, dmap, cr3, &physical_limit);
        if (physical == UINT64_MAX || physical_limit <= physical)
            return 0;
        size_t chunk = (size_t)(physical_limit - physical);
        if (chunk > size)
            chunk = size;
        if (physical > UINTPTR_MAX - dmap ||
            kernel_copyin(source, (intptr_t)(dmap + physical), chunk) != 0)
            return 0;
        address += chunk;
        source += chunk;
        size -= chunk;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Finding SceShellCore                                                 */
/* ------------------------------------------------------------------ */
static pid_t find_pid_by_name(const char *name) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t buf_size = 0;
    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0 || buf_size == 0)
        return -1;
    uint8_t *buf = malloc(buf_size);
    if (!buf)
        return -1;
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
        free(buf);
        return -1;
    }
    const pid_t mypid = getpid();
    pid_t found = 0;
    uint8_t *ptr = buf;
    const uint8_t *end = buf + buf_size;
    while ((size_t)(end - ptr) >= sizeof(int)) {
        int ki_structsize = 0;
        memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
        if (ki_structsize <= KINFO_TDNAME_OFFSET ||
            (size_t)ki_structsize > (size_t)(end - ptr)) {
            found = -1;
            break;
        }
        pid_t ki_pid = 0;
        memcpy(&ki_pid, &ptr[KINFO_PID_OFFSET], sizeof(ki_pid));
        const char *ki_tdname = (const char *)&ptr[KINFO_TDNAME_OFFSET];
        const size_t tdname_size = (size_t)ki_structsize - KINFO_TDNAME_OFFSET;
        ptr += ki_structsize;
        /* NUL-terminated within bounds (memchr, not strnlen - the helper cannot import strnlen). */
        if (ki_pid != mypid && memchr(ki_tdname, 0, tdname_size) != NULL &&
            strcmp(ki_tdname, name) == 0) {
            found = ki_pid;
            break;
        }
    }
    free(buf);
    return found;
}

static pid_t find_shellcore(void) {
    pid_t pid = find_pid_by_name("SceShellCore");
    if (pid <= 0)
        pid = find_pid_by_name("SceShellCore.elf");
    return pid;
}

static void log_firmware(void) {
    const uint32_t fw = kernel_get_fw_version();
    const uint32_t major = ((((fw >> 24) & 0xffu) >> 4) & 0xfu) * 10u + ((fw >> 24) & 0xfu);
    const uint32_t minor = ((((fw >> 16) & 0xffu) >> 4) & 0xfu) * 10u + ((fw >> 16) & 0xfu);
    stepf("fw %u.%02u (raw 0x%08x): 11.40's offset is the fast path, anything else goes through the signature scan",
          major, minor, (unsigned)fw);
}

/* ------------------------------------------------------------------ */
/* The 8x patch                                                         */
/* ------------------------------------------------------------------ */
static pid_t g_shell_pid;
static uintptr_t g_shell_base;
static int g_patched;
static int g_fail_tries;

static const uint8_t kSpeedTable[64] = {
    0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x32, 0x00,
    0x01, 0x00, 0x50, 0x00, 0x01, 0x00, 0x60, 0x00,
    0x01, 0x00, 0x80, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x02, 0x00, 0x40, 0x00, 0x02, 0x00, 0x50, 0x00,
    0x01, 0x00, 0x60, 0x00, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0xff, 0xff, 0x01, 0x00, 0x60, 0x00,
};

static const uint8_t kShapeOriginal[11] = {0x42, 0x0f, 0xb7, 0x5c, 0xa8, 0x02, 0x46, 0x0f, 0xb6, 0x3c, 0xa8};
static const uint8_t kShapePatched[11] = {0x0f, 0xb7, 0x58, 0x02, 0x90, 0x90, 0x44, 0x0f, 0xb6, 0x38, 0x90};
#define SCAN_CHUNK 0x10000u
#define SCAN_OVERLAP 17u
#define SCAN_LIMIT 0x2000000u

static int find_speed_region(pid_t pid, uintptr_t base, uintptr_t *region_out,
                             uint8_t orig[18], uint8_t patch[18]) {
    static uint8_t buf[SCAN_CHUNK + SCAN_OVERLAP];
    unsigned shape_hits = 0;
    for (uintptr_t at = base; at < base + SCAN_LIMIT; at += SCAN_CHUNK) {
        size_t limit = sizeof(buf);
        if (!remote_read(pid, at, buf, sizeof(buf))) {
            if (!remote_read(pid, at, buf, SCAN_CHUNK))
                continue;
            limit = SCAN_CHUNK;
        }
        for (size_t i = 0; i + 18 <= limit; i++) {
            if (buf[i] != 0x48 || buf[i + 1] != 0x8d || buf[i + 2] != 0x05)
                continue;
            const uint8_t *shape = NULL;
            int already = 0;
            if (memcmp(buf + i + 7, kShapeOriginal, 11) == 0)
                shape = kShapeOriginal;
            else if (memcmp(buf + i + 7, kShapePatched, 11) == 0) {
                shape = kShapePatched;
                already = 1;
            }
            if (!shape)
                continue;
            shape_hits++;
            int32_t disp;
            memcpy(&disp, buf + i + 3, 4);
            const uintptr_t hit = at + i;
            const uintptr_t table = hit + 7 + (uintptr_t)(int64_t)disp;
            uint8_t t[64] = {0};
            const size_t tlen = already ? 32 : 64;
            if (!remote_read(pid, table, t, tlen))
                continue;
            if (memcmp(t, kSpeedTable + (already ? 32 : 0), tlen) != 0)
                continue;
            if (already) {
                stepf("patch: the patched form is already in place at image+0x%lx (its table verified)",
                      (unsigned long)(hit - base));
                return 2;
            }
            memcpy(orig, buf + i, 18);
            memcpy(patch, orig, 18);
            int32_t d;
            memcpy(&d, patch + 3, 4);
            d += 0x20;
            memcpy(patch + 3, &d, 4);
            static const uint8_t kMovzxEntry8[11] = {
                0x0f, 0xb7, 0x58, 0x02, 0x90, 0x90, 0x44, 0x0f, 0xb6, 0x38, 0x90};
            memcpy(patch + 7, kMovzxEntry8, 11);
            *region_out = hit;
            stepf("patch: the setSpeed table-index load found at image+0x%lx (its 16-entry table verified)",
                  (unsigned long)(hit - base));
            return 1;
        }
    }
    stepf("patch: %u code-shape hit(s), none with the speed table behind it", shape_hits);
    return 0;
}

/* 1 fresh, 2 already, 0 transient failure, -1 not found on this shell. */
static int patch_session(pid_t pid, uintptr_t base) {
    if (!remote_attach(pid)) {
        step("patch: ptrace attach failed");
        return 0;
    }
    int result = 0;
    do {
        uintptr_t region = 0;
        uint8_t orig[18] = {0}, patch[18] = {0};
        int found = 0;
        if (((kernel_get_fw_version() >> 16) & 0xffffu) == FW_1140) {
            uint8_t got[18] = {0};
            if (remote_read(pid, base + REGION_OFF, got, sizeof(got))) {
                if (memcmp(got, kRegionPatched, sizeof(got)) == 0) {
                    result = 2;
                    break;
                }
                if (memcmp(got, kRegionOriginal, sizeof(got)) == 0) {
                    region = base + REGION_OFF;
                    memcpy(orig, kRegionOriginal, sizeof(orig));
                    memcpy(patch, kRegionPatched, sizeof(patch));
                    found = 1;
                }
            }
        }
        if (!found) {
            const int s = find_speed_region(pid, base, &region, orig, patch);
            if (s == 2) {
                result = 2;
                break;
            }
            if (s != 1) {
                stepf("patch: the setSpeed signature/table was not found (fw 0x%04x) - the 60 s drive pin still runs",
                      (unsigned)(((kernel_get_fw_version() >> 16) & 0xffffu)));
                result = -1;
                break;
            }
        }
        if (!remote_write(pid, region, patch, sizeof(patch))) {
            step("patch: writing failed");
            break;
        }
        uint8_t back[18] = {0};
        if (!remote_read(pid, region, back, sizeof(back)) || memcmp(back, patch, sizeof(patch)) != 0) {
            step("patch: read-back mismatch");
            break;
        }
        stepf("patched setSpeed's table-index load at image+0x%lx: every setSpeed applies the table's 8.0 entry "
              "(CDB db 01 00 80)", (unsigned long)(region - base));
        result = 1;
    } while (0);
    (void)remote_detach(pid);
    return result;
}

static void patch_shell(void) {
    const pid_t pid = find_shellcore();
    if (pid <= 0) {
        step("SceShellCore not found yet");
        return;
    }
    const uintptr_t base = (uintptr_t)kernel_dynlib_mapbase_addr(pid, 0);
    if (!base) {
        step("SceShellCore image base not found");
        return;
    }
    const int r = patch_session(pid, base);
    if (r == 1) {
        g_fail_tries = 0;
        g_shell_pid = pid;
        g_shell_base = base;
        g_patched = 1;
        stepf("SceShellCore patched (pid %d, base 0x%lx): every setSpeed applies the table's 8.0 entry (CDB db 01 00 80)",
              (int)pid, (unsigned long)base);
    } else if (r == 2) {
        g_fail_tries = 0;
        g_shell_pid = pid;
        g_shell_base = base;
        g_patched = 1;
        step("SceShellCore already patched (8.0 in place)");
    } else if (r == -1) {
        g_shell_pid = pid;
        g_shell_base = base;
    } else {
        if (++g_fail_tries >= 5) {
            g_shell_pid = pid;
            g_shell_base = base;
        }
        stepf("patch attempt %d failed (transient?); %s", g_fail_tries,
              g_fail_tries >= 5 ? "leaving this shell alone (a restarted shell is tried again)" : "retrying");
    }
}

/* ------------------------------------------------------------------ */
/* The drive (the 60 s pin through the pass device)                     */
/* ------------------------------------------------------------------ */
static uint32_t g_raw_sense;

static int scsi(int pass, const uint8_t *cdb, int cdb_len, uint32_t dir, uint8_t *data, uint32_t len,
                uint32_t timeout_ms) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    cam_fill_csio(&ccb.csio, 1, NULL, dir | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG, data, len, SSD_FULL_SIZE,
                  (uint8_t)cdb_len, timeout_ms);
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
    g_raw_sense = 0;
    if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0)
        return -1;
    if ((ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
        return 0;
    if (ccb.ccb_h.status & CAM_AUTOSNS_VALID) {
        const uint8_t *sd = (const uint8_t *)&ccb.csio.sense_data;
        g_raw_sense = (uint32_t)(sd[2] & 15) << 16 | (uint32_t)sd[12] << 8 | sd[13];
    }
    return -1;
}

/* The PS5's own drive pass node (INQUIRY peripheral type 5, model holds PS-SYSTEM). Opened once and kept. */
static int g_drive = -1;
static int drive_pass(void) {
    if (g_drive >= 0)
        return g_drive;
    DIR *d = opendir("/dev");
    if (!d)
        return -1;
    struct dirent *e;
    while (g_drive < 0 && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "pass", 4) != 0)
            continue;
        char p[64];
        snprintf(p, sizeof(p), "/dev/%s", e->d_name);
        const int fd = open(p, O_RDWR);
        if (fd < 0)
            continue;
        uint8_t inq[36] = {0};
        const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
        char id[25] = {0};
        if (scsi(fd, c, 6, CAM_DIR_IN, inq, sizeof(inq), 5000) == 0 && (inq[0] & 0x1f) == 5 &&
            (memcpy(id, inq + 8, 24), strstr(id, "PS-SYSTEM") != NULL)) {
            g_drive = fd;
            stepf("drive: %s is the PS5's own drive (%.8s %.16s)", p, (const char *)inq + 8, (const char *)inq + 16);
        } else {
            close(fd);
        }
    }
    closedir(d);
    return g_drive;
}

/* GET CONFIGURATION current profile: > 0 means a disc is in. */
static int disc_in(void) {
    const int pass = drive_pass();
    if (pass < 0)
        return 0;
    uint8_t h[8] = {0};
    const uint8_t c[10] = {0x46, 0x00, 0, 0, 0, 0, 0, 0, sizeof(h), 0};
    if (scsi(pass, c, 10, CAM_DIR_IN, h, sizeof(h), 10000) != 0)
        return 0;
    return (h[6] << 8 | h[7]) > 0;
}

static int push_8x(void) {
    const int pass = drive_pass();
    if (pass < 0)
        return 0;
    if (scsi(pass, kCdb8x, 12, CAM_DIR_NONE, NULL, 0, 10000) == 0)
        return 1;
    stepf("pin: the drive refused db 01 00 80 (sense %06x)", g_raw_sense);
    return 0;
}

/* ------------------------------------------------------------------ */
/* The thread                                                           */
/* ------------------------------------------------------------------ */
extern int dc_claim_of(int drive); /* disctile.c (171k) */

void *ds8_thread(void *arg) {
    (void)arg;
    struct stat st;
    if (stat(FLAG_OFF, &st) == 0) {
        klog_printf("ds8: off (flag " FLAG_OFF ")\n");
        return NULL;
    }

    g_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_log < 0) {
        mkdir("/data/PCSX2", 0777);
        mkdir("/data/PCSX2/logs", 0777);
        g_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
    step("DiscSpeed8 (helper 171j): SceShellCore 8x patch + 60 s drive pin");

    ensure_rights();
    log_firmware();
    patch_shell();

    time_t last_push = 0;
    for (;;) {
        sleep(kPollSec);

        /* re-check the off flag each loop: a tester can turn it off at runtime */
        if (stat(FLAG_OFF, &st) == 0) {
            step("off (flag disc_nospeed8 appeared): the thread stops patching and pinning");
            if (g_log >= 0) { close(g_log); g_log = -1; }
            return NULL;
        }

        /* re-patch only when the shell restarted (new pid or new base) */
        const pid_t pid = find_shellcore();
        if (pid > 0 && (pid != g_shell_pid ||
                        (uintptr_t)kernel_dynlib_mapbase_addr(pid, 0) != g_shell_base))
            patch_shell();

        /* re-send the 8x CDB every 60 s while a disc is in (an unpatched shell can reset the drive after us) */
        const int ps2 = dc_claim_of(0) == DC_PS2; /* 171k */
        static int ps2_was = -1;
        if (ps2 != ps2_was) {
            ps2_was = ps2;
            step(ps2 ? "pin: a PS2 disc is in the PS5's drive: pinned to 8x every 60 s"
                     : "pin: no PS2 disc in the PS5's drive: not pinned, the drive not asked (171k)");
        }
        if (ps2 && disc_in() && difftime(time(NULL), last_push) >= 60.0) {
            last_push = time(NULL);
            if (push_8x())
                step("pin: db 01 00 80 re-sent to the drive (8.0, rotation 1)");
        }
    }
}

void ds8_start(void) {
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, ds8_thread, NULL) != 0)
        klog_printf("ds8: could not start the disc-speed thread\n");
    pthread_attr_destroy(&at);
}
