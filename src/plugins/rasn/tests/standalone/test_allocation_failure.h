#pragma once

#include <cstddef>

namespace dsn {
namespace rasn {
namespace test_support {

namespace detail {
bool consume_allocation_failure(std::size_t size) noexcept;
}

// Arms one allocation-failure rule on the calling thread. Nested scopes
// temporarily suspend and then restore the outer rule; destruction out of stack
// order fails fast. A fired rule auto-disarms before throwing.
class allocation_failure_scope
{
public:
    allocation_failure_scope(
        std::size_t minimum_size,
        std::size_t matching_allocations_before_failure = 0) noexcept;
    ~allocation_failure_scope() noexcept;

    allocation_failure_scope(const allocation_failure_scope &) = delete;
    allocation_failure_scope &
    operator=(const allocation_failure_scope &) = delete;
    allocation_failure_scope(allocation_failure_scope &&) = delete;
    allocation_failure_scope &operator=(allocation_failure_scope &&) = delete;

    bool armed() const noexcept;
    bool triggered() const noexcept;
    std::size_t matching_allocations_observed() const noexcept;
    std::size_t last_skipped_allocation_size() const noexcept;
    std::size_t triggered_allocation_size() const noexcept;

    static bool current_thread_rule_armed() noexcept;

private:
    friend bool detail::consume_allocation_failure(std::size_t size) noexcept;

    allocation_failure_scope *_previous;
    std::size_t _minimum_size;
    std::size_t _matching_allocations_before_failure;
    std::size_t _matching_allocations_observed;
    std::size_t _last_skipped_allocation_size;
    std::size_t _triggered_allocation_size;
    bool _armed;
    bool _triggered;
};

} // namespace test_support
} // namespace rasn
} // namespace dsn
