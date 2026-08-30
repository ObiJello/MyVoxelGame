// File: src/common/core/TickParallel.cpp
#include "common/core/TickParallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace Core {

    namespace {

        // One in-flight range. Workers claim [cursor, cursor+grain) until the
        // range is exhausted; the caller waits for every claim to complete.
        struct Batch {
            const std::function<void(size_t)>* fn = nullptr;
            size_t                             count = 0;
            size_t                             grain = 1;
            std::atomic<size_t>                cursor{0};
            std::atomic<size_t>                remaining{0};
            // Workers currently holding a pointer to THIS batch. Incremented
            // under the pool lock while the batch is still published,
            // decremented after the worker has stopped touching it. This is
            // what makes it safe for the batch to live on the caller's stack —
            // see the join protocol in ParallelFor.
            std::atomic<int>                   active{0};
        };

        struct Pool {
            std::vector<std::thread> workers;
            std::mutex               m;
            std::condition_variable  cv;
            // Every batch currently published, from any calling thread. The
            // server tick thread and the client's render/tick thread both
            // fan out during a mass event — a hundred thousand falling blocks
            // are simulated on one and gathered for drawing on the other —
            // and this used to hold ONE batch behind a try-lock, so whichever
            // caller arrived second ran its whole range inline. Measured: the
            // client's gather stayed at its serial 5.6 ms a frame and the
            // server's classify pass at 9 ms a tick, each because the other
            // side held the pool. Workers now drain whichever batches are
            // published, in list order, and every caller keeps its own
            // thread as a runner.
            std::vector<Batch*>      batches;
            uint64_t                 epoch = 0;   // bumped per publish
            bool                     stop  = false;

            // Stop and join here as well as in ShutdownTickParallel. Without
            // this, a process that exits without calling the explicit shutdown
            // destroys joinable std::threads and std::terminate aborts the
            // program — verified: a stress run that skipped the call died with
            // "libc++abi: terminating" AFTER passing every check. Exit paths
            // that bypass the normal teardown (an early return, a fatal error)
            // must not turn into a crash on the way out.
            //
            // Safe in a static destructor: the workers touch nothing but this
            // Pool, and no batch can be in flight once main has returned.
            ~Pool() {
                // Joins UNCONDITIONALLY. An early `if (stop) return;` here was a
                // latent abort: ShutdownTickParallel constructs the Pool (its
                // static is independent of the call_once flag) and can set stop
                // BEFORE the pool is ever used. A later ParallelFor then fires
                // call_once for the first time, spawns workers that immediately
                // see stop and return — and a returned thread is still
                // joinable. The destructor would skip the join, the vector
                // member would destroy joinable threads, and std::terminate
                // would abort the process at exit.
                //
                // Harmless after a normal ShutdownTickParallel: it already
                // cleared `workers`, so the loop below runs over an empty
                // vector.
                {
                    std::lock_guard<std::mutex> lk(m);
                    stop = true;
                }
                cv.notify_all();
                for (auto& t : workers) if (t.joinable()) t.join();
                workers.clear();
            }
        };

        Pool&           ThePool()   { static Pool p;  return p; }
        std::once_flag& InitFlag()  { static std::once_flag f; return f; }

        // A nested ParallelFor runs inline instead of deadlocking against a
        // pool that is already committed to an outer range.
        thread_local bool t_inParallel = false;

        // Drain claims until the range is exhausted. Shared by the workers and
        // by the calling thread, which is also a runner.
        void RunBatch(Batch& b) {
            const size_t n = b.count;
            for (;;) {
                const size_t i = b.cursor.fetch_add(b.grain, std::memory_order_relaxed);
                if (i >= n) break;
                const size_t end = (i + b.grain < n) ? i + b.grain : n;
                for (size_t k = i; k < end; ++k) (*b.fn)(k);
                b.remaining.fetch_sub(end - i, std::memory_order_acq_rel);
            }
        }

        void WorkerLoop(Pool& p) {
            std::unique_lock<std::mutex> lk(p.m);
            for (;;) {
                if (p.stop) return;
                // The first published batch with unclaimed indices. A batch
                // whose cursor has passed its count may still be running its
                // last claims, but there is nothing left in it to take.
                Batch* b = nullptr;
                for (Batch* c : p.batches) {
                    if (c->cursor.load(std::memory_order_relaxed) < c->count) { b = c; break; }
                }
                if (!b) {
                    // Nothing to do: sleep until a publish or shutdown bumps
                    // the epoch. Spurious wakeups just re-scan the list.
                    const uint64_t seen = p.epoch;
                    p.cv.wait(lk, [&] { return p.stop || p.epoch != seen; });
                    continue;
                }
                // Claim a reference to the batch while it is still published.
                // Once the caller unpublishes it under this same lock, no
                // further worker can get here, so the caller can then wait for
                // `active` to drain and know nobody is left holding a pointer.
                b->active.fetch_add(1, std::memory_order_relaxed);
                lk.unlock();
                t_inParallel = true;
                RunBatch(*b);
                t_inParallel = false;
                b->active.fetch_sub(1, std::memory_order_release);
                lk.lock();
            }
        }

    } // namespace

    void InitTickParallel() {
        std::call_once(InitFlag(), [] {
            Pool& p = ThePool();
            // Leave headroom: the render thread, the mesh workers and the
            // server worker pool are all live. Oversubscribing makes the tick
            // slower, not faster — a batch cannot finish until its slowest
            // claim does, and a descheduled runner stalls the whole join.
            const unsigned hw = std::thread::hardware_concurrency();
            const unsigned want = hw > 3 ? (hw - 2) : 1;
            const unsigned spawn = want > 1 ? want - 1 : 0;  // caller is a runner
            // Never spawn into a pool that has already been shut down — those
            // threads would exit immediately and only exist to be joined.
            {
                std::lock_guard<std::mutex> lk(p.m);
                if (p.stop) return;
            }
            p.workers.reserve(spawn);
            for (unsigned i = 0; i < spawn; ++i) {
                p.workers.emplace_back([&p] { WorkerLoop(p); });
            }
        });
    }

    size_t ParallelWidth() {
        InitTickParallel();
        Pool& p = ThePool();
        std::lock_guard<std::mutex> lk(p.m);
        return p.workers.size() + 1;
    }

    void ShutdownTickParallel() {
        Pool& p = ThePool();
        {
            std::lock_guard<std::mutex> lk(p.m);
            if (p.stop) return;
            p.stop = true;
        }
        p.cv.notify_all();
        for (auto& t : p.workers) if (t.joinable()) t.join();
        p.workers.clear();
    }

    // noexcept is load-bearing, not decoration. Without it, an exception out of
    // fn on the CALLING thread unwinds straight past this frame: `b` is
    // destroyed while workers still hold `p.batch == &b` and are calling
    // through `b.fn`, which points at a std::function the unwind is also
    // destroying. That is a hard use-after-free. With noexcept the terminate
    // handler is found at this boundary, so the frame owning `b` is never
    // popped.
    void ParallelFor(size_t count, size_t grain,
                     const std::function<void(size_t)>& fn) noexcept {
        if (count == 0) return;
        if (grain == 0) grain = 1;

        InitTickParallel();
        Pool& p = ThePool();

        // Inline when there is nothing to gain, when the pool is empty or shut
        // down, or when we are already inside a batch (no nesting).
        bool poolEmpty;
        {
            std::lock_guard<std::mutex> lk(p.m);
            poolEmpty = p.workers.empty();
        }
        if (t_inParallel || poolEmpty || count <= grain) {
            for (size_t i = 0; i < count; ++i) fn(i);
            return;
        }

        Batch b;
        b.fn    = &fn;
        b.count = count;
        b.grain = grain;
        b.cursor.store(0, std::memory_order_relaxed);
        b.remaining.store(count, std::memory_order_relaxed);

        // Publish under the lock, RUN outside it. Executing fn while holding
        // p.m would block every worker at the top of WorkerLoop for the
        // duration of arbitrary user code.
        bool inlineFallback = false;
        {
            std::lock_guard<std::mutex> lk(p.m);
            if (p.stop) {                      // shut down between the checks
                inlineFallback = true;
            } else {
                p.batches.push_back(&b);
                ++p.epoch;
            }
        }
        if (inlineFallback) {
            t_inParallel = true;               // hold the no-nesting invariant here too
            for (size_t i = 0; i < count; ++i) fn(i);
            t_inParallel = false;
            return;
        }
        p.cv.notify_all();

        // The caller is a runner too, so an N-worker pool gives N+1 runners.
        t_inParallel = true;
        RunBatch(b);
        t_inParallel = false;

        // ── Join, in two stages, and BOTH are required ──────────────────────
        //
        // remaining == 0 says every index has been executed. It does NOT say
        // every worker has stopped touching `b`: the worker that performed the
        // last fetch_sub goes around the loop once more and reads b.cursor to
        // discover the range is exhausted. Returning here would destroy `b`
        // out from under that read.
        //
        // So: wait for the work, then unpublish the batch under the lock (after
        // which no new worker can pick it up), then wait for the workers that
        // did pick it up to let go.
        while (b.remaining.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        {
            std::lock_guard<std::mutex> lk(p.m);
            p.batches.erase(std::find(p.batches.begin(), p.batches.end(), &b));
        }
        while (b.active.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

} // namespace Core
