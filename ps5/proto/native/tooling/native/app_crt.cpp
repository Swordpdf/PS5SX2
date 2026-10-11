/*
 * ps5-native-app-boilerplate - Native application startup.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Initializes the platform runtime, runs static constructors and main, and
 * hands normal process termination back to the platform runtime.
 */

#include <cstddef>
#include <cstdint>
#ifdef PS5_INIT_TRACE
#include <cstdio>
#endif

using Destructor = void (*)();
using Initializer = void (*)();

extern "C"
{
    void _init_env(void *process_parameters);
    void *mmap(void *address, std::size_t size, int protection, int flags, int fd, long offset);
    int atexit(Destructor callback);
    [[noreturn]] void exit(int status);
    int application_main(int argc, char **argv, char **envp) __asm__("main");

    extern Initializer __preinit_array_start[] __attribute__((weak));
    extern Initializer __preinit_array_end[] __attribute__((weak));
    extern Initializer __init_array_start[] __attribute__((weak));
    extern Initializer __init_array_end[] __attribute__((weak));
    extern Initializer __fini_array_start[] __attribute__((weak));
    extern Initializer __fini_array_end[] __attribute__((weak));

    __attribute__((visibility("default"))) extern void *ps5_early_stack_guard_address;
}

extern "C" void *ps5_early_stack_guard_address = nullptr;

namespace
{
#ifdef PS5_INIT_TRACE
void trace_initializer(std::size_t index, Initializer initializer)
{
    FILE *file = std::fopen("/data/PCSX2/init.log", "a");
    if (file == nullptr)
        return;
    std::fprintf(file, "before[%zu]=%p\n", index, reinterpret_cast<void *>(initializer));
    std::fclose(file);
}
#endif

void run_forward(Initializer *first, Initializer *last) noexcept
{
    if (first == nullptr || last == nullptr)
        return;
#ifdef PS5_INIT_TRACE
    std::size_t index = 0;
#endif
    while (first != last)
    {
#ifdef PS5_INIT_TRACE
        trace_initializer(index++, *first);
#endif
#ifdef PS5_SKIP_CONSOLE_INIT
        if (index == 4 || index == 5 || index == 7 || index == 22 || index == 30)
        {
            ++first;
            continue;
        }
#endif
        (*first++)();
    }
}

void run_reverse(Initializer *first, Initializer *last) noexcept
{
    if (first == nullptr || last == nullptr)
        return;
    while (last != first)
        (*--last)();
}
} // namespace

extern "C" __attribute__((weak)) void catchReturnFromMain(int status)
{
    (void)status;
}

extern "C" void _init()
{
    run_forward(__preinit_array_start, __preinit_array_end);
    run_forward(__init_array_start, __init_array_end);
}

extern "C" void _fini()
{
    run_reverse(__fini_array_start, __fini_array_end);
}

extern "C" [[noreturn]] __attribute__((visibility("default"))) void
_start(void *process_parameters, Destructor loader_teardown)
{
    const int argc = *static_cast<const int *>(process_parameters);
    auto *parameters = static_cast<std::uint8_t *>(process_parameters);
    auto **argv = reinterpret_cast<char **>(parameters + sizeof(std::uint64_t));

    _init_env(process_parameters);
#ifdef PS5_EARLY_STACK_GUARD
    ps5_early_stack_guard_address = mmap(nullptr, 0x8000000, 3, 0, -1, 0);
#endif
    if (loader_teardown != nullptr)
        (void)atexit(loader_teardown);
    (void)atexit(_fini);
    _init();
    const int status = application_main(argc, argv, nullptr);
    catchReturnFromMain(status);
    exit(status);
}
