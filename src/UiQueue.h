#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace granular {
struct UiChange { uint32_t id = 0; double value = 0; int kind = 0; };
// Single main-thread producer, single process/flush consumer. Keep room for a
// final gesture-end so a saturated queue cannot leave automation recording open.
class UiQueue {
public:
    bool push(UiChange change) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto used = head-tail;
        if (used >= capacity-(change.kind == 2 ? 0u : 1u)) return false;
        data_[head%capacity] = change;
        head_.store(head+1,std::memory_order_release);
        return true;
    }
    const UiChange* peek() const noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        return tail == head_.load(std::memory_order_acquire) ? nullptr : &data_[tail%capacity];
    }
    void pop() noexcept { tail_.fetch_add(1,std::memory_order_release); }
private:
    static constexpr uint32_t capacity = 1024;
    std::array<UiChange,capacity> data_ {};
    std::atomic<uint32_t> head_ {0}, tail_ {0};
};
} // namespace granular
