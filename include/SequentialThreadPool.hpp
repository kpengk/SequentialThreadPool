#pragma once
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>


enum class TaskReply { done, retry };

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

    template <typename T, typename std::enable_if<std::is_same_v<decltype(std::declval<T>()()), TaskReply>>::type* = nullptr>
    void post(uint32_t group, T&& task) {
        bool waiting{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_token_) {
                throw std::runtime_error("enqueue on stopped thread pool");
            }
            waiting = group_tasks_[group].empty();
            group_tasks_[group].emplace(std::forward<T>(task));
            if (waiting) {
                runnable_group_.push(group);
            }
        }
        if (waiting) {
            condition_.notify_one();
        }
    }

    template <typename T, typename std::enable_if<std::is_same_v<decltype(std::declval<T>()()), void>>::type* = nullptr>
    void post(uint32_t group, T&& task) {
        post(group, [task = std::forward<T>(task)]() {
            task();
            return TaskReply::done;
        });
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
            std::function<TaskReply()> task{};

            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this]() { return !runnable_group_.empty() || stop_token_; });
                if (runnable_group_.empty() && stop_token_) {
                    return;
                }
                group = runnable_group_.front();
                runnable_group_.pop();
                task = group_tasks_[group].front();
            }

            const TaskReply reply = task();

            bool notify{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (reply == TaskReply::done) {
                    group_tasks_[group].pop();
                }
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
    std::unordered_map<uint32_t, std::queue<std::function<TaskReply()>>> group_tasks_;
    std::queue<uint32_t> runnable_group_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_token_;
};
