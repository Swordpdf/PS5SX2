/* Mount /data into homebrew app sandboxes, the ShadowMountPlus way: a nullfs
 * nmount of /user/data (the real /data partition, as seen from the system root
 * this payload runs with) onto <sandbox>/data, done from outside the sandbox.
 * ps-patch-system's mountUserData did the same "/user/data" -> "/data" mount
 * from inside ShellCore; ShadowMountPlus's mount_title_nullfs does its nullfs
 * mounts from a payload this way, which is the approach taken here.
 *
 * Hook-equivalence fixes (v2): the old ShellCore hook covered every sandbox
 * at creation. This watcher now matches it:
 *   - at startup it mounts every EXISTING PPSA-style slot too (no baseline
 *     skip: leftover slots from earlier runs were never mounted before);
 *   - it accepts ALL "TITLEID_NNN" slot shapes, not just PPSA######_NNN;
 *   - it polls at 50 ms instead of 200 ms to stay ahead of fast-starting
 *     apps;
 *   - self-healing: a slot whose /data is not this nullfs gets remounted.
 */
#include "mount.h"
#include "progress.h"

#include <dirent.h>
#include <errno.h>
#include <ps5/klog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define SANDBOX_BASE "/mnt/sandbox"
#define DATA_SOURCE "/user/data"
#define MOUNT_POLL_US 50000

#define IOVEC_ENTRY(x)                                                        \
  {(void *)(x),                                                               \
   ((const char *)(x) == NULL ? 0u : (size_t)(strlen((const char *)(x)) + 1u))}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))

/* "PPSA99203_000", "ITEM00001_007", "CUSA12345_050": TITLEID + '_' + digits.
 * Accepts any 4-9 char alphanumeric title id, then _NNN (1-4 digits). */
static int is_app_slot(const char *name)
{
    size_t n = strlen(name);
    size_t id = 0;
    while (id < n && name[id] != '_') {
        const char c = name[id];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z')))
            return 0;
        ++id;
    }
    if (id < 4 || id > 10 || id >= n || name[id] != '_')
        return 0;
    size_t digits = n - id - 1;
    if (digits < 1 || digits > 4)
        return 0;
    for (size_t i = id + 1; i < n; ++i)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    return 1;
}

/* True when <sandbox>/data is already the /user/data nullfs mount. */
static int data_mount_present(const char *dst)
{
    struct statfs st;
    if (statfs(dst, &st) != 0)
        return 0;
    return st.f_mntfromname[0] &&
           !strncmp(st.f_mntfromname, DATA_SOURCE, sizeof(DATA_SOURCE));
}

static int mount_data_into(const char *slot)
{
    char dst[192];
    if ((size_t)snprintf(dst, sizeof(dst), "%s/%s/data", SANDBOX_BASE, slot) >=
        sizeof(dst))
        return -1;

    if (data_mount_present(dst))
        return 0;

    if (mkdir(dst, 0777) != 0 && errno != EEXIST) {
        klog_printf("mount: mkdir %s: %s\n", dst, strerror(errno));
        return -1;
    }

    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(DATA_SOURCE),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    if (nmount(iov, IOVEC_SIZE(iov), 0) != 0) {
        klog_printf("mount: nullfs %s -> %s: %s\n", DATA_SOURCE, dst,
                    strerror(errno));
        return -1;
    }
    klog_printf("mount: %s -> %s mounted\n", DATA_SOURCE, dst);
    {
        char text[160];
        snprintf(text, sizeof(text), "data mounted: %s", dst);
        payload_progress(text);
    }
    return 0;
}

void *payload_mount_thread(void *arg)
{
    (void)arg;
    for (;;) {
        DIR *directory = opendir(SANDBOX_BASE);
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory)))
                if (is_app_slot(entry->d_name))
                    (void)mount_data_into(entry->d_name);
            closedir(directory);
        } else if (errno != ENOENT) {
            klog_printf("mount: opendir %s: %s\n", SANDBOX_BASE,
                        strerror(errno));
        }
        usleep(MOUNT_POLL_US);
    }
    return NULL;
}
