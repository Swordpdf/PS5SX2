// SPDX-License-Identifier: GPL-3.0-or-later
// vk-285-62: the memory probe (the flag file memprobe, after the jailbreak).
//
// PS5SX2 keeps the recompilers' code in JIT shared memory and the C/C++ heap, the
// thread stacks and a 128 MiB stack guard in flexible memory: 448 MiB configured,
// ~55 MiB free during play (the [rec] line). The guest's memory is already direct
// memory (pcsx2/Memory.cpp, 0x6'0000'0000), and fastmem is off: its 4 GiB area
// needs the guest pages mapped at many addresses, which flexible memory cannot do.
//
// Each test below says what it measures and logs one [memprobe] line; nothing is
// kept mapped except what the kernel refuses to release. A test that faults
// (executing a mapping the kernel did not make executable) is caught by a
// temporary SIGSEGV/SIGBUS handler and reported as a fault.
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int sceKernelAllocateDirectMemory(long long searchStart, long long searchEnd, unsigned long long len,
	unsigned long long alignment, int memoryType, long long* physAddrOut);
int sceKernelMapDirectMemory(void** addr, unsigned long long len, int prot, int flags, long long directMemoryStart,
	unsigned long long alignment);
int sceKernelReleaseDirectMemory(long long start, unsigned long long len);
int sceKernelMunmap(void* addr, unsigned long long len);
int sceKernelMprotect(const void* addr, unsigned long long len, int prot);
int sceKernelReserveVirtualRange(void** addr, unsigned long long len, int flags, unsigned long long alignment);
int sceKernelAvailableDirectMemorySize(long long searchStart, long long searchEnd, unsigned long long alignment,
	long long* physAddrOut, unsigned long long* sizeOut);
long long sceKernelGetDirectMemorySize(void);
int sceKernelConfiguredFlexibleMemorySize(unsigned long long* size);
int sceKernelAvailableFlexibleMemorySize(unsigned long long* size);
int sceKernelJitCreateSharedMemory(int flags, unsigned long long size, int protection, int* destinationHandle);
int sceKernelJitMapSharedMemory(int handle, int protection, void** destination);
}

namespace
{
constexpr int kProtCpuRead = 0x01;
constexpr int kProtCpuWrite = 0x02;
constexpr int kProtCpuExec = 0x04;
constexpr int kProtGpuRead = 0x10;
constexpr int kProtGpuWrite = 0x20;
constexpr int kMapFixed = 0x10;
constexpr int kMemoryType = 12; // what the driver and pcsx2/Memory.cpp allocate
constexpr unsigned long long kPage = 0x4000;
constexpr unsigned long long kTwoMiB = 0x200000;

sigjmp_buf s_jump;
volatile sig_atomic_t s_faults = 0;

void on_fault(int, siginfo_t*, void*)
{
	s_faults = s_faults + 1;
	siglongjmp(s_jump, 1);
}

struct FaultGuard
{
	struct sigaction old_segv{}, old_bus{};
	FaultGuard()
	{
		struct sigaction action{};
		action.sa_sigaction = on_fault;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		sigaction(SIGSEGV, &action, &old_segv);
		sigaction(SIGBUS, &action, &old_bus);
	}
	~FaultGuard()
	{
		sigaction(SIGSEGV, &old_segv, nullptr);
		sigaction(SIGBUS, &old_bus, nullptr);
	}
};

unsigned long long flexible_free()
{
	unsigned long long size = 0;
	sceKernelAvailableFlexibleMemorySize(&size);
	return size;
}

double now_us()
{
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

// Writes "mov eax, 42; ret" and calls it under the fault guard: 42, or -1 when the
// call faulted (the page is not executable), or -2 when the write faulted.
int run_code(void* write_view, void* exec_view)
{
	static const unsigned char code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
	FaultGuard guard;
	if (sigsetjmp(s_jump, 1) != 0)
		return -2;
	std::memcpy(write_view, code, sizeof(code));
	__builtin___clear_cache(static_cast<char*>(exec_view), static_cast<char*>(exec_view) + sizeof(code));
	if (sigsetjmp(s_jump, 1) != 0)
		return -1;
	using Fn = int (*)();
	return reinterpret_cast<Fn>(exec_view)();
}

// One direct allocation for a test, released afterwards.
struct Direct
{
	long long phys = -1;
	unsigned long long size = 0;
	int rc = -1;
	explicit Direct(unsigned long long bytes)
		: size(bytes)
	{
		rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes, kTwoMiB, kMemoryType, &phys);
	}
	~Direct()
	{
		if (rc == 0)
			sceKernelReleaseDirectMemory(phys, size);
	}
};

void test_budgets()
{
	unsigned long long configured = 0;
	sceKernelConfiguredFlexibleMemorySize(&configured);
	long long phys = 0;
	unsigned long long available = 0;
	const int rc = sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), kPage, &phys, &available);
	std::printf("[memprobe] budgets: flexible configured %llu MiB, free %llu MiB; direct total %lld MiB, "
				"largest free block %llu MiB at 0x%llx (rc=%d)\n",
		configured >> 20, flexible_free() >> 20, sceKernelGetDirectMemorySize() >> 20, available >> 20,
		static_cast<unsigned long long>(phys), rc);
}

