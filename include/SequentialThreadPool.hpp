#pragma once
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace detail {
    template <typename Container, typename UnaryPredicate>
    auto find_if_from(Container& container, typename Container::iterator start, UnaryPredicate pred) {
        // [start, end)
        auto it = std::find_if(start, container.end(), pred);
        if (it != container.end()) {
            return it;
        }

        // [begin, start)
        it = std::find_if(container.begin(), start, pred);
        return it == start ? container.end() : it;
    }
} // namespace detail

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
    explicit SequentialThreadPool(size_t threads) : wait_task_count_{}, task_id_{}, stop_token_{} {
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
        bool waiting{}; // Waiting for new tasks.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_token_) {
                throw std::runtime_error("enqueue on stopped thread pool");
            }
            if (group_tasks_.find(group) == group_tasks_.end()) {
                const auto res = group_tasks_.emplace(group, std::deque<Task>());
                next_exec_group_ = res.first;
            }
            group_tasks_[group].emplace_back(Task{false, task_id_, std::forward<T>(task)});
            waiting = wait_task_count_ == 0;
            ++task_id_;
            ++wait_task_count_;
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
            std::lock_guard<std::mutex> lock(mutex_);
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
            condition_.wait(lock, [this]() { return wait_task_count_ != 0 || stop_token_; });
            if (wait_task_count_ == 0 && stop_token_) {
                break;
            }
            // Pull tasks from task group.
            std::pair<uint32_t, Task&> task_info = find_ready_task();
            const uint32_t& group = task_info.first;
            Task& task = task_info.second;
            task.runing = true;
            --wait_task_count_;
            lock.unlock();
            // Execute the task.
            TaskContext ctx(this, group, task.id);
            const TaskReply reply = task.f(&ctx);
            // Check if the result needs to be retried.
            if (reply == TaskReply::done) {
                ctx.wait_previous();
                lock.lock();
                group_tasks_[group].pop_front();
                lock.unlock();
                // Wake up the next task in the group that may be waiting.
                // Notify all threads because multiple tasks in a task group may be waiting,
                // and waking up only one task cannot determine the wakeup order.
                condition_.notify_all();
            } else {
                lock.lock();
                task.runing = false;
                ++wait_task_count_;
                lock.unlock();
                // No "notify_one()" is needed here, because the current thread
                // will immediately check whether "ready_group_" is empty.
            }
        }
    }

    void wait_previous(uint32_t group, uint32_t task) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() {
            if (group_tasks_[group].empty()) {
                return true;
            }
            return group_tasks_[group].front().id == task;
        });
    }

    friend class TaskContext;

    struct Task {
        bool runing{};
        uint32_t id{};
        std::function<TaskReply(TaskContext*)> f;
    };

    std::pair<uint32_t, Task&> find_ready_task() {
        // Get the task at the head of the queue to avoid waiting for the previous task to complete.
        const auto ready_group = detail::find_if_from(group_tasks_, next_exec_group_,
            [](const auto& item) { return !item.second.empty() && !item.second.front().runing; });
        if (ready_group != group_tasks_.end()) {
            next_exec_group_ = std::next(ready_group);
            return {ready_group->first, ready_group->second.front()};
        }
        // Get a task that has not running.
        while (true) {
            if (next_exec_group_ == group_tasks_.end()) {
                next_exec_group_ = group_tasks_.begin();
            }
            const auto task_iter = std::find_if(next_exec_group_->second.begin(), next_exec_group_->second.end(),
                [](const auto& item) { return !item.runing; });
            if (task_iter != next_exec_group_->second.end()) {
                const uint32_t group = next_exec_group_->first;
                ++next_exec_group_;
                return {group, *task_iter};
            }
            ++next_exec_group_;
        }
    }

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<uint32_t, std::deque<Task>> group_tasks_;
    std::unordered_map<uint32_t, std::deque<Task>>::iterator next_exec_group_;
    std::uint32_t wait_task_count_;
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
        waited_ = true;
    }
}
