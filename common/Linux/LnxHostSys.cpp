// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "OrbisPaths.h" // vk-285-33 (the port's include-orbis)
#include <cstring> // vk-285-65: OrbisMcontext
extern "C" int orbis_reserve_range(void** addr, unsigned long long len);
#ifdef ORBIS_VULKAN
// vk-285-64: fastmem on the PS5. The guest's data block is direct memory (pcsx2/Memory.cpp,
// g_orbis_data_phys), and the kernel maps one direct-memory range at several addresses (the
// memory probe, vk-285-62), so the fastmem area is a plain reservation and each view of a guest
// page is a fixed direct mapping of that page into it -- what shm_open and mmap(MAP_SHARED) do
// elsewhere. Flexible memory, which the mmap shim hands out, has no second view.
extern "C" long long g_orbis_data_phys;
extern "C" int sceKernelMapDirectMemory(void** addr, unsigned long long len, int prot, int flags,
	long long directMemoryStart, unsigned long long alignment);
extern "C" int sceKernelMunmap(void* addr, unsigned long long len);
extern "C" int sceKernelReserveVirtualRange(void** addr, unsigned long long len, int flags, unsigned long long alignment);
static constexpr int ORBIS_MAP_FIXED = 0x10;
#endif
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/HostSys.h"

#include <cstdio>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#ifndef __APPLE__
#include <ucontext.h>
#endif

#include "fmt/format.h"

#if defined(__FreeBSD__)
#include "cpuinfo.h"
#endif

static __ri uint LinuxProt(const PageProtectionMode& mode)
{
	u32 lnxmode = 0;

	if (mode.CanWrite())
		lnxmode |= PROT_WRITE;
	if (mode.CanRead())
		lnxmode |= PROT_READ;
	if (mode.CanExecute())
		lnxmode |= PROT_EXEC | PROT_READ;

	return lnxmode;
}

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	pxAssertMsg((size & (__pagesize - 1)) == 0, "Size is page aligned");

	const u32 lnxmode = LinuxProt(mode);

	const int result = mprotect(baseaddr, size, lnxmode);
	if (result != 0)
		pxFail("mprotect() failed");
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	const unsigned pid = static_cast<unsigned>(getpid());
#if defined(__FreeBSD__)
	// FreeBSD's shm_open(3) requires name to be absolute
	return fmt::format("/tmp/{}_{}", prefix, pid);
#else
	return fmt::format("{}_{}", prefix, pid);
#endif
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
	{
		std::fprintf(stderr, "shm_open failed: %d\n", errno);
		return nullptr;
	}

	// we're not going to be opening this mapping in other processes, so remove the file
	shm_unlink(name);

	// ensure it's the correct size
	if (ftruncate(fd, static_cast<off_t>(size)) < 0)
	{
		std::fprintf(stderr, "ftruncate(%zu) failed: %d\n", size, errno);
		return nullptr;
	}

	return reinterpret_cast<void*>(static_cast<intptr_t>(fd));
}

void HostSys::DestroySharedMemory(void* ptr)
{
	close(static_cast<int>(reinterpret_cast<intptr_t>(ptr)));
}

#ifndef __APPLE__

size_t HostSys::GetRuntimePageSize()
{
	int res = sysconf(_SC_PAGESIZE);
	return (res > 0) ? static_cast<size_t>(res) : 0;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
#if defined(__FreeBSD__)
	if (!cpuinfo_initialize())
		return 0;

	u32 max_line_size = 0;
	for (u32 i = 0; i < cpuinfo_get_processors_count(); i++)
	{
		const u32 l1i = cpuinfo_get_processor(i)->cache.l1i->line_size;
		const u32 l1d = cpuinfo_get_processor(i)->cache.l1d->line_size;
		const u32 res = std::max<u32>(l1i, l1d);

		max_line_size = std::max<u32>(max_line_size, res);
	}

	return static_cast<size_t>(max_line_size);
#else
	int l1i = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	int l1d = sysconf(_SC_LEVEL1_ICACHE_LINESIZE);
	int res = (l1i > l1d) ? l1i : l1d;
	for (int index = 0; index < 16; index++)
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "/sys/devices/system/cpu/cpu0/cache/index%d/coherency_line_size", index);
		std::FILE* fp = std::fopen(buf, "rb");
		if (!fp)
			break;

		std::fread(buf, sizeof(buf), 1, fp);
		std::fclose(fp);
		int val = std::atoi(buf);
		res = (val > res) ? val : res;
	}

	return (res > 0) ? static_cast<size_t>(res) : 0;
#endif
}

#endif

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");

