// Cancel a worker's synchronous Windows I/O without detaching a live thread.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <mutex>
namespace luma {
class WorkerCancellation {
public:
    class Scope {
    public:
        explicit Scope(WorkerCancellation& owner) : owner_(owner) {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                            &owner_.thread_, 0, FALSE, DUPLICATE_SAME_ACCESS);
        }
        ~Scope() {
            std::lock_guard<std::mutex> lock(owner_.mutex_);
            if (owner_.thread_) CloseHandle(owner_.thread_);
            owner_.thread_ = nullptr;
        }
    private: WorkerCancellation& owner_;
    };
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_) CancelSynchronousIo(thread_);
    }
private:
    std::mutex mutex_;
    HANDLE thread_ = nullptr;
};
}  // namespace luma

#else
// Portable protocol tests include output declarations but never start Windows workers.
namespace luma {
class WorkerCancellation {
public:
    class Scope { public: explicit Scope(WorkerCancellation&) {} };
    void Cancel() {}
};
}
#endif
