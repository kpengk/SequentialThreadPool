#include "SequentialThreadPool.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <chrono>

void print_group(int count) {
    for (int col = 0; col < count; ++col) {
        std::cout << std::right << std::setw(4) << static_cast<char>('A' + col);
    }
    std::cout << std::endl;
}

int add(int) {
    return 1;
}

void print_task(int group, int task) {
    const auto task_str = std::to_string(task);
    const size_t space_count = 4 * (group + 1) - task_str.size();
    std::string str;
    str.reserve(space_count + task_str.size() + 1);
    str.resize(space_count, ' ');
    str.append(task_str);
    str.push_back('\n');
    std::cout << str;
}

int main() {
    print_group(5);
    const auto start = std::chrono::high_resolution_clock::now();

    SequentialThreadPool pool(4);
    for (int task = 0; task < 30; ++task) {
        const int group = task % 5; // Task Grouping
        pool.post(group, [=]() {
            const int sleep = task == 0 ? 200 : rand() % 100 + 50;
#if 1
            print_task(group, task);
#else
            const auto end = std::chrono::high_resolution_clock::now();
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            std::stringstream ss;
            ss << "Time:" << duration << ", group:" << group << ", task:" << task
               << ", tid:" << std::this_thread::get_id() << ", sleep:" << sleep << "ms\n";
            std::cout << ss.str();
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
        });
    }

    return 0;
}