#ifdef ORBIS_VULKAN
	// vk-285-64: a reservation (Create), not flexible memory.
	if (sceKernelMunmap(m_base_ptr, m_size) != 0)
		pxFailRel("Failed to release shared memory area");
#else
	if (munmap(m_base_ptr, m_size) != 0)
		pxFailRel("Failed to release shared memory area");
#endif
}


std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	pxAssertRel(Common::IsAlignedPow2(size, __pagesize), "Size is page aligned");

	uint flags = MAP_ANONYMOUS | MAP_PRIVATE;
#ifdef __APPLE__
	if (jit)
		flags |= MAP_JIT;
#endif
	// Orbis: reserve the range first so the flexible allocator cannot place
	// thread stacks inside it, then map flexible memory at the reserved address.
	void* reserved = nullptr;
#ifdef ORBIS_VULKAN
	// PS5 Vulkan build: the driver's GPU-visible memory must lie in the 4 GiB window
	// [0x2'0000'0000, 0x3'0000'0000) (its shaders' address high word is 2), and the
	// kernel places a reservation with no hint first-fit from the window's start.
	// This area is 4 GiB (vtlb fastmem): unhinted it takes the whole window, and the
	// driver's first mapping (vkCreateDevice's submission buffer) lands outside it
	// (VK_ERROR_OUT_OF_DEVICE_MEMORY, vk-285-1). Ask for space above the window.
	reserved = reinterpret_cast<void*>(static_cast<uintptr_t>(0x300000000ULL));
	const int reserve_rc = orbis_reserve_range(&reserved, size);
	std::printf("[dbg] shmarea: reserve %zu bytes rc=%d at %p\n", size, reserve_rc, reserve_rc == 0 ? reserved : nullptr);
	if (reserve_rc != 0)
		reserved = nullptr;
	// vk-285-64: with the guest's data in direct memory the reservation is the whole area; Map()
	// fills it with direct mappings. (The mmap below would ask the flexible shim for all of it.)
	if (reserved && g_orbis_data_phys >= 0)
		return std::unique_ptr<SharedMemoryMappingArea>(
			new SharedMemoryMappingArea(static_cast<u8*>(reserved), size, size / __pagesize));
#else
	if (orbis_reserve_range(&reserved, size) != 0)
		reserved = nullptr;
#endif
	void* alloc = mmap(reserved, size, PROT_NONE, flags | MAP_FIXED, -1, 0);
	if (alloc == MAP_FAILED)
	{
		alloc = mmap(nullptr, size, PROT_NONE, flags, -1, 0);
		if (alloc == MAP_FAILED)
			return nullptr;
	}

	return std::unique_ptr<SharedMemoryMappingArea>(new SharedMemoryMappingArea(static_cast<u8*>(alloc), size, size / __pagesize));
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

	const uint lnxmode = LinuxProt(mode);
#ifdef ORBIS_VULKAN
	// vk-285-64: a view of the guest's data block at a fixed address (see the top of this file).
	if (!file_handle && g_orbis_data_phys >= 0)
	{
		void* addr = map_base;
		const int prot = (mode.CanRead() ? 0x1 : 0) | (mode.CanWrite() ? 0x2 : 0);
		const int rc = sceKernelMapDirectMemory(&addr, map_size, prot, ORBIS_MAP_FIXED,
			g_orbis_data_phys + static_cast<long long>(file_offset), __pagesize);
		if (rc != 0 || addr != map_base)
		{
			static int s_logged = 0;
			if (s_logged++ < 8)
				std::printf("[fastmem] map %p +%zu offset 0x%zx failed: rc=0x%x at %p\n", map_base, map_size,
					file_offset, static_cast<unsigned>(rc), addr);
			return nullptr;
		}
		m_num_mappings++;
		return static_cast<u8*>(map_base);
	}
#endif
	if (file_handle)
	{
		const int fd = static_cast<int>(reinterpret_cast<intptr_t>(file_handle));
		// MAP_FIXED is okay here, since we've reserved the entire region, and *want* to overwrite the mapping.
		void* const ptr = mmap(map_base, map_size, lnxmode, MAP_SHARED | MAP_FIXED, fd, static_cast<off_t>(file_offset));
		if (ptr == MAP_FAILED)
			return nullptr;
	}
	else
	{
		// macOS doesn't seem to allow MAP_JIT with MAP_FIXED
		// So we do the MAP_JIT in the allocation, and just mprotect here
		// Note that this will only work the first time for a given region
		if (mprotect(map_base, map_size, lnxmode) < 0)
			return nullptr;
	}

	m_num_mappings++;
	return static_cast<u8*>(map_base);
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	pxAssert(static_cast<u8*>(map_base) >= m_base_ptr && static_cast<u8*>(map_base) < (m_base_ptr + m_size));