// Whether JIT shared memory comes out of the flexible budget: 16 MiB, left mapped
// (there is no release for it here), so the probe costs 16 MiB of whichever pool.
void test_jit_accounting()
{
	const unsigned long long before = flexible_free();
	int handle = -1;
	const int rc = sceKernelJitCreateSharedMemory(0, 16ull << 20, 7, &handle);
	void* view = nullptr;
	const int rm = rc == 0 ? sceKernelJitMapSharedMemory(handle, 7, &view) : -1;
	if (view != nullptr)
		std::memset(view, 0, 16ull << 20);
	const unsigned long long after = flexible_free();
	std::printf("[memprobe] jit shm 16 MiB: create rc=%d map rc=%d at %p; flexible free %llu -> %llu KiB "
				"(%lld KiB)\n",
		rc, rm, view, before >> 10, after >> 10, static_cast<long long>(before >> 10) - static_cast<long long>(after >> 10));
}

// Whether direct memory can hold the recompilers' code: mapped RWX at once, or RW
// and then made executable, and whether the code then runs.
void test_direct_exec()
{
	static const struct
	{
		const char* name;
		int prot;
	} maps[] = {
		{"prot RWX (0x07)", kProtCpuRead | kProtCpuWrite | kProtCpuExec},
		{"prot RWX+GPU RW (0x37)", kProtCpuRead | kProtCpuWrite | kProtCpuExec | kProtGpuRead | kProtGpuWrite},
	};
	for (const auto& map : maps)
	{
		Direct direct(kTwoMiB);
		void* addr = nullptr;
		const int rm = direct.rc == 0 ? sceKernelMapDirectMemory(&addr, kTwoMiB, map.prot, 0, direct.phys, kTwoMiB) : -1;
		const int ran = rm == 0 ? run_code(addr, addr) : 0;
		std::printf("[memprobe] direct %s: alloc rc=%d map rc=0x%x at %p, code %s (%d)\n", map.name, direct.rc,
			static_cast<unsigned>(rm), addr,
			rm != 0 ? "not run" : ran == 42 ? "RAN" : ran == -1 ? "faulted on execute" : "faulted on write", ran);
		if (rm == 0)
			sceKernelMunmap(addr, kTwoMiB);
	}
	{
		Direct direct(kTwoMiB);
		void* addr = nullptr;
		const int rm = direct.rc == 0 ? sceKernelMapDirectMemory(&addr, kTwoMiB, kProtCpuRead | kProtCpuWrite, 0, direct.phys, kTwoMiB) : -1;
		const int rp = rm == 0 ? sceKernelMprotect(addr, kTwoMiB, kProtCpuRead | kProtCpuWrite | kProtCpuExec) : -1;
		const int rp2 = rm == 0 ? mprotect(addr, kTwoMiB, PROT_READ | PROT_WRITE | PROT_EXEC) : -1;
		const int e2 = rp2 != 0 ? errno : 0;
		const int ran = rm == 0 ? run_code(addr, addr) : 0;
		std::printf("[memprobe] direct RW then sceKernelMprotect RWX rc=0x%x, mprotect RWX rc=%d errno=%d: code %s (%d)\n",
			static_cast<unsigned>(rp), rp2, e2,
			rm != 0 ? "not run" : ran == 42 ? "RAN" : ran == -1 ? "faulted on execute" : "faulted on write", ran);
		if (rm == 0)
			sceKernelMunmap(addr, kTwoMiB);
	}
	{
		// Two views of one allocation: RW for the writer, RX for the CPU to run.
		Direct direct(kTwoMiB);
		void* rw = nullptr;
		void* rx = nullptr;
		const int r1 = direct.rc == 0 ? sceKernelMapDirectMemory(&rw, kTwoMiB, kProtCpuRead | kProtCpuWrite, 0, direct.phys, kTwoMiB) : -1;
		const int r2 = direct.rc == 0 ? sceKernelMapDirectMemory(&rx, kTwoMiB, kProtCpuRead | kProtCpuExec, 0, direct.phys, kTwoMiB) : -1;
		const int ran = r1 == 0 && r2 == 0 ? run_code(rw, rx) : 0;
		std::printf("[memprobe] direct RW view + RX view: map rc=0x%x/0x%x at %p/%p, code %s (%d)\n",
			static_cast<unsigned>(r1), static_cast<unsigned>(r2), rw, rx,
			r1 != 0 || r2 != 0 ? "not run" : ran == 42 ? "RAN" : ran == -1 ? "faulted on execute" : "faulted on write", ran);
		if (r1 == 0)
			sceKernelMunmap(rw, kTwoMiB);
		if (r2 == 0)
			sceKernelMunmap(rx, kTwoMiB);
	}
}

