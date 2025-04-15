#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "SequentialThreadPool.hpp"
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

void ret_void_fun(){}

TaskReply ret_reply_fun() {
    return TaskReply{};
}

TEST_CASE("Testing supported return type") {
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

TEST_CASE("Testing retry parameter passing") {
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
            CHECK(obj.data()  == ptr);
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

TEST_CASE("Testing retry") {
    SequentialThreadPool pool(2);
    int count0{};
    int count1{};
    int count2{};
    std::vector<int> task0_exec;
    std::vector<int> task1_exec;
    std::vector<int> task2_exec;
    pool.post(0, [&]() {
        task0_exec.push_back(count0);
        high_precision_sleep(100);
        return ++count0 < 20 ? TaskReply::retry : TaskReply::done;
    });
    pool.post(1, [&]() {
        task1_exec.push_back(count1);
        high_precision_sleep(200);
        return ++count1 < 10 ? TaskReply::retry : TaskReply::done;
    });
    for (int i = 0; i < 1000; ++i) {
        pool.post(2, [&]() {
            task2_exec.push_back(count2++);
            high_precision_sleep(10);
        });
    }
    pool.wait_for_done();

    CHECK(task0_exec.size() == 20);
    for (int i = 0; i < task0_exec.size(); ++i) {
        CHECK(task0_exec[i] == i);
    }

    CHECK(task1_exec.size() == 10);
    for (int i = 0; i < task1_exec.size(); ++i) {
        CHECK(task1_exec[i] == i);
    }

    CHECK(task2_exec.size() == 1000);
    for (int i = 0; i < task2_exec.size(); ++i) {
        CHECK(task2_exec[i] == i);
    }
}

TEST_CASE("test execution order") {
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
        pool.post(group, [group, task, &executed_tasks, &mtx]() {
            const int sleep = task == 0 ? 200 : rand() % 100;
            high_precision_sleep(sleep);
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

TEST_CASE("test execution time") {
    constexpr int thread_count = 2;
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
        pool.post(group, [&value]() {
            const int sleep = 1000 + rand() % 200;
            const auto real = high_precision_sleep(sleep);
            value.fetch_add(real, std::memory_order_release);
        });
    }

    pool.wait_for_done();
    const auto now = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    std::cout << "time:" << elapsed << "ms, task time sum: " << value.load(std::memory_order_acquire) / 1000.0F << "ms\n";
}
