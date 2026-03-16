# Terrain Library Patches

When replacing `src/my_terrain_library/` with a newer version, re-apply these patches.

## Patch 1: Abort flag for clean shutdown

**Problem:** `ServerChunkCache::getChunk()` has a blocking `while (!future->isDone())` loop that prevents worker threads from exiting during shutdown, causing the game to hang on close.

**Files to modify:**

### `include/server/level/ServerChunkCache.h`

Add to the **public** section (after `setTaskPoller`):

```cpp
void requestAbort() { m_abort.store(true, std::memory_order_release); }
bool isAbortRequested() const { return m_abort.load(std::memory_order_acquire); }
```

Add to the **private** section (after `m_taskPoller`):

```cpp
std::atomic<bool> m_abort{false};
```

Also add `#include <atomic>` if not already present (it should be via other headers).

### `src/server/level/ServerChunkCache.cpp`

In `getChunk()`, add an abort check at the top of the `while (!future->isDone())` loop:

```cpp
while (!future->isDone()) {
    // ADD THIS:
    if (m_abort.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // ... existing code (runDistanceManagerUpdates, taskPoller, yield) ...
}
```
