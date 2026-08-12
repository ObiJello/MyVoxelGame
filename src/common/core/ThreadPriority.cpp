// File: src/common/core/ThreadPriority.cpp
#include "ThreadPriority.hpp"

#if defined(__APPLE__)
    #include <pthread.h>
    #include <sys/qos.h>
#elif defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <sys/resource.h>
    #include <sys/time.h>
    #include <unistd.h>
    #if defined(__linux__)
        #include <sys/syscall.h>
    #endif
#endif

namespace Core {

    void SetCurrentThreadPriority(ThreadPriorityClass cls) {
#if defined(__APPLE__)
        // pthread_set_qos_class_self_np is the only supported way to set a QoS
        // class, and it applies to the calling thread only — there is no
        // "set another thread's QoS" API, which is why every caller has to be
        // inside its own thread entry point.
        qos_class_t qos = QOS_CLASS_DEFAULT;
        switch (cls) {
            case ThreadPriorityClass::Interactive: qos = QOS_CLASS_USER_INTERACTIVE; break;
            case ThreadPriorityClass::Elevated:    qos = QOS_CLASS_USER_INITIATED;   break;
            case ThreadPriorityClass::Throughput:  qos = QOS_CLASS_UTILITY;          break;
        }
        pthread_set_qos_class_self_np(qos, 0);

#elif defined(_WIN32)
        // Windows has no QoS classes; the nearest equivalent is the thread
        // priority offset within the process priority class. HIGHEST rather
        // than TIME_CRITICAL for the frame thread — TIME_CRITICAL outranks
        // most system threads and can starve input and audio.
        int priority = THREAD_PRIORITY_NORMAL;
        switch (cls) {
            case ThreadPriorityClass::Interactive: priority = THREAD_PRIORITY_HIGHEST;      break;
            case ThreadPriorityClass::Elevated:    priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
            case ThreadPriorityClass::Throughput:  priority = THREAD_PRIORITY_BELOW_NORMAL; break;
        }
        SetThreadPriority(GetCurrentThread(), priority);

#elif defined(__linux__)
        // Only ever LOWER priority here. Raising it (negative nice) needs
        // CAP_SYS_NICE or a matching RLIMIT_NICE, which a game should not
        // demand — so Interactive/Elevated are left at the inherited default
        // and only throughput work steps down, which needs no privileges.
        if (cls == ThreadPriorityClass::Throughput) {
            const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
            setpriority(PRIO_PROCESS, static_cast<id_t>(tid), 5);
        }

#else
        (void)cls;   // Unknown platform: scheduling hints are advisory anyway.
#endif
    }

} // namespace Core