#ifdef ORBIS_VULKAN
	// vk-285-64: drop the view and reserve the hole again, so nothing else lands in the area.
	if (g_orbis_data_phys >= 0)
	{
		if (sceKernelMunmap(map_base, map_size) != 0)
			return false;
		void* again = map_base;
		if (sceKernelReserveVirtualRange(&again, map_size, ORBIS_MAP_FIXED, 0) != 0 || again != map_base)
			return false;
		m_num_mappings--;
		return true;
	}
#endif

	if (mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
		return false;

	m_num_mappings--;
	return true;
}

#ifdef ARCH_ARM64

void HostSys::FlushInstructionCache(void* address, u32 size)
{
	__builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address) + size);
}

#endif

#ifndef __APPLE__ // These are done in DarwinMisc

namespace PageFaultHandler
{
	static std::recursive_mutex s_exception_handler_mutex;
	static bool s_in_exception_handler = false;
	static bool s_installed = false;
} // namespace PageFaultHandler

#ifdef ARCH_ARM64

[[maybe_unused]] static bool IsStoreInstruction(const void* ptr)
{
	u32 bits;
	std::memcpy(&bits, ptr, sizeof(bits));

	// Based on vixl's disassembler Instruction::IsStore().
	// if (Mask(LoadStoreAnyFMask) != LoadStoreAnyFixed)
	if ((bits & 0x0a000000) != 0x08000000)
		return false;

	// if (Mask(LoadStorePairAnyFMask) == LoadStorePairAnyFixed)
	if ((bits & 0x3a000000) == 0x28000000)
	{
		// return Mask(LoadStorePairLBit) == 0
		return (bits & (1 << 22)) == 0;
	}

	switch (bits & 0xC4C00000)
	{
		case 0x00000000: // STRB_w
		case 0x40000000: // STRH_w
		case 0x80000000: // STR_w
		case 0xC0000000: // STR_x
		case 0x04000000: // STR_b
		case 0x44000000: // STR_h
		case 0x84000000: // STR_s
		case 0xC4000000: // STR_d
		case 0x04800000: // STR_q
			return true;

		default:
			return false;
	}
}

#endif // ARCH_ARM64

namespace PageFaultHandler
{
	static void SignalHandler(int sig, siginfo_t* info, void* ctx);
} // namespace PageFaultHandler

#if defined(__FreeBSD__) && defined(ARCH_X86) && defined(ORBIS_VULKAN)
// vk-285-65: one field of the PS5's signal mcontext, by its offset in FreeBSD's amd64 mcontext_t
// (mc_rbp 0x48, mc_addr 0x88, mc_err 0x98, mc_rip 0xa0, mc_rsp 0xb8). The PS5 puts the mcontext at
// ucontext + 0x40, not + 0x10: vk-285-64's crash dump had rbp (the fastmem base 0x3'0000'0000) at
// +0x88, the fault address at +0xc8, the error code 6 at +0xd8 and rip at +0xe0 -- the note
// "FreeBSD layout + 0x30" of the crash printer.
static inline u64 OrbisMcontext(void* ctx, u32 field)
{
	u64 value;
	std::memcpy(&value, static_cast<const u8*>(ctx) + 0x40 + field, sizeof(value));
	return value;
}
#endif
static bool s_pf_crashing = false;

extern "C" volatile unsigned long long orbis_fault_count;
void PageFaultHandler::SignalHandler(int sig, siginfo_t* info, void* ctx)
{
	orbis_fault_count++;
	// Orbis: log EVERY fault at entry (before any handling). Lets us reconstruct
	// primary->secondary chains when a handler forwards or mishandles.
	// vk-285-64: fastmem backpatches a code site on its first fault, and a game can fault
	// thousands of times while it runs its first frames: the first 64 and every 4096th.
	if (orbis_fault_count <= 64 || (orbis_fault_count & 4095) == 0)
	{
		FILE* f = fopen(g_orbis_pf_log, "a");
		if (f)
		{
			void* epc = nullptr;
			void* esp = nullptr;
			if (ctx)
			{
#if defined(__FreeBSD__) && defined(ARCH_X86) && defined(ORBIS_VULKAN)
				// vk-285-65: the PS5's mcontext (OrbisMcontext below).
				epc = reinterpret_cast<void*>(OrbisMcontext(ctx, 0xa0));
				esp = reinterpret_cast<void*>(OrbisMcontext(ctx, 0xb8));
#elif defined(__FreeBSD__) && defined(ARCH_X86)
				ucontext_t* uc = static_cast<ucontext_t*>(ctx);
				epc = reinterpret_cast<void*>(uc->uc_mcontext.mc_rip);
				esp = reinterpret_cast<void*>(uc->uc_mcontext.mc_rsp);
#endif
			}
			fprintf(f, "[pf+] sig=%d pc=%p rsp=%p addr=%p\n",
				sig, epc, esp, info ? reinterpret_cast<void*>(info->si_addr) : nullptr);
			fclose(f);
		}
	}
#if defined(__linux__)
	void* const exception_address = reinterpret_cast<void*>(info->si_addr);

#if defined(ARCH_X86)
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_RIP]);
	const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(ARCH_ARM64)
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.pc);
	const bool is_write = IsStoreInstruction(exception_pc);
