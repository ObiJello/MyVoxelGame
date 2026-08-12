# File: cmake/PatchTracyAppleSampling.cmake
#
# Removes the root requirement from Tracy's Apple system tracing.
#
# Tracy 0.14 added prototype system tracing on Apple. It is SAMPLING only
# (QueueType::CallstackSample at 1000 Hz) — it does not capture context
# switches. SysTraceStart gates it behind an effective-uid check:
#
#     // check for elevated privileges
#     // (technically, since this is a software-based user-mode sampling, elevated
#     // privileges are unnecessary, but doing so keeps the behavior consistent with
#     // the system tracing in other platforms)
#     if( geteuid() != 0 ) return false;
#
# Upstream's own comment says root is unnecessary: it samples its OWN process
# via mach_task_self() / thread_suspend / thread_get_state and a frame-pointer
# walk. Nothing privileged is touched. The check exists purely so behaviour
# matches Windows and Linux.
#
# We drop it because the alternative — running the game under sudo — creates
# root-owned files in ~/Library/Application Support/obeycraft/ (saves,
# options.txt, worlds.json) that then break every subsequent normal run.
#
# Invoked as a FetchContent PATCH_COMMAND, so it MUST be idempotent: CMake can
# re-run the patch step on reconfigure, and a patch that fails the second time
# breaks the build. Re-running this is a no-op.

if(NOT DEFINED TRACY_SRC)
    message(FATAL_ERROR "PatchTracyAppleSampling: TRACY_SRC not set")
endif()

set(_target "${TRACY_SRC}/public/client/apple/TracyMach.cpp")
set(_marker "[MyVoxelGame patch]")

# Not an error: the file only exists in Tracy versions that have Apple system
# tracing. On an older pin there is simply nothing to patch.
if(NOT EXISTS "${_target}")
    message(STATUS "Tracy: no Apple system tracing source; skipping root-check patch")
    return()
endif()

file(READ "${_target}" _contents)

# Already patched — reconfigure, not a fresh fetch.
if(_contents MATCHES "\\[MyVoxelGame patch\\]")
    return()
endif()

set(_needle "    if( geteuid() != 0 ) return false;")
set(_replacement
"    // ${_marker} root check removed — see cmake/PatchTracyAppleSampling.cmake.
    // Upstream notes above that elevated privileges are technically unnecessary
    // for this user-mode self-sampling; running the game as root would leave
    // root-owned files in the game's application-support directory.
    // if( geteuid() != 0 ) return false;")

string(FIND "${_contents}" "${_needle}" _found)
if(_found EQUAL -1)
    # Loud, but NOT fatal: the game still builds and runs, you just get no
    # Apple sampling without sudo. Failing the build over a profiler-only
    # convenience would be the wrong trade.
    message(WARNING
        "Tracy: could not find the geteuid() guard in TracyMach.cpp — upstream "
        "changed it. Apple sampling will still require running the game as root. "
        "Re-check cmake/PatchTracyAppleSampling.cmake against the new source.")
    return()
endif()

string(REPLACE "${_needle}" "${_replacement}" _contents "${_contents}")
file(WRITE "${_target}" "${_contents}")
message(STATUS "Tracy: patched out Apple system-tracing root check (no sudo needed)")
