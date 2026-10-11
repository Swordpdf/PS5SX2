// Orbis memory helpers: pooled memory (data) + JIT shared memory (code).
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

extern "C" {
// Pooled memory (separate region, RW-only)
int sceKernelMemoryPoolReserve(void* addr_in, unsigned long long len, unsigned long long alignment, int flags, void** addr_out);
int sceKernelMemoryPoolCommit(void* addr, unsigned long long len, int type, int prot, int flags);

// JIT shared memory (executable-capable, separate region)
int sceKernelJitCreateSharedMemory(int flags, unsigned long long size, int protection, int* destinationHandle);
int sceKernelJitMapSharedMemory(int handle, int protection, void** destination);
int sceKernelJitCreateAliasOfSharedMemory(int handle, int protection, int* destinationHandle);

// misc
int sceKernelAvailableFlexibleMemorySize(unsigned long long*);
}

// Allocate `len` bytes of RW pooled memory. Returns nullptr on failure.
extern "C" void* orbis_alloc_pool(unsigned long long len)
{
    // Pool allocations must be 2MB-aligned.
    const unsigned long long align = 0x200000;
    len = (len + align - 1) & ~(align - 1);
    void* out = nullptr;
    int r1 = sceKernelMemoryPoolReserve(nullptr, len, 0, 0, &out);
    if (r1 != 0)
    {
        printf("[orbismem] pool reserve failed rc=%d\n", r1);
        return nullptr;
    }
    // type 1 = write-back onion; commit in 64KB-aligned chunks
    int r2 = sceKernelMemoryPoolCommit(out, len, 1, 3 /*RW*/, 0);
    if (r2 != 0)
    {
        printf("[orbismem] pool commit failed rc=%d\n", r2);
        return nullptr;
    }
    return out;
}

// Allocate `len` bytes of RWX JIT shared memory. Returns nullptr on failure.
extern "C" void* orbis_alloc_jit(unsigned long long len)
{
    printf("[orbismem] jit create begin len=%llu\n", (unsigned long long)len);
    fflush(stdout);
    int fd = 0;
    int r1 = sceKernelJitCreateSharedMemory(0, len, 7 /*RWX*/, &fd);
    printf("[orbismem] jit create rc=%d\n", r1);
    fflush(stdout);
    if (r1 != 0)
    {
        printf("[orbismem] jit create failed rc=%d\n", r1);
        return nullptr;
    }
    void* out = nullptr;
    int r2 = sceKernelJitMapSharedMemory(fd, 7 /*RWX*/, &out);
    printf("[orbismem] jit map rc=%d out=%p\n", r2, out);
    fflush(stdout);
    if (r2 != 0)
    {
        printf("[orbismem] jit map failed rc=%d\n", r2);
        return nullptr;
    }
    return out;
}
