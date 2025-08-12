Runbook: Adding a New Thread with io_context

This guide covers adding new threads that use Boost.Asio's io_context pattern, following the established work guard and lifecycle management practices.

Overview Pattern

All new threads that perform async operations should follow this pattern:
1. Create dedicated io_context for the thread
2. Install work guard to prevent premature exit
3. Start thread running io_context::run()
4. Provide clean shutdown mechanism
5. Join thread before destroying resources

Step 1: Thread Class Structure

Basic Thread Manager Template

    // Example: AudioStreamingThread
    class AudioStreamingThread {
        public:
            AudioStreamingThread();
            ~AudioStreamingThread();
    
            // Non-copyable, non-movable
            AudioStreamingThread(const AudioStreamingThread&) = delete;
            AudioStreamingThread& operator=(const AudioStreamingThread&) = delete;

            // Lifecycle
            bool Start();
            void Stop();
            bool IsRunning() const { return m_running.load(); }

            // Work submission
            void StreamAudio(const AudioBuffer& buffer);
            void SetVolume(float volume);

        private:
            // Async operation completion handlers
            void OnStreamComplete(const boost::system::error_code& error, size_t bytesTransferred);
            void OnVolumeChanged(float newVolume);

            // Thread management  
            std::unique_ptr<boost::asio::io_context> m_ioContext;
            std::unique_ptr<std::thread> m_thread;
            using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
            std::unique_ptr<WorkGuard> m_workGuard;

            // Thread state
            std::atomic<bool> m_running{false};
            std::atomic<bool> m_shouldStop{false};

            // Thread-specific resources
            AudioDevice m_audioDevice;
            std::queue<AudioBuffer> m_bufferQueue;
    };

Step 2: Implementation Pattern

Constructor - Resource Initialization Only

    AudioStreamingThread::AudioStreamingThread() {
        // Only initialize resources, don't start thread yet
        Log::Info("AudioStreamingThread created");
    }
    
    AudioStreamingThread::~AudioStreamingThread() {
        if (m_running.load()) {
            Log::Warning("AudioStreamingThread destroyed while running - forcing stop");
            Stop();
        }
        Log::Info("AudioStreamingThread destroyed");
    }
    
    Start() - Thread Creation and Work Guard Setup
    
    bool AudioStreamingThread::Start() {
        if (m_running.load()) {
            Log::Warning("AudioStreamingThread already running");
            return false;
        }

        Log::Info("Starting AudioStreamingThread...");

        // Create fresh io_context
        m_ioContext = std::make_unique<boost::asio::io_context>();

        // Initialize audio device on main thread (may require specific context)
        if (!m_audioDevice.Initialize()) {
            Log::Error("Failed to initialize audio device");
            return false;
        }

        // Create work guard to keep io_context alive
        m_workGuard = std::make_unique<WorkGuard>(
            boost::asio::make_work_guard(*m_ioContext)
        );

        // Start thread
        m_shouldStop.store(false);
        m_running.store(true);

        m_thread = std::make_unique<std::thread>([this]() {
            Log::Info("AudioStreamingThread started (tid: %zu)",
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));

            try {
                // This runs until Stop() is called
                m_ioContext->run();
                Log::Info("AudioStreamingThread exiting normally");
            } catch (const std::exception& e) {
                Log::Error("AudioStreamingThread exception: %s", e.what());
            }

            m_running.store(false);
        });

        Log::Info("✓ AudioStreamingThread started successfully");
        return true;
    }

    Stop() - Clean Shutdown Sequence
    
    void AudioStreamingThread::Stop() {
        if (!m_running.load()) {
            return;
        }

        Log::Info("Stopping AudioStreamingThread...");

        // Signal thread to stop accepting new work
        m_shouldStop.store(true);

        // Remove work guard - allows io_context to exit when queue empty
        if (m_workGuard) {
            m_workGuard.reset();
        }

        // Stop io_context event loop
        if (m_ioContext) {
            m_ioContext->stop();
        }

        // Wait for thread to finish current operations and exit
        if (m_thread && m_thread->joinable()) {
            Log::Debug("Waiting for AudioStreamingThread to finish...");
            m_thread->join();
            Log::Debug("AudioStreamingThread finished");
        }

        // Clean up resources
        m_thread.reset();
        m_ioContext.reset();
        m_audioDevice.Shutdown();

        m_running.store(false);
        Log::Info("✓ AudioStreamingThread stopped");
    }

Step 3: Work Submission Pattern

Async Work Submission

    void AudioStreamingThread::StreamAudio(const AudioBuffer& buffer) {
        if (!m_running.load() || m_shouldStop.load()) {
            Log::Warning("Cannot stream audio - thread not running");
            return;
        }

        // Post work to io_context thread
        boost::asio::post(*m_ioContext, [this, buffer]() {
            // This runs on the AudioStreamingThread
            if (m_shouldStop.load()) {
                return; // Skip work if shutting down
            }

            try {
                // Perform async audio streaming operation
                m_audioDevice.AsyncWrite(buffer,
                  [this](const boost::system::error_code& error, size_t bytesTransferred) {
                      OnStreamComplete(error, bytesTransferred);
                  });
            } catch (const std::exception& e) {
                Log::Error("Audio streaming failed: %s", e.what());
            }
        });
    }
    
    void AudioStreamingThread::SetVolume(float volume) {
        if (!m_running.load()) {
            return;
        }

        // Post volume change to io_context thread
        boost::asio::post(*m_ioContext, [this, volume]() {
            if (!m_shouldStop.load()) {
                OnVolumeChanged(volume);
            }
        });
    }

