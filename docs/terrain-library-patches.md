# Terrain Library Patches

When replacing `src/my_terrain_library/` with a newer version, re-apply these patches.

## Patch 1: Abort flag for clean shutdown

**Problem:** `ServerChunkCache::getChunk()` has two blocking paths (main thread `while` loop and worker thread `future->join()`) that prevent threads from exiting during shutdown, causing the game to hang on close.

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

### `src/server/level/ServerChunkCache.cpp`

**Worker thread path** — replace the `future->join()` call with an abort-aware poll:

```cpp
// If not on main thread, dispatch and wait
if (std::this_thread::get_id() != m_mainThreadId) {
    if (m_abort.load(std::memory_order_acquire)) return nullptr;

    auto future = getChunkFuture(x, z, targetStatus, loadOrGenerate);

    // Poll with abort check instead of blocking join()
    while (!future->isDone()) {
        if (m_abort.load(std::memory_order_acquire)) return nullptr;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    auto result = future->getNow(nullptr);
    return result ? result->orElse(nullptr) : nullptr;
}
```

**Main thread path** — add abort check at top of the `while (!future->isDone())` loop:

```cpp
while (!future->isDone()) {
    if (m_abort.load(std::memory_order_acquire)) {
        return nullptr;
    }
    // ... existing code (runDistanceManagerUpdates, taskPoller, yield) ...
}
```
