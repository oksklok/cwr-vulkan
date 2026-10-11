#pragma once
#include <atomic>
#include <cstdint>

namespace Poseidon::render
{
// Process-local lifetime identity. Never serialized, never copied from another
// instance, and never derived from a recyclable address or a mission object ID.
class RenderIdentity
{
    inline static std::atomic<uint64_t> next{1};
    uint64_t value = next.fetch_add(1, std::memory_order_relaxed);

  public:
    RenderIdentity() = default;
    RenderIdentity(const RenderIdentity&) {}
    RenderIdentity& operator=(const RenderIdentity&)
    {
        value = next.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }
    uint64_t Get() const { return value; }
};
} // namespace Poseidon::render