Completion Handlers (Run on io_context Thread)

    void AudioStreamingThread::OnStreamComplete(const boost::system::error_code& error, size_t bytesTransferred) {
        // This method runs on AudioStreamingThread

        if (error) {
            Log::Warning("Audio stream error: %s", error.message().c_str());
            return;
        }

        Log::Debug("Streamed %zu bytes to audio device", bytesTransferred);

        // Check for more buffers to stream
        if (!m_bufferQueue.empty() && !m_shouldStop.load()) {
            auto nextBuffer = m_bufferQueue.front();
            m_bufferQueue.pop();

            // Continue streaming
            m_audioDevice.AsyncWrite(nextBuffer,
              [this](const boost::system::error_code& error, size_t bytes) {
                  OnStreamComplete(error, bytes);
              });
        }
    }

Step 4: Integration with Main Systems

Global Management Pattern

    // In global system manager
    class AudioSystem {
        private:
            std::unique_ptr<AudioStreamingThread> m_streamingThread;
    
        public:
            bool Initialize() {
                m_streamingThread = std::make_unique<AudioStreamingThread>();
    
                if (!m_streamingThread->Start()) {
                    Log::Error("Failed to start audio streaming thread");
                    return false;
                }

                return true;
            }

            void Shutdown() {
                if (m_streamingThread) {
                    m_streamingThread->Stop();
                    m_streamingThread.reset();
                }
            }

            void PlaySound(const AudioBuffer& buffer) {
                if (m_streamingThread) {
                    m_streamingThread->StreamAudio(buffer);
                }
            }
    };

Startup Order in Main Thread

    // In main application startup
    bool Application::Start() {
        // Initialize systems in dependency order
        if (!m_audioSystem->Initialize()) {
            Log::Error("Audio system initialization failed");
            return false;
        }

        if (!m_networkSystem->Initialize()) {
            Log::Error("Network system initialization failed");
            return false;
        }

        // Other systems...

        return true;
    }

Shutdown Order in Main Thread

    // In application shutdown
    void Application::Stop() {
        Log::Info("Shutting down application systems...");

        // Stop systems in reverse dependency order
        if (m_networkSystem) {
            m_networkSystem->Shutdown();
        }

        if (m_audioSystem) {
            m_audioSystem->Shutdown();
        }

        Log::Info("✓ All systems stopped");
    }

Step 5: Error Handling and Recovery

Exception Safety in Thread Loop

    // In thread start lambda
    m_thread = std::make_unique<std::thread>([this]() {
        Log::Info("CustomThread started");

        try {
            m_ioContext->run();
        } catch (const boost::system::system_error& e) {
            Log::Error("CustomThread system error: %s", e.what());
            // Could attempt recovery here
        } catch (const std::exception& e) {
            Log::Error("CustomThread exception: %s", e.what());
        } catch (...) {
            Log::Error("CustomThread unknown exception");
        }

        m_running.store(false);
        Log::Info("CustomThread exited");
    });

Resource Cleanup on Error

    bool CustomThread::Start() {
        try {
        // ... initialization code
        return true;
        } catch (const std::exception& e) {
            Log::Error("CustomThread start failed: %s", e.what());

            // Clean up partial initialization
            if (m_workGuard) {
                m_workGuard.reset();
            }
            if (m_thread && m_thread->joinable()) {
                m_ioContext->stop();
                m_thread->join();
            }
            m_thread.reset();
            m_ioContext.reset();

            m_running.store(false);
            return false;
        }
    }

Step 6: Common Patterns and Utilities

Timer-Based Work

    class PeriodicThread {
        private:
            std::unique_ptr<boost::asio::steady_timer> m_timer;
    
            void SchedulePeriodicWork() {
                m_timer = std::make_unique<boost::asio::steady_timer>(*m_ioContext);
                m_timer->expires_after(std::chrono::seconds(1));
                m_timer->async_wait([this](const boost::system::error_code& error) {
                    if (!error && !m_shouldStop.load()) {
                        DoPeriodicWork();
                        SchedulePeriodicWork(); // Reschedule
                    }
                });
        }
    };

Thread-Safe Communication

    // Using thread-safe queue for cross-thread communication
    template<typename T>
    class ThreadSafeQueue {
        std::queue<T> m_queue;
        mutable std::mutex m_mutex;
        std::condition_variable m_condition;
    
        public:
            void push(T item) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push(std::move(item));
                m_condition.notify_one();
            }

            bool try_pop(T& item) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_queue.empty()) return false;

                item = std::move(m_queue.front());
                m_queue.pop();
                return true;
            }
    };

Shutdown Checklist

When adding a new thread, ensure you handle these shutdown scenarios:

- ✅ Normal Shutdown: Stop() called, thread exits cleanly
- ✅ Exception in Thread: Thread exits, resources cleaned up
- ✅ Destructor Called While Running: Force stop, clean up
- ✅ Work Guard Removed: io_context exits when queue empty
- ✅ io_context::stop(): Pending operations cancelled gracefully
- ✅ Thread Join: Main thread waits for worker thread completion
- ✅ Resource Cleanup: All heap allocations and handles freed

Performance Considerations

Thread Affinity (Optional)

    // Set thread affinity for real-time threads
    #ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset); // Pin to CPU core 2
        pthread_setaffinity_np(m_thread->native_handle(), sizeof(cpu_set_t), &cpuset);
    #endif

Thread Priority (Optional)

    // Set high priority for time-critical threads
    #ifdef _WIN32
        SetThreadPriority(m_thread->native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    #elif defined(__linux__)
        struct sched_param param;
        param.sched_priority = 10;
        pthread_setschedparam(m_thread->native_handle(), SCHED_RR, &param);
    #endif

This pattern ensures robust, maintainable thread management that integrates cleanly with the existing architecture while following Boost.Asio best practices.