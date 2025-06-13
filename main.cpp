#include <iostream>
#include <vector>
#include <thread>
#include <future>
#include <random>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>

class ThreadPool {
public:
    ThreadPool(size_t threads);
    ~ThreadPool();

    void submit(std::function<void()> task);
    void wait_for_all();

private:
    struct TaskQueue {
        std::deque < std::function<void()>> tasks;
        std::mutex m;
    };

    std::vector<std::thread> workers;
    std::vector <std::shared_ptr<TaskQueue>> queues;
    std::atomic<bool> stop {false};

    void worker_thread(int index);
};

ThreadPool::ThreadPool(size_t threads) {
    for (size_t i = 0; i < threads; ++i) {
        queues.push_back(std::make_shared<TaskQueue>());
    }

    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back([this, i] { worker_thread(i); });
    }
}

void ThreadPool::submit(std::function<void()> task) {
    static thread_local size_t index = rand() % queues.size();
    auto& q = queues[index];
    {
        std::lock_guard<std::mutex> lock(q->m);
        q->tasks.push_front(std::move(task));
    }
}

void ThreadPool::worker_thread(int index) {
    auto& my_queue = queues[index];
    while (!stop) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(my_queue->m);
            if (!my_queue->tasks.empty()) {
                task = std::move(my_queue->tasks.front());
                my_queue->tasks.pop_front();
            }
            else {
                lock.unlock();
                for (size_t i = 0; i < queues.size(); ++i) {
                    size_t victim = (index + i + 1) % queues.size();
                    auto& q = queues[victim];
                    std::lock_guard<std::mutex> lock_v(q->m);
                    if (!q->tasks.empty()) {
                        task = std::move(q->tasks.back());
                        q->tasks.pop_back();
                        break;
                    }
                }
                if (!task) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
                }
            }
        }
        task();
    }
}

void ThreadPool::wait_for_all() {
    while (true) {
        bool all_empty = true;
        for (auto& q : queues) {
            std::lock_guard<std::mutex> lock(q->m);
            if (!q->tasks.empty()) {
                all_empty = false;
                break;
            }
        }
        if (all_empty) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

ThreadPool::~ThreadPool() {
    stop = true;
    for (auto& thread : workers) {
        if (thread.joinable())
            thread.join();
    }
}

void parallel_quicksort(int* arr, long left, long right,
    ThreadPool& pool,
    std::shared_ptr <std::promise<void>> done_promise,
    std::shared_ptr <std::atomic<int>> counter) {
    if (left >= right) {
        if (--(*counter) == 0)
            done_promise->set_value();
        return;
    }

    long i = left, j = right;
    int pivot = arr[(left + right) / 2];

    while (i <= j) {
        while (arr[i]<pivot) ++i;
        while (arr[j]>pivot) --j;
        if (i <= j) {
            std::swap(arr[i], arr[j]);
            ++i; --j;
        }
    }

    auto try_spawn = [&](long l, long r) {
        if (l < r) {
            counter->fetch_add(1);
            pool.submit([=, &pool]() {
                parallel_quicksort(arr, l, r, pool, done_promise, counter);
                });
        }
        };

    if ((j - left) > 100000) {
        try_spawn(left, j);
    }
    else {
        parallel_quicksort(arr, left, j, pool, done_promise, counter);
    }

    if ((right - i) > 100000) {
        try_spawn(i, right);
    }
    else {
        parallel_quicksort(arr, i, right, pool, done_promise, counter);
    }
    if (--(*counter) == 0)
        done_promise->set_value();
}

int main() {
    const long size = 10000000;
    int* data = new int[size];
    for (long i = 0; i < size; ++i)
        data[i] = rand();

    ThreadPool pool(std::thread::hardware_concurrency());

    auto start = std::chrono::high_resolution_clock::now();
    {
        auto done_promise = std::make_shared < std::promise <void>>();
        auto counter = std::make_shared < std::atomic <int>>(1);
        pool.submit([=, &pool]() {
            parallel_quicksort(data, 0, size - 1, pool, done_promise, counter);
            });
        done_promise->get_future().wait();
        pool.wait_for_all();
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Multithreaded quicksort time: "
              << std::chrono::duration<double>(end - start).count()
              << " seconds\n";

    for (long i = 0; i < size - 1; ++i) {
        if (data[i] > data[i + 1]) {
            std::cout << "Unsorted!\n";
            break;
        }
    }

    for (long i = 0; i < size; ++i)
        data[i] = rand();

    start = std::chrono::high_resolution_clock::now(); 
    std::sort(data, data + size);
    end = std::chrono::high_resolution_clock::now(); 
    std::cout << "Single-threaded sort time: "
        << std::chrono::duration<double>(end - start).count()
        << " seconds\n";

    delete[] data;
    return 0;
}