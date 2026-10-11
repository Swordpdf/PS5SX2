// Orbis opendir/readdir/closedir backed by sceKernelOpen + sceKernelGetdents.
// The libc opendir in libSceLibcInternal returns EPERM in the bigapp sandbox;
// the direct kernel path is proven working (native-iso-probe-03).
//
// vk-285-112: each open directory has its own dirent, as POSIX has it (valid until the next readdir on the
// same DIR, or closedir). Until 111 one thread-local dirent served every open directory, so a walk that
// reads a subfolder while its parent is open lost the parent's entry: PCSX2's recursive FindFiles passes
// the parent's d_name down as the subfolder's name, the subfolder's first readdir overwrote it, and every
// path in the subfolder came out as <folder>/<name>/<name>. Texture replacement packs (files in subfolders
// of textures/<serial>/replacements) were never found.
// vk-285-135: folders under /nfs (games on NFS shares) are listed by OrbisNfs.cpp.
#include <sys/dirent.h>
#include <cstddef>
#include <cstring>
#include <fcntl.h>

#include "OrbisNfs.h"

extern "C" int sceKernelOpen(const char*, int, int);
extern "C" int sceKernelGetdents(int, char*, int);
extern "C" int sceKernelClose(int);

typedef struct __dirstream DIR;

static_assert(offsetof(dirent, d_fileno) == 0 && sizeof(dirent::d_fileno) == 4);
static_assert(offsetof(dirent, d_reclen) == 4 && sizeof(dirent::d_reclen) == 2);
static_assert(offsetof(dirent, d_type) == 6 && sizeof(dirent::d_type) == 1);
static_assert(offsetof(dirent, d_namlen) == 7 && sizeof(dirent::d_namlen) == 1);
static_assert(offsetof(dirent, d_name) == 8 && sizeof(dirent::d_name) == 256);

struct OrbisDIR
{
    int fd;
    alignas(0x4000) char buffer[0x10000];
    size_t offset;
    size_t length;
    bool eof;
    dirent entry; // vk-285-112: this directory's own (see above)
    void* nfs = nullptr; // vk-285-135: a folder on an NFS share (OrbisNfs.h), listed by OrbisNfs; fd is -1 then
};

extern "C" DIR* opendir(const char* name)
{
    if (OrbisNfs::IsPath(name)) // vk-285-135: /nfs/<host>/... is a share's folder, not the kernel's
    {
        void* nfs = OrbisNfs::OpenDir(name);
        if (!nfs)
            return nullptr;
        OrbisDIR* d = new OrbisDIR();
        d->fd = -1;
        d->offset = 0;
        d->length = 0;
        d->eof = false;
        d->nfs = nfs;
        return reinterpret_cast<DIR*>(d);
    }
    const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
    const int fd = sceKernelOpen(name, flags, 0);
    if (fd < 0)
        return nullptr;
    OrbisDIR* d = new OrbisDIR();
    d->fd = fd;
    d->offset = 0;
    d->length = 0;
    d->eof = false;
    return reinterpret_cast<DIR*>(d);
}

extern "C" struct dirent* readdir(DIR* dirp)
{
    OrbisDIR* d = reinterpret_cast<OrbisDIR*>(dirp);
    if (!d || d->eof)
        return nullptr;
    if (d->nfs)
        return OrbisNfs::ReadDir(d->nfs, &d->entry) ? &d->entry : nullptr;

    // Consume one record from the current buffer.
    if (d->offset + 8 <= d->length)
    {
        char* p = d->buffer + d->offset;
        const size_t reclen = static_cast<unsigned char>(p[4]) | (static_cast<size_t>(static_cast<unsigned char>(p[5])) << 8);
        const size_t namlen = static_cast<unsigned char>(p[7]);
        if (reclen >= 12 && reclen % 4 == 0 && d->offset + reclen <= d->length && 8 + namlen < reclen)
        {
            // This directory's entry; valid until the next readdir on it or closedir.
            dirent& entry = d->entry;
            std::memset(&entry, 0, sizeof(entry));
            entry.d_fileno = *reinterpret_cast<const u32*>(p);
            entry.d_reclen = static_cast<u16>(reclen);
            entry.d_type = static_cast<u8>(p[6]);
            entry.d_namlen = static_cast<u8>(namlen);
            std::memcpy(entry.d_name, p + 8, namlen);
            entry.d_name[namlen] = '\0';
            d->offset += reclen;
            return &entry;
        }
        d->offset += reclen >= 4 ? (reclen + 3) & ~size_t{3} : 12;
        // Fall through to refill if the record was malformed.
    }

    // Refill buffer.
    const int n = sceKernelGetdents(d->fd, d->buffer, static_cast<int>(sizeof(d->buffer)));
    if (n <= 0)
    {
        d->eof = true;
        return nullptr;
    }
    d->length = static_cast<size_t>(n);
    d->offset = 0;

    // Consume the first record of the new buffer.
    if (d->offset + 8 <= d->length)
    {
        char* p = d->buffer + d->offset;
        const size_t reclen = static_cast<unsigned char>(p[4]) | (static_cast<size_t>(static_cast<unsigned char>(p[5])) << 8);
        const size_t namlen = static_cast<unsigned char>(p[7]);
        if (reclen >= 12 && reclen % 4 == 0 && d->offset + reclen <= d->length && 8 + namlen < reclen)
        {
            dirent& entry = d->entry;
            std::memset(&entry, 0, sizeof(entry));
            entry.d_fileno = *reinterpret_cast<const u32*>(p);
            entry.d_reclen = static_cast<u16>(reclen);
            entry.d_type = static_cast<u8>(p[6]);
            entry.d_namlen = static_cast<u8>(namlen);
            std::memcpy(entry.d_name, p + 8, namlen);
            entry.d_name[namlen] = '\0';
            d->offset += reclen;
            return &entry;
        }
        d->eof = true;
        return nullptr;
    }

    d->eof = true;
    return nullptr;
}

extern "C" int closedir(DIR* dirp)
{
    OrbisDIR* d = reinterpret_cast<OrbisDIR*>(dirp);
    if (!d)
        return -1;
    if (d->nfs)
    {
        OrbisNfs::CloseDir(d->nfs);
        delete d;
        return 0;
    }
    const int rc = sceKernelClose(d->fd);
    delete d;
    return rc;
}
