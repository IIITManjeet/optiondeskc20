#pragma once
// Linux thread helpers: CPU pinning and names visible in `top -H` / `ps -L`.

#include <chrono>
#include <cstdint>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace od {

inline std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Pin the calling thread to one CPU. Keeps its cache warm and stops the scheduler
// migrating it; pair with `isolcpus=` on the kernel cmdline to keep other work off.
inline bool pin_current_thread(int cpu) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

// Max 15 chars on Linux.
inline void name_current_thread(const char* name) {
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

}  // namespace od
