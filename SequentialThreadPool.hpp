#pragma once
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

class SequentialThreadPool {
public:
    explicit SequentialThreadPool(size_t threads) : stop_token_{} {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this]() { run(); });
        }
    }

    ~SequentialThreadPool() {
        wait_for_done();
    }

    template <typename F, typename... Args>
#if __cplusplus < 201703
    auto enqueue(uint32_t group, F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        using result_type = typename std::result_of<F(Args...)>::type;
#else
    auto enqueue(uint32_t group, F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using result_type = std::invoke_result_t<F, Args...>;
#endif
        auto task = std::make_shared<std::packaged_task<result_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<result_type> res = task->get_future();

        bool waiting{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_token_) {
                throw std::runtime_error("enqueue on stopped thread pool");
            }
            waiting = group_tasks_[group].empty();
            group_tasks_[group].emplace([task]() { (*task)(); });
            if (waiting) {
                runnable_group_.push(group);
            }
        }
        if (waiting) {
            condition_.notify_one();
        }
        return res;
    }

    size_t task_size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count{};
        for (const auto& item : group_tasks_) {
            count += item.second.size();
        }
        return count;
    }

    void wait_for_done() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_token_ = true;
        }
        condition_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable())
                worker.join();
        }
    }

private:
    void run() {
        while (true) {
            uint32_t group{};
            std::function<void()> task{};

            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() { return !runnable_group_.empty() || stop_token_; });
                if (runnable_group_.empty() && stop_token_) {
                    return;
                }
                group = runnable_group_.front();
                runnable_group_.pop();
                task = std::move(group_tasks_[group].front());
            }

            task();

            bool notify{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                group_tasks_[group].pop();
                if (!group_tasks_[group].empty()) {
                    runnable_group_.push(group);
                    notify = true;
                }
            }
            if (notify) {
                condition_.notify_one();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::unordered_map<uint32_t, std::queue<std::function<void()>>> group_tasks_;
    std::queue<uint32_t> runnable_group_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_token_;
};
