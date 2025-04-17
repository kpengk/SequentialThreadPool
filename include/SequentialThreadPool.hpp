#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>


enum class TaskReply { done, retry };
class SequentialThreadPool;

// @brief TaskContext is used to pass the task group and id to the task.
class TaskContext {
public:
    TaskContext(SequentialThreadPool* pool, uint32_t group, uint32_t id);
    ~TaskContext();
    TaskContext(const TaskContext&) = delete;
    TaskContext(TaskContext&&) = delete;
    TaskContext& operator=(const TaskContext&) = delete;
    TaskContext& operator=(TaskContext&&) = delete;
    uint32_t group() const;
    uint32_t id() const;
    void wait_previous();

private:
    SequentialThreadPool* const pool_;
    const uint32_t group_;
    const uint32_t id_;
    bool waited_;
};

class SequentialThreadPool {
public:
    explicit SequentialThreadPool(size_t threads) : task_id_{}, stop_token_{} {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this]() { run(); });
        }
    }

    ~SequentialThreadPool() {
        wait_for_done();
    }

    SequentialThreadPool(const SequentialThreadPool&) = delete;
    SequentialThreadPool(SequentialThreadPool&&) = delete;
    SequentialThreadPool& operator=(const SequentialThreadPool&) = delete;
    SequentialThreadPool& operator=(SequentialThreadPool&&) = delete;

    // @brief Task param TaskContext*, task return TaskReply.
    template <typename T,
        typename std::enable_if<std::is_same<decltype(std::declval<T>()({})), TaskReply>::value>::type* = nullptr>
    void post(uint32_t group, T&& task) {
        bool waiting{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_token_) {
                throw std::runtime_error("enqueue on stopped thread pool");
            }
            waiting = group_tasks_[group].empty();
            group_tasks_[group].emplace_back(Task{task_id_, false, std::forward<T>(task)});
            ++task_id_;
            if (waiting) {
                ready_group_.push(group);
            }
        }
        if (waiting) {
            condition_.notify_one();
        }
    }

    // @brief Task param void, task return TaskReply.
    template <typename T,
        typename std::enable_if<std::is_same<decltype(std::declval<T>()()), TaskReply>::value>::type* = nullptr>
    void post(uint32_t group, T&& task) {
        post(group, [task = std::forward<T>(task)](TaskContext*) mutable -> TaskReply { return task(); });
    }

    // @brief Task param TaskContext*, task return void.
    template <typename T,
        typename std::enable_if<std::is_same<decltype(std::declval<T>()({})), void>::value>::type* = nullptr>
    void post(uint32_t group, T&& task) {
        post(group, [task = std::forward<T>(task)](TaskContext* ctx) mutable -> TaskReply {
            task(ctx);
            return TaskReply::done;
        });
    }

    // @brief Task param void, task return void.
    template <typename T,
        typename std::enable_if<std::is_same<decltype(std::declval<T>()()), void>::value>::type* = nullptr>
    void post(uint32_t group, T&& task) {
        post(group, [task = std::forward<T>(task)](TaskContext*) mutable -> TaskReply {
            task();
            return TaskReply::done;
        });
    }

    size_t task_size(uint32_t group) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iter = group_tasks_.find(group);
        return iter != group_tasks_.cend() ? iter->second.size() : 0;
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
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() { return !ready_group_.empty() || stop_token_; });
            if (ready_group_.empty() && stop_token_) {
                break;
            }
            const uint32_t group = ready_group_.front();
            ready_group_.pop();
            const auto task_iter = std::find_if(
                group_tasks_[group].begin(), group_tasks_[group].end(), [](const Task& task) { return !task.running; });
            task_iter->running = true;
            if (task_iter + 1 != group_tasks_[group].cend()) {
                ready_group_.push(group);
                lock.unlock();
                condition_.notify_one();
            } else {
                lock.unlock();
            }

            TaskContext ctx(this, group, task_iter->id);
            const TaskReply reply = task_iter->f(&ctx);

            if (reply == TaskReply::done) {
                ctx.wait_previous();
                lock.lock();
                group_tasks_[group].pop_front();
                lock.unlock();
                condition_.notify_all();
            } else {
                lock.lock();
                task_iter->running = false;
                ready_group_.push(group);
                lock.unlock();
            }
        }
    }

    void wait_previous(uint32_t group, uint32_t task) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() {
            if (group_tasks_[group].empty()) {
                return true;
            }
            const uint32_t val = group_tasks_[group].front().id;
            if (val != task) {
                return false;
            }
            return group_tasks_[group].front().id == task;
        });
    }

    friend class TaskContext;
    struct Task {
        uint32_t id{};
        bool running{};
        std::function<TaskReply(TaskContext*)> f;
    };

    std::vector<std::thread> workers_;
    std::unordered_map<uint32_t, std::deque<Task>> group_tasks_;
    std::queue<uint32_t> ready_group_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::uint32_t task_id_;
    bool stop_token_;
};


inline TaskContext::TaskContext(SequentialThreadPool* pool, uint32_t group, uint32_t id)
    : pool_{pool}, group_{group}, id_{id}, waited_{false} {}

inline TaskContext::~TaskContext() {
    wait_previous();
}

inline uint32_t TaskContext::group() const {
    return group_;
}

inline uint32_t TaskContext::id() const {
    return id_;
}

inline void TaskContext::wait_previous() {
    if (!waited_) {
        pool_->wait_previous(group_, id_);
    }
    waited_ = true;
}