// Fastmem's needs: one guest page seen at two addresses, a mapping at a fixed
// address inside a reserved range, its removal and the hole reserved again, and
// what a 16 KiB map+unmap costs.
void test_aliasing()
{
	Direct direct(kTwoMiB);
	void* a = nullptr;
	void* b = nullptr;
	const int r1 = direct.rc == 0 ? sceKernelMapDirectMemory(&a, kTwoMiB, kProtCpuRead | kProtCpuWrite, 0, direct.phys, kTwoMiB) : -1;
	const int r2 = direct.rc == 0 ? sceKernelMapDirectMemory(&b, kTwoMiB, kProtCpuRead | kProtCpuWrite, 0, direct.phys, kTwoMiB) : -1;
	bool same = false;
	if (r1 == 0 && r2 == 0)
	{
		FaultGuard guard;
		if (sigsetjmp(s_jump, 1) == 0)
		{
			static_cast<volatile uint32_t*>(a)[100] = 0x5053535au;
			same = static_cast<volatile uint32_t*>(b)[100] == 0x5053535au;
		}
	}
	std::printf("[memprobe] alias: two views of one block rc=0x%x/0x%x at %p/%p, a write through one %s the other\n",
		static_cast<unsigned>(r1), static_cast<unsigned>(r2), a, b, same ? "SHOWS in" : "does NOT show in");
	if (r1 == 0)
		sceKernelMunmap(a, kTwoMiB);
	if (r2 == 0)
		sceKernelMunmap(b, kTwoMiB);

	void* base = nullptr;
	const int rr = sceKernelReserveVirtualRange(&base, 64ull << 20, 0, kTwoMiB);
	void* at = rr == 0 ? static_cast<char*>(base) + (16ull << 20) : nullptr;
	void* got = at;
	const int rf = rr == 0 && direct.rc == 0 ? sceKernelMapDirectMemory(&got, kTwoMiB, kProtCpuRead | kProtCpuWrite, kMapFixed, direct.phys, kTwoMiB) : -1;
	bool readable = false;
	if (rf == 0 && got == at)
	{
		FaultGuard guard;
		if (sigsetjmp(s_jump, 1) == 0)
		{
			static_cast<volatile uint32_t*>(got)[7] = 7;
			readable = static_cast<volatile uint32_t*>(got)[7] == 7;
		}
	}
	const int ru = rf == 0 ? sceKernelMunmap(got, kTwoMiB) : -1;
	void* again = at;
	const int ra = ru == 0 ? sceKernelReserveVirtualRange(&again, kTwoMiB, kMapFixed, 0) : -1;
	std::printf("[memprobe] fixed: reserve 64 MiB rc=0x%x at %p; map fixed at +16 MiB rc=0x%x -> %p (%s, %s); "
				"unmap rc=0x%x; reserve the hole again rc=0x%x -> %p\n",
		static_cast<unsigned>(rr), base, static_cast<unsigned>(rf), got, got == at ? "at the address" : "ELSEWHERE",
		readable ? "readable" : "not readable", static_cast<unsigned>(ru), static_cast<unsigned>(ra), again);

	// 16 KiB pages mapped and unmapped at fixed addresses, as fastmem remaps them.
	if (rr == 0 && direct.rc == 0)
	{
		const int count = 512;
		int failures = 0;
		const double t0 = now_us();
		for (int i = 0; i < count; i++)
		{
			void* page = static_cast<char*>(base) + (32ull << 20) + static_cast<unsigned long long>(i) * kPage;
			void* p = page;
			if (sceKernelMapDirectMemory(&p, kPage, kProtCpuRead | kProtCpuWrite, kMapFixed, direct.phys + (i % 128) * kPage, kPage) != 0 || p != page)
				failures++;
		}
		const double t1 = now_us();
		for (int i = 0; i < count; i++)
		{
			void* page = static_cast<char*>(base) + (32ull << 20) + static_cast<unsigned long long>(i) * kPage;
			if (sceKernelMunmap(page, kPage) != 0)
				failures++;
		}
		const double t2 = now_us();
		std::printf("[memprobe] fixed 16 KiB pages: %d maps %.2f us each, %d unmaps %.2f us each, %d failures\n",
			count, (t1 - t0) / count, count, (t2 - t1) / count, failures);
	}
}

