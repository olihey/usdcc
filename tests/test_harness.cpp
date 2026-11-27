#include "test_harness.h"

#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// After windows.h, which it needs.
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>  // <ucontext.h> wants _XOPEN_SOURCE there
#else
#include <ucontext.h>  // REG_RIP: with _GNU_SOURCE, which g++ defines
#endif
#endif

namespace usdcc::test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int& failures() {
    static int count = 0;
    return count;
}

int& passes() {
    static int count = 0;
    return count;
}

namespace {

// The test that's running, for the crash report.
const char* g_current = "(between tests)";

// A crash prints where it happened before the process dies, so a crash on a CI runner,
// which keeps no dump, still says which code it was in. Written to stderr, which CTest
// shows with --output-on-failure and --verbose.
#if defined(_WIN32)

LONG WINAPI reportCrash(EXCEPTION_POINTERS* info) {
    // Once: a crash while reporting goes to Windows' own handling.
    static volatile LONG reporting = 0;
    if (InterlockedExchange(&reporting, 1)) return EXCEPTION_CONTINUE_SEARCH;

    std::fprintf(stderr, "\n*** Crash in test [%s]: exception 0x%08lx at %p\n", g_current,
                 info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);  // the PDBs beside each DLL and the executable

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame = {};
#if defined(_M_ARM64)
    const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrStack.Offset = context.Sp;
    frame.AddrFrame.Offset = context.Fp;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrFrame.Offset = context.Rbp;
#endif
    frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    for (int depth = 0; depth < 64; ++depth) {
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        const bool named = SymFromAddr(process, address, &offset, symbol);
        char module[MAX_PATH] = "?";
        if (const DWORD64 base = SymGetModuleBase64(process, address)) {
            GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, MAX_PATH);
        }
        const char* moduleName = std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module;
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD column = 0;
        if (named && SymGetLineFromAddr64(process, address, &column, &line)) {
            std::fprintf(stderr, "  #%-2d %s!%s + 0x%llx  (%s:%lu)\n", depth, moduleName, symbol->Name,
                         static_cast<unsigned long long>(offset), line.FileName, line.LineNumber);
        } else if (named) {
            std::fprintf(stderr, "  #%-2d %s!%s + 0x%llx\n", depth, moduleName, symbol->Name, static_cast<unsigned long long>(offset));
        } else {
            std::fprintf(stderr, "  #%-2d %s!0x%llx\n", depth, moduleName, static_cast<unsigned long long>(address));
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;  // then the usual crash, which CTest reports
}

void installCrashReport() {
    SetUnhandledExceptionFilter(reportCrash);
    // No "has stopped working" dialog to wait on, on a runner nobody watches.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

#else

void writeText(const char* text) {
    // Only async-signal-safe calls here.
    const ssize_t ignored = write(STDERR_FILENO, text, std::strlen(text));
    (void)ignored;
}

// Where it crashed, from the signal's context: backtrace() can miss that frame, a leaf
// function's that has no frame record of its own.
void* crashAddress(void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    return reinterpret_cast<void*>(uc->uc_mcontext->__ss.__pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    return reinterpret_cast<void*>(uc->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__aarch64__)
    return reinterpret_cast<void*>(uc->uc_mcontext.pc);
#elif defined(__linux__) && defined(__x86_64__)
    return reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    (void)uc;
    return nullptr;
#endif
}

void reportCrash(int signal, siginfo_t*, void* context) {
    writeText("\n*** Crash in test [");
    writeText(g_current);
    writeText("]: ");
    writeText(signal == SIGSEGV ? "SIGSEGV" : signal == SIGBUS ? "SIGBUS" : signal == SIGILL ? "SIGILL" : signal == SIGFPE ? "SIGFPE" : "SIGABRT");
    writeText("\n");
    if (void* at = crashAddress(context)) {
        writeText("at:\n");
        backtrace_symbols_fd(&at, 1, STDERR_FILENO);
        writeText("called from:\n");
    }
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    // Then the signal's own outcome, which CTest reports.
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void installCrashReport() {
    void* warm[1];
    backtrace(warm, 1);  // loads what backtrace() needs now, not in the handler
    for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        struct sigaction action = {};
        action.sa_sigaction = reportCrash;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(signal, &action, nullptr);
    }
}

#endif

}  // namespace

int runTests(int argc, char** argv) {
    installCrashReport();
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (const auto& test : registry()) {
        if (filter && !std::strstr(test.name, filter)) {
            continue;
        }
        const int failuresBefore = failures();
        std::printf("[%s]\n", test.name);
        std::fflush(stdout);  // so that a crash says which test it was
        g_current = test.name;
        test.run();
        g_current = "(between tests)";
        if (failures() != failuresBefore) {
            std::printf("  -> %d failure(s)\n", failures() - failuresBefore);
        }
        ++ran;
    }
    std::printf("\n%d test(s), %d checks passed, %d failed\n", ran, passes(), failures());
    return failures() == 0 && ran > 0 ? 0 : 1;
}

}  // namespace usdcc::test
