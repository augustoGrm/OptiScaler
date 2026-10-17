#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>

// Keep a native failure visible in CI, with module offsets even without private symbols.
inline void InstallNrCrashReport()
{
    SetUnhandledExceptionFilter(
        [](EXCEPTION_POINTERS* fault) -> LONG
        {
            auto describe = [](const char* label, void* address)
            {
                HMODULE module = nullptr;
                wchar_t path[MAX_PATH] {};
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(address), &module))
                    GetModuleFileNameW(module, path, MAX_PATH);
                std::fprintf(stderr, "NR_CRASH: %s %ls + 0x%llx\n", label, path,
                             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                             reinterpret_cast<uintptr_t>(module)));
            };
            std::fprintf(stderr, "NR_CRASH: exception 0x%08lx\n", fault->ExceptionRecord->ExceptionCode);
            describe("instruction", fault->ExceptionRecord->ExceptionAddress);
            void* stack[6] {};
            const auto count = CaptureStackBackTrace(0, 6, stack, nullptr);
            for (USHORT i = 0; i < count; ++i)
                describe("stack", stack[i]);
            std::fflush(stderr);
            return EXCEPTION_EXECUTE_HANDLER;
        });
}