// Where the kernel puts GPU-visible direct memory with no address given, and
// whether it maps some above the driver's 4 GiB window [0x2'0000'0000, 0x3'0000'0000).
void test_gpu_placement()
{
	Direct direct(kTwoMiB);
	void* anywhere = nullptr;
	const int r1 = direct.rc == 0 ? sceKernelMapDirectMemory(&anywhere, kTwoMiB, 0x33, 0, direct.phys, kTwoMiB) : -1;
	if (r1 == 0)
		sceKernelMunmap(anywhere, kTwoMiB);
	void* high = reinterpret_cast<void*>(0x500000000ull);
	const int r2 = direct.rc == 0 ? sceKernelMapDirectMemory(&high, kTwoMiB, 0x33, 0, direct.phys, kTwoMiB) : -1;
	if (r2 == 0)
		sceKernelMunmap(high, kTwoMiB);
	std::printf("[memprobe] gpu-visible (0x33): no hint rc=0x%x -> %p; hint 0x5'0000'0000 rc=0x%x -> %p\n",
		static_cast<unsigned>(r1), anywhere, static_cast<unsigned>(r2), high);
}
} // namespace

void orbis_memprobe()
{
	std::printf("[memprobe] start (uid %d)\n", static_cast<int>(getuid()));
	std::fflush(stdout);
	test_budgets();
	std::fflush(stdout);
	test_jit_accounting();
	std::fflush(stdout);
	test_direct_exec();
	std::fflush(stdout);
	test_aliasing();
	std::fflush(stdout);
	test_gpu_placement();
	std::printf("[memprobe] done, %d faults caught\n", static_cast<int>(s_faults));
	std::fflush(stdout);
}
