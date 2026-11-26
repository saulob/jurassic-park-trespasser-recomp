// One job at a time on a worker thread, as the Dragon's Crown launcher prepares a character's appearance. The job owns its
// inputs and returns its result by value; only the thread that collects it uses the result.
#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace jpt {

template <typename Result>
class BackgroundJob {
public:
    using Work = std::function<Result(const std::atomic<bool>& cancelled)>;

    struct Done {
        uint64_t serial = 0;
        std::optional<Result> result; // empty when the job failed
        std::string error;
    };

    BackgroundJob() = default;
    BackgroundJob(const BackgroundJob&) = delete;
    BackgroundJob& operator=(const BackgroundJob&) = delete;
    // Asks a running job to stop and waits for it: it never outlives its owner.
    ~BackgroundJob() {
        Cancel();
        if (worker_.joinable()) worker_.join();
    }

    // 0 while an earlier job runs or its result is uncollected.
    uint64_t Start(Work work) {
        if (worker_.joinable() || !work) return 0;
        done_.store(false);
        cancelled_ = std::make_shared<std::atomic<bool>>(false);
        const uint64_t serial = ++serial_;
        worker_ = std::thread([this, work = std::move(work), cancelled = cancelled_] {
            Done done;
            try {
                done.result = work(*cancelled);
            } catch (const std::exception& error) {
                done.error = error.what();
            } catch (...) {
                done.error = "unexpected error";
            }
            done_result_ = std::move(done);
            done_.store(true, std::memory_order_release);
        });
        return serial;
    }

    void Cancel() {
        if (cancelled_) cancelled_->store(true);
    }
    bool busy() const { return worker_.joinable(); }

    // The finished job's result, once.
    std::optional<Done> Collect() {
        if (!worker_.joinable() || !done_.load(std::memory_order_acquire)) return std::nullopt;
        worker_.join();
        Done done = std::move(done_result_);
        done.serial = serial_;
        return done;
    }

private:
    std::thread worker_;
    std::atomic<bool> done_{false};
    std::shared_ptr<std::atomic<bool>> cancelled_;
    Done done_result_;
    uint64_t serial_ = 0;
};

} // namespace jpt
