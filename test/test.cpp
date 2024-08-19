#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
#include "SequentialThreadPool.hpp"

void high_precision_sleep(int microsec) {
    const auto start = std::chrono::high_resolution_clock::now();
    while (true) {
        const auto now = std::chrono::high_resolution_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        if (elapsed >= microsec) {
            break;
        }
    }
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
