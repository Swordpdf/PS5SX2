// Orbis mmap/munmap backed by sceKernelMapFlexibleMemory.
// Plain mmap in the bigapp sandbox returns MAP_FAILED for everything; the
// flexible-memory API is the supported path and draws from the pool granted
// by the system (param.json kernel.flexibleMemorySize).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sys/mman.h>

extern "C" int sceKernelMapFlexibleMemory(void** addr, std::size_t len, int prot, int flags);
extern "C" int sceKernelMapNamedFlexibleMemory(void** addr, std::size_t len, int prot, int flags, const char* name);
extern "C" int sceKernelMapNamedSystemFlexibleMemory(void** addr, std::size_t len, int prot, int flags, const char* name);
extern "C" int sceKernelReserveVirtualRange(void** addr, unsigned long long len, int flags, unsigned long long alignment);
extern "C" int sceKernelReleaseFlexibleMemory(void* addr, std::size_t len);

extern "C" int orbis_reserve_range(void** addr, unsigned long long len)
{
    return sceKernelReserveVirtualRange(addr, len, 0, 0);
}

static int orbis_prot(int prot)
{
    int p = 0;
    if (prot & PROT_READ) p |= 0x1;
    if (prot & PROT_WRITE) p |= 0x2;
    if (prot & PROT_EXEC) p |= 0x4;
    return p;
}

extern "C" void* mmap(void* addr, std::size_t len, int prot, int flags, int fd, std::int64_t offset)
{
    if (len == 0)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    // Flexible mappings cannot be mprotected afterwards (no sceKernelMprotect),
    // so map RWX when the kernel allows (recompiler needs executable buffers);
    // fall back to RW if RWX is rejected.
    void* result = addr;
    const int map_flags = (flags & MAP_FIXED) ? 0x1 : 0; // MemoryMapFlags::Fixed
    int rc = sceKernelMapFlexibleMemory(&result, len, 7, map_flags);
    int used_prot = 7;
    if (rc != 0)
    {
        result = addr;
        used_prot = 3;
        rc = sceKernelMapFlexibleMemory(&result, len, 3, map_flags);
    }
    if (rc != 0)
    {
        errno = rc;
        return MAP_FAILED;
    }
    if (len > 0x100000)
        printf("[dbg] mmapshim: len=%zu prot=%d rc=%d addr=%p\n", (size_t)len, used_prot, rc, result);
    if (addr && (flags & MAP_FIXED) && result != addr)
    {
        errno = EINVAL;
        return MAP_FAILED;
    }
    return result;
}

extern "C" int munmap(void* addr, std::size_t len)
{
    return sceKernelReleaseFlexibleMemory(addr, len);
}
