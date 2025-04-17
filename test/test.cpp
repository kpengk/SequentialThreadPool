#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "SequentialThreadPool.hpp"
#include "doctest/doctest.h"

#include <chrono>

std::uint64_t high_precision_sleep(int microsec) {
    const auto start = std::chrono::high_resolution_clock::now();
    while (true) {
        const auto now = std::chrono::high_resolution_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        if (elapsed >= microsec) {
            return elapsed;
        }
    }
    return 0;
}

void ret_void_fun() {}

TaskReply ret_reply_fun() {
    return TaskReply{};
}

TEST_CASE("test_supported_return_type") {
    SequentialThreadPool pool(1);
    pool.post(0, ret_void_fun);
    pool.post(0, ret_reply_fun);
    pool.post(0, std::bind(ret_void_fun));
    pool.post(0, std::bind(ret_reply_fun));
    pool.post(0, std::function<void()>(ret_void_fun));
    pool.post(0, std::function<void()>(ret_reply_fun));
    pool.post(0, []() {});
    pool.post(0, []() -> TaskReply { return TaskReply::done; });

    int val{};
    pool.post(0, [val]() mutable { ++val; });
}

TEST_CASE("test_retry_parameter_passing") {
    std::vector<int> vec{1, 2, 3, 4, 5, 6, 7, 8};
    const int* ptr = vec.data();

    SequentialThreadPool pool(1);
    pool.post(0, [obj = std::move(vec), ptr]() mutable {
        static int count{};
        static std::vector<int> temp;
        ++count;

        if (count <= 3) {
            CHECK(obj.data() == ptr);
            CHECK(obj.size() == 8);
            return TaskReply::retry;
        } else if (count == 4) {
            CHECK(obj.data() == ptr);
            CHECK(obj.size() == 8);
            temp = std::move(obj);
            CHECK(obj.data() == nullptr);
            CHECK(obj.size() == 0);
            return TaskReply::retry;
        } else {
            CHECK(obj.data() == nullptr);
            CHECK(obj.size() == 0);
            return TaskReply::done;
        }
    });
}

TEST_CASE("test_partial_task_retry") {
    SequentialThreadPool pool(2);
    int count0{};
    pool.post(0, [&]() {
        high_precision_sleep(100);
        return ++count0 < 5 ? TaskReply::retry : TaskReply::done;
    });
    for (int i = 0; i < 10; ++i) {
        pool.post(0, [&]() {
            high_precision_sleep(200);
            return TaskReply::done;
        });
    }
    pool.wait_for_done();
}

TEST_CASE("test_all_task_retry") {
    SequentialThreadPool pool(4);

    std::mutex mtx0;
    int count0{};
    std::vector<int> task0_exec;
    task0_exec.reserve(2000);
    pool.post(0, [&]() {
        high_precision_sleep(100);
        std::lock_guard<std::mutex> lk(mtx0);
        task0_exec.push_back(count0);
        return ++count0 < 2000 ? TaskReply::retry : TaskReply::done;
    });

    std::mutex mtx1;
    int count1{};
    std::vector<int> task1_exec;
    task1_exec.reserve(1000);
    pool.post(1, [&]() {
        high_precision_sleep(200);
        std::lock_guard<std::mutex> lk(mtx1);
        task1_exec.push_back(count1);
        return ++count1 < 1000 ? TaskReply::retry : TaskReply::done;
    });

    std::mutex mtx2;
    int count2{};
    std::vector<int> task2_exec;
    task2_exec.reserve(5000);
    for (int i = 0; i < 5000; ++i) {
        pool.post(2, [&]() {
            high_precision_sleep(10);
            std::lock_guard<std::mutex> lk(mtx2);
            task2_exec.push_back(count2++);
        });
    }
    pool.wait_for_done();

    CHECK(task0_exec.size() == 2000);
    for (int i = 0; i < task0_exec.size(); ++i) {
        CHECK(task0_exec[i] == i);
    }

    CHECK(task1_exec.size() == 1000);
    for (int i = 0; i < task1_exec.size(); ++i) {
        CHECK(task1_exec[i] == i);
    }

    CHECK(task2_exec.size() == 5000);
    for (int i = 0; i < task2_exec.size(); ++i) {
        CHECK(task2_exec[i] == i);
    }
}

TEST_CASE("test_execution_order") {
    constexpr int thread_count = 4;
    constexpr int group_count = 10;
    constexpr int group_task_count = 2000;
    // Execution result task number
    std::vector<std::vector<int>> executed_tasks(group_count);
    for (auto& tasks : executed_tasks) {
        tasks.reserve(group_task_count);
    }

    // Add tasks in groups
    std::mutex mtx;
    SequentialThreadPool pool(thread_count);
    for (int task = 0; task < group_count * group_task_count; ++task) {
        const int group = task % group_count;
        pool.post(group, [group, task, &executed_tasks, &mtx](TaskContext* ctx) {
            const int sleep = task == 0 ? 200 : rand() % 100;
            high_precision_sleep(sleep);
            ctx->wait_previous();
            std::lock_guard<std::mutex> lock(mtx);
            executed_tasks[group].push_back(task);
        });
    }

    pool.wait_for_done();

    // Check the order of task execution within the group
    for (int group = 0; group < group_count; ++group) {
        const auto& tasks = executed_tasks[group];
        CHECK(tasks.size() == group_task_count);
        for (int i = 0; i < group_task_count; ++i) {
            CHECK(tasks[i] == group_count * i + group);
        }
    }
}

TEST_CASE("test_execution_time") {
    constexpr int thread_count = 4;
    constexpr int group_count = 2;
    constexpr int group_task_count = 5000;
    // Execution result task number
    std::atomic_uint64_t value{0};

    // Add tasks in groups
    SequentialThreadPool pool(thread_count);
    const auto start = std::chrono::high_resolution_clock::now();
    for (int task = 0; task < group_count * group_task_count; ++task) {
        const int remains = task % (group_count + 1);
        const int group = remains >= group_count ? 0 : remains;
        pool.post(group, [&value](TaskContext* ctx) {
            const int sleep = 1000 + rand() % 200;
            const auto real = high_precision_sleep(sleep);
            ctx->wait_previous();
            value.fetch_add(real, std::memory_order_release);
        });
    }

    pool.wait_for_done();
    const auto now = std::chrono::high_resolution_clock::now();
    const int64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    const float task_time = value.load(std::memory_order_acquire) / 1000.0F;
    std::cout << "Execution time:" << elapsed << "ms, task time sum: " << task_time << "ms.\n";
}

TEST_CASE("test_scheduling_time") {
    constexpr int thread_count = 4;
    constexpr int group_count = 50;
    constexpr int group_task_count = 20000;
    // Execution result task number.
    std::atomic_uint64_t value{0};

    // Add tasks in groups.
    SequentialThreadPool pool(thread_count);
    const auto start = std::chrono::high_resolution_clock::now();
    for (int task = 0; task < group_count * group_task_count; ++task) {
        const int remains = task % group_count;
        const int group = remains >= group_count ? 0 : remains;
        pool.post(group, [&value](TaskContext* ctx) {
            ctx->wait_previous();
            value.fetch_add(1, std::memory_order_release);
        });
    }

    pool.wait_for_done();
    const auto now = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    std::cout << "Scheduling time:" << elapsed << "ms, task count: " << value.load(std::memory_order_acquire) << ".\n";
}
