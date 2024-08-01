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


TEST_CASE("Testing return values") {
    SequentialThreadPool pool(1);
    const int a = rand();
    const int b = rand();
    std::future<int> res = pool.enqueue(0, [&]() { return a + b; });
    CHECK(res.get() == a + b);
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
        pool.enqueue(group, [group, task, &executed_tasks, &mtx]() {
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
