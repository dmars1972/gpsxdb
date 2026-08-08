// Diagnostic only -- not shipped, not part of any install path. Built solely
// by the `win_link_smoketest` target in the WIN32 branch of CMakeLists.txt.
//
// Why this exists: after -static fixed the missing-DLL load failure, the
// cross-compiled regional_install.exe started dying with 0xC0000374
// (STATUS_HEAP_CORRUPTION) on Windows, with no output at all -- not even on
// the unbuffered std::cerr path -- for every argv, which puts the fault
// BEFORE main(). None of this project's three linked translation units has a
// namespace-scope object with a dynamic initialiser, so the suspicion is the
// statically-linked libraries (libpq/libpgcommon/libpgport, OpenSSL, libpqxx)
// or the hand-ordered --start-group link line, not our own code.
//
// This binary links the exact same archives with the exact same options but
// compiles none of this project's sources. It prints three checkpoints, each
// flushed immediately so nothing is lost when the process is killed:
//
//   [1] early-ctor   -- a priority-101 C constructor, runs before C++ static
//                       init. Missing => fault is in CRT/loader-level setup,
//                       before any constructor runs at all.
//   [2] cpp-ctor     -- ordinary namespace-scope C++ static init.
//   [3] main         -- plus a real libpq call and a real (deliberately
//                       failing) pqxx connection attempt.
//
// Interpreting the result:
//   prints nothing            -> fault is below user code entirely (CRT/loader)
//   stops after [1] or [2]    -> a linked library's static init corrupts the heap
//   reaches [3] and exits 0   -> the libraries are innocent; the fault is in
//                                regional_install.cpp / NavDB.cpp /
//                                RegionalNodeMap.cpp's own initialisation
//
// The libpq/pqxx calls in main() are load-bearing, not decoration: a main()
// that referenced nothing would leave every libpq and OpenSSL archive member
// unreferenced, and ld would simply not link them -- so their initialisers
// would never run and this test would print "alive" while proving nothing.
// The connection is expected to fail; only the fact that it was attempted
// (and that it threw rather than crashed) matters.

#include <cstdio>
#include <exception>

#include <libpq-fe.h>
#include <pqxx/pqxx>

// stderr is unbuffered by default, but flush anyway -- a process killed by the
// heap validator gets no cleanup, so anything still sitting in a buffer is
// lost, and "how far did it get" is the entire point of this binary.
static void checkpoint(const char* msg) {
    std::fputs(msg, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

extern "C" __attribute__((constructor(101))) void early_ctor_probe(void) {
    checkpoint("[1] early-ctor reached");
}

namespace {
struct CppStaticInitProbe {
    CppStaticInitProbe() { checkpoint("[2] cpp-ctor reached"); }
};
CppStaticInitProbe cpp_static_init_probe;
} // namespace

int main() {
    checkpoint("[3] main entered");

    std::fprintf(stderr, "    libpq version: %d\n", PQlibVersion());
    std::fflush(stderr);

    // Expected to fail -- port 1 on loopback isn't Postgres. The point is to
    // drag libpq's connection machinery and OpenSSL's init in and run them.
    try {
        pqxx::connection c(
            "host=127.0.0.1 port=1 dbname=none user=none connect_timeout=1");
        checkpoint("    unexpectedly connected");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "    connect failed as expected: %s\n", e.what());
        std::fflush(stderr);
    }

    checkpoint("alive -- linked libraries initialised without corrupting the heap");
    return 0;
}
