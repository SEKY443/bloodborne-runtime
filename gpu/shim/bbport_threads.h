// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.
// macOS has neither affinity masks nor SCHED_IDLE: all hardware threads count, and helpers run
// at the utility QoS class (background QoS would keep them on the efficiency cores).

#pragma once

#include <algorithm>
#include <cstdlib>
#include <sched.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace BbThreads {

/// Hardware threads available to the process.
inline unsigned Available() {
#ifdef __APPLE__
    return std::max(1u, std::thread::hardware_concurrency());
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
#endif
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
#endif
}

/// The calling thread is on the frame's critical path (the GPU command processor, the recorders):
/// on Apple platforms the user-interactive QoS class, so the scheduler keeps it on the performance
/// cores (an iPhone ran these threads on its efficiency cores about 78% of the time). Elsewhere
/// nothing changes. BB_THREAD_QOS=0: the default class, as before.
inline void MakeCritical() {
#ifdef __APPLE__
    const char* env = std::getenv("BB_THREAD_QOS");
    if (!(env && env[0] == '0')) {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
#endif
}

} // namespace BbThreads
