// Confirms a particular lighting request only after its device write succeeded.
#pragma once
#include <atomic>
#include <cstdint>
namespace luma {
class DeliveryReceipt {
public:
    void Confirm(uint64_t token) {
        uint64_t old = value_.load();
        while (old < token && !value_.compare_exchange_weak(old, token)) {}
    }
    uint64_t value() const { return value_.load(); }
    bool Has(uint64_t token) const { return token != 0 && value() >= token; }
private: std::atomic<uint64_t> value_{0};
};
}  // namespace luma
