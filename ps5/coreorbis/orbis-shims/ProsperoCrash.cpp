// Orbis crash printer: fault address + PC via siginfo/ucontext (FreeBSD x86).
// Pipe the PC through llvm-addr2line on pcsx2_boot.elf for file:line.
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>



#include "common/CrashHandler.h"
#include "debug_overlay.h"

extern "C" void orbis_rec_dump();
extern "C" void orbis_vtlb_dump();
// vk-285-51: the settings log (frontend/fe_ps5.cpp; open/write only). Weak: the GL build has none.
extern "C" void orbis_event_log(const char* line) __attribute__((weak));
// vk-285-104: the deferred GS-thread log to stdout (pcsx2/OrbisDeferredLog.cpp). Weak: builds without it.
extern "C" void orbis_log_drain() __attribute__((weak));

namespace CrashHandler
{
void CrashSignalHandler(int signal, siginfo_t* siginfo, void* ctx)
{
  void* addr = siginfo ? siginfo->si_addr : nullptr;
  void* pc = nullptr;
  if (ctx)
  {
    ucontext_t* uc = static_cast<ucontext_t*>(ctx);
    pc = reinterpret_cast<void*>(uc->uc_mcontext.mc_rip);
  }
  if (orbis_event_log)
  {
    // vk-285-51: one line in the settings log first, before anything that could fault again.
    // The PS5's context is FreeBSD's shifted by 0x30: the real RIP is at +0xe0 (below).
    const unsigned long long rip = ctx ? static_cast<const unsigned long long*>(ctx)[0xe0 / 8] : 0;
    char line[240];
    snprintf(line, sizeof(line), "crash: signal %d at eboot+%#llx (fault address %p); the details are in this session's boot.log",
      signal, rip >= 0x400000 ? rip - 0x400000 : rip, addr);
    orbis_event_log(line);
  }
  // Orbis: stdout is block-buffered now; flush first so the buffered tail
  // (the most important part) is never lost on abort().
  if (orbis_log_drain)
    orbis_log_drain(); // vk-285-104: the GS thread's deferred lines first (OrbisDeferredLog.h)
  fflush(stdout);
  printf("[crash] signal=%d fault_addr=%p pc=%p tid=%llu\n", signal, addr, pc, (unsigned long long)pthread_self());
  if (ctx)
  {
    const unsigned long long* q = static_cast<const unsigned long long*>(ctx);
    for (int i = 0; i < 80; i += 4)
      printf("[crash] ucraw +%03x: %llx %llx %llx %llx\n", i * 8, q[i], q[i + 1], q[i + 2], q[i + 3]);
    {
      // PS5 mcontext is FreeBSD's shifted by +0x30: rbp@0x88 rip@0xe0 rsp@0xf8 (ctx-relative).
      const unsigned long long rrip = q[0xe0 / 8], rrsp = q[0xf8 / 8], rrbp = q[0x88 / 8];
      printf("[crash] REAL rip=%llx rsp=%llx rbp=%llx trapno=%llx err=%llx\n", rrip, rrsp, rrbp, q[0xc0 / 8] & 0xffffffff, q[0xd8 / 8]);
      if (rrsp >= 0x100000 && (rrsp & 7) == 0)
      {
        const unsigned long long* sp = reinterpret_cast<const unsigned long long*>(rrsp);
        for (int i = 0; i < 48; i++)
          if (sp[i] >= 0x400000 && sp[i] < 0x10000000)
            printf("[crash] REAL st[%d]=%llx elf-off=%llx\n", i, sp[i], sp[i] - 0x400000);
      }
      unsigned long long fp = rrbp;
      for (int i = 0; i < 16 && fp >= 0x100000 && (fp & 7) == 0; i++)
      {
        const unsigned long long* f = reinterpret_cast<const unsigned long long*>(fp);
        printf("[crash] REAL bt%d ret=%llx\n", i, f[1]);
        if (f[0] <= fp) break;
        fp = f[0];
      }
      fflush(stdout);
    }
    printf("[crash] mcontext offset=%zu\n", (size_t)((const char*)&static_cast<ucontext_t*>(ctx)->uc_mcontext - (const char*)ctx));
    fflush(stdout);
  }
  // Orbis: peek the faulting bytes + stack through STDOUT (stderr proved
  // unreliable here - its writes never land). Flush between stages so a fault
  // in the risky RSP peek loses only the RSP line; the pf guard _exit(1)s it.
  if (pc)
  {
    const unsigned char* b = (const unsigned char*)pc;
    printf("[crash] rip-bytes:");
    for (int i = 0; i < 32; i++)
      printf(" %02x", b[i]);
    printf("\n");
    fflush(stdout);
  }
  if (ctx)
  {
    // Orbis: registers straight from the ucontext, printed EARLY (before any
    // stack peek that could fault on a corrupted RSP).
    ucontext_t* ucr = static_cast<ucontext_t*>(ctx);
    printf("[crash] regs: rip=%p rsp=%p rbp=%p rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
      (void*)ucr->uc_mcontext.mc_rip, (void*)ucr->uc_mcontext.mc_rsp, (void*)ucr->uc_mcontext.mc_rbp,
      (unsigned long long)ucr->uc_mcontext.mc_rax, (unsigned long long)ucr->uc_mcontext.mc_rbx,
      (unsigned long long)ucr->uc_mcontext.mc_rcx, (unsigned long long)ucr->uc_mcontext.mc_rdx,
      (unsigned long long)ucr->uc_mcontext.mc_rsi, (unsigned long long)ucr->uc_mcontext.mc_rdi,
      (unsigned long long)ucr->uc_mcontext.mc_r8, (unsigned long long)ucr->uc_mcontext.mc_r9,
      (unsigned long long)ucr->uc_mcontext.mc_r10, (unsigned long long)ucr->uc_mcontext.mc_r11,
      (unsigned long long)ucr->uc_mcontext.mc_r12, (unsigned long long)ucr->uc_mcontext.mc_r13,
      (unsigned long long)ucr->uc_mcontext.mc_r14, (unsigned long long)ucr->uc_mcontext.mc_r15);
    fflush(stdout);
  }
  if (ctx)
  {
    // Stack peek only if RSP is plausibly mapped (0x400000-0x10000000 image,
    // 0x200000000+ app maps, 0x7ee000000 libc). A wild RSP must not fault here.
    ucontext_t* uc2 = static_cast<ucontext_t*>(ctx);
    const uintptr_t sp = (uintptr_t)uc2->uc_mcontext.mc_rsp;
    const bool sp_ok = (sp >= 0x400000 && sp < 0x10000000) ||
                       (sp >= 0x200000000ULL && sp < 0x800000000ULL) ||
                       (sp >= 0x7ee000000ULL && sp < 0x7ef000000ULL);
    if (sp_ok)
    {
      const unsigned char* s = (const unsigned char*)sp;
      printf("[crash] rsp-bytes:");
      for (int i = 0; i < 16; i++)
        printf(" %02x", s[i]);
      printf("\n");
      fflush(stdout);
    }
    else
    {
      printf("[crash] rsp unmapped, skipping peek\n");
      fflush(stdout);
    }
  }
  ps5::debug::set_line(2, "CRASH sig=%d", signal);
  ps5::debug::set_line(3, "fault=%p pc=%p", addr, pc);
  {
    // No new imports allowed on PS5 stubs: identify candidate return addresses
    // already present on this thread's stack (our ELF text, JIT area).
    void* sp0 = nullptr;
#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp0));
#endif
    uintptr_t* stk = (uintptr_t*)sp0;
    for (int i = 0; i < 96; i++)
    {
      uintptr_t v = stk[i];
      if (v >= 0x400000 && v < 0x10000000)
      {
        printf("[crash] ret-elf st[%d]=0x%llx fileoff=0x%llx\n", i,
          (unsigned long long)v, (unsigned long long)(v - 0x400000));
      }
      else if (v >= 0x900000000ULL && v < 0x910000000ULL)
      {
        printf("[crash] ret-jit st[%d]=0x%llx\n", i, (unsigned long long)v);
      }
    }
    fflush(stdout);
  }
  {
    orbis_rec_dump();
    orbis_vtlb_dump();
    printf("[crash] threadid=%llu\n", (unsigned long long)pthread_self());
    fflush(stdout);
  }
  if (ctx)
  {
    ucontext_t* uc = static_cast<ucontext_t*>(ctx);
    printf("[crash] regs: rip=%p rsp=%p rbp=%p rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
      (void*)uc->uc_mcontext.mc_rip, (void*)uc->uc_mcontext.mc_rsp, (void*)uc->uc_mcontext.mc_rbp,
      (unsigned long long)uc->uc_mcontext.mc_rax, (unsigned long long)uc->uc_mcontext.mc_rbx,
      (unsigned long long)uc->uc_mcontext.mc_rcx, (unsigned long long)uc->uc_mcontext.mc_rdx,
      (unsigned long long)uc->uc_mcontext.mc_rsi, (unsigned long long)uc->uc_mcontext.mc_rdi,
      (unsigned long long)uc->uc_mcontext.mc_r8, (unsigned long long)uc->uc_mcontext.mc_r9,
      (unsigned long long)uc->uc_mcontext.mc_r10, (unsigned long long)uc->uc_mcontext.mc_r11,
      (unsigned long long)uc->uc_mcontext.mc_r12, (unsigned long long)uc->uc_mcontext.mc_r13,
      (unsigned long long)uc->uc_mcontext.mc_r14, (unsigned long long)uc->uc_mcontext.mc_r15);
    fflush(stdout);
  }
  // Raw stack dump: -O2 builds may omit frame pointers, so walk the raw stack
  // and print qwords that land in executable ranges (binary 0x400000-0x10000000,
  // libc/libkernel 0x7ee000000-0x7ef000000).
  void* sp = nullptr;
#if defined(__x86_64__)
  asm volatile("mov %%rsp, %0" : "=r"(sp));
#endif
  printf("[crash] rsp=%p\n", sp);
  uintptr_t* stack = (uintptr_t*)sp;
  for (int i = 0; i < 96; i++)
  {
    uintptr_t v = stack[i];
    if (v != 0)
      printf("[crash] st[%d]=0x%llx\n", i, (unsigned long long)v);
  }
  // Frame-pointer backtrace (return addresses need base-subtract like pc).
  void** fp = nullptr;
#if defined(__x86_64__)
  asm volatile("mov %%rbp, %0" : "=r"(fp));
#endif
  for (int i = 0; i < 10 && fp; i++)
  {
    void* ret = fp[1];
    if (!ret)
      break;
    printf("[crash] bt%d ret=%p\n", i, ret);
    void** next = (void**)fp[0];
    if (!next || next <= fp)
      break;
    fp = next;
  }
  fflush(stdout);
  if (orbis_log_drain)
    orbis_log_drain(); // vk-285-102: boot.log goes through a pipe; write it out before dying
  abort();
}
} // namespace CrashHandler
