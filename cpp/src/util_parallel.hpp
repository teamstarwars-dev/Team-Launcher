#pragma once

// mini pool de threads pour telechargements paralleles (libs x8, assets x16)

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tl {

// Execute body(0..total-1) sur au plus maxWorkers threads.
// La premiere exception lancee par body est repropagee apres join (comme Task.WhenAll).
// cancel interrompt la distribution des indices restants.
inline void parallel_for(int total, int maxWorkers,
                         const std::function<void(int)>& body,
                         const std::atomic<bool>* cancel = nullptr) {
    if (total <= 0) return;
    int workers = maxWorkers < total ? maxWorkers : total;
    if (workers < 1) workers = 1;

    std::atomic<int> next{0};
    std::atomic<bool> hasError{false};
    std::exception_ptr firstError;
    std::mutex errM;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([&] {
            for (;;) {
                if (cancel && cancel->load(std::memory_order_relaxed)) return;
                if (hasError.load(std::memory_order_relaxed)) return;
                const int i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= total) return;
                try {
                    body(i);
                } catch (...) {
                    hasError.store(true, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> g(errM);
                    if (!firstError) firstError = std::current_exception();
                    return;
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    if (firstError) std::rethrow_exception(firstError);
}

} // namespace tl
