// testutil.hxx — shared plumbing for the sqr_odbc test programs: the
// check/tally helpers and the spawn-a-real-sqrd harness (fork/exec on a
// scratch database and an ephemeral port, reading the port back from the
// LISTENING line).

#pragma once

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <print>
#include <string>
#include <string_view>

namespace testutil {

inline int npass = 0;
inline int nfail = 0;

inline void check(bool cond, std::string_view label) {
    if (cond) {
        ++npass;
        std::println("  OK   {}", label);
    } else {
        ++nfail;
        std::println("  FAIL {}", label);
    }
}

inline int summary(std::string_view suite) {
    std::println("{}: {} passed, {} failed", suite, npass, nfail);
    return nfail > 0 ? 1 : 0;
}

struct Server {
    pid_t pid = -1;
    int port = 0;
};

inline Server spawn_sqrd(const char* exe, const char* dbdir) {
    std::array<int, 2> p = {-1, -1};
    if (::pipe(p.data()) != 0) {
        return {};
    }
    const auto pid = ::fork();
    if (pid == 0) {
        ::dup2(p[1], 1);
        ::close(p[0]);
        ::close(p[1]);
        ::execl(exe, "sqrd", dbdir, "0", static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(p[1]);
    Server srv = {.pid = pid, .port = 0};
    {
        std::string line;
        char c = 0;
        while (::read(p[0], &c, 1) == 1 && c != '\n') {
            line.push_back(c);
        }
        if (line.starts_with("LISTENING ")) {
            srv.port = std::stoi(line.substr(10));
        }
    }
    ::close(p[0]);
    return srv;
}

inline void stop_sqrd(const Server& srv) {
    // SIGKILL, not SIGTERM: the ifx runtime prints a noisy traceback on
    // SIGTERM, and abrupt death is exactly the crash case sqr's journal is
    // designed to cover.
    if (srv.pid > 0) {
        ::kill(srv.pid, SIGKILL);
        ::waitpid(srv.pid, nullptr, 0);
    }
}

} // namespace testutil