#endif

#elif defined(__FreeBSD__)

#if defined(ARCH_X86) && defined(ORBIS_VULKAN)
	// vk-285-65: the PS5's mcontext starts 0x30 bytes later than FreeBSD's ucontext_t puts it, so
	// uc_mcontext.mc_rip read r14 and mc_addr read r11, and no fault was ever recognised (fastmem's
	// first MMIO access crashed, vk-285-64). The crash printer reads the same layout.
	void* const exception_address = reinterpret_cast<void*>(OrbisMcontext(ctx, 0x88));
	void* const exception_pc = reinterpret_cast<void*>(OrbisMcontext(ctx, 0xa0));
	const bool is_write = (OrbisMcontext(ctx, 0x98) & 2) != 0;
#elif defined(ARCH_X86)
	void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_addr);
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_rip);
	const bool is_write = (static_cast<ucontext_t*>(ctx)->uc_mcontext.mc_err & 2) != 0;
#elif defined(ARCH_ARM64)
	void* const exception_address = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__es.__far);
	void* const exception_pc = reinterpret_cast<void*>(static_cast<ucontext_t*>(ctx)->uc_mcontext->__ss.__pc);
	const bool is_write = IsStoreInstruction(exception_pc);
#endif

#endif

	// Orbis: recursive fault while the handler is already active - the mutex is
	// held, so bail immediately to avoid a deadlock/fault-storm (kernel panic).
	if (s_in_exception_handler)
	{
		FILE* f = fopen(g_orbis_pf_log, "a");
		if (f)
		{
			fprintf(f, "[pf] sig=%d pc=%p addr=%p write=%d (recursive, bail)\n", sig, exception_pc, exception_address, is_write);
			fclose(f);
		}
		_exit(1);
	}

	// Executing the handler concurrently from multiple threads wouldn't go down well.
	s_exception_handler_mutex.lock();

	// Prevent recursive exception filtering.
	HandlerResult result = HandlerResult::ExecuteNextHandler;
	if (!s_in_exception_handler)
	{
		s_in_exception_handler = true;
		result = HandlePageFault(exception_pc, exception_address, is_write);
		s_in_exception_handler = false;
	}

	s_exception_handler_mutex.unlock();

	// Resumes execution right where we left off (re-executes instruction that caused the SIGSEGV).
	if (result == HandlerResult::ContinueExecution)
		return;

	// We couldn't handle it. Pass it off to the crash dumper.
	{
		FILE* f = fopen(g_orbis_pf_log, "a");
		if (f)
		{
			fprintf(f, "[pf] sig=%d pc=%p addr=%p write=%d\n", sig, exception_pc, exception_address, is_write);
			fclose(f);
		}
	}
	// Orbis: prevent a fault-storm (fault while the crash dumper runs) from
	// looping and taking the kernel down. A recursive fault bails immediately.
	if (s_pf_crashing)
		_exit(1);
	s_pf_crashing = true;
	CrashHandler::CrashSignalHandler(sig, info, ctx);
	_exit(1);
}

bool PageFaultHandler::Install(Error* error)
{
	std::unique_lock lock(s_exception_handler_mutex);
#ifdef ORBIS_VULKAN
	// vk-285-64: main-boot installs it at start, and vtlb_Core_Alloc again once the fastmem area
	// exists; the second install is the first one.
	if (s_installed)
		return true;
#endif
	pxAssertRel(!s_installed, "Page fault handler has already been installed.");

	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO | SA_NODEFER;
	sa.sa_sigaction = SignalHandler;

	if (sigaction(SIGSEGV, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGSEGV failed: ", errno);
		return false;
	}

#ifdef ARCH_ARM64
	// We can get SIGBUS on ARM64.
	if (sigaction(SIGBUS, &sa, nullptr) != 0)
	{
		Error::SetErrno(error, "sigaction() for SIGBUS failed: ", errno);
		return false;
	}
#endif

	s_installed = true;
	return true;
}

bool PageFaultHandler::InstallSecondaryThread() { return true; }

#endif // __APPLE__
