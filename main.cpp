#include "SequentialThreadPool.hpp"

#include <iostream>
#include <sstream>


int main() {
    const auto start = std::chrono::high_resolution_clock::now();

    SequentialThreadPool pool(4);
    for (int task = 0; task < 30; ++task) {
        const int group = task % 5; // Task Grouping
        pool.enqueue(group, [=]() {
            const auto end = std::chrono::high_resolution_clock::now();
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            const int sleep = task == 0 ? 200 : rand() % 100 + 50;
            std::stringstream ss;
            ss << "Time:" << duration << ", group:" << group << ", task:" << task
               << ", tid:" << std::this_thread::get_id() << ", sleep:" << sleep << "ms\n";
            std::cout << ss.str();
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
        });
    }

    return 0;
}
