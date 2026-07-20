#include "test_allocation_failure.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace dsn {
namespace rasn {
namespace test_support {
namespace {

thread_local allocation_failure_scope *active_allocation_failure_scope =
    nullptr;

} // namespace
} // namespace test_support
} // namespace rasn
} // namespace dsn

namespace {
void *allocate(std::size_t size)
{
    if (::dsn::rasn::test_support::detail::consume_allocation_failure(size))
    {
        throw std::bad_alloc();
    }

    size = size == 0 ? 1 : size;
    for (;;)
    {
        if (void *memory = std::malloc(size))
        {
            return memory;
        }
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
        {
            throw std::bad_alloc();
        }
        handler();
    }
}

} // namespace

void *operator new(std::size_t size) { return allocate(size); }

void *operator new[](std::size_t size) { return allocate(size); }

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete[](void *memory) noexcept { std::free(memory); }

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept
{
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace dsn {
namespace rasn {
namespace test_support {

allocation_failure_scope::allocation_failure_scope(
    std::size_t minimum_size,
    std::size_t matching_allocations_before_failure) noexcept
    : _previous(active_allocation_failure_scope),
      _minimum_size(minimum_size),
      _matching_allocations_before_failure(
          matching_allocations_before_failure),
      _matching_allocations_observed(0),
      _last_skipped_allocation_size(0),
      _triggered_allocation_size(0),
      _armed(true),
      _triggered(false)
{
    active_allocation_failure_scope = this;
}

allocation_failure_scope::~allocation_failure_scope() noexcept
{
    if (active_allocation_failure_scope != this)
    {
        std::abort();
    }
    _armed = false;
    active_allocation_failure_scope = _previous;
}

bool allocation_failure_scope::armed() const noexcept { return _armed; }

bool allocation_failure_scope::triggered() const noexcept
{
    return _triggered;
}

std::size_t
allocation_failure_scope::matching_allocations_observed() const noexcept
{
    return _matching_allocations_observed;
}

std::size_t
allocation_failure_scope::last_skipped_allocation_size() const noexcept
{
    return _last_skipped_allocation_size;
}

std::size_t
allocation_failure_scope::triggered_allocation_size() const noexcept
{
    return _triggered_allocation_size;
}

bool allocation_failure_scope::current_thread_rule_armed() noexcept
{
    return active_allocation_failure_scope != nullptr &&
           active_allocation_failure_scope->_armed;
}

namespace detail {

bool consume_allocation_failure(std::size_t size) noexcept
{
    allocation_failure_scope *scope = active_allocation_failure_scope;
    if (scope == nullptr || !scope->_armed || size < scope->_minimum_size)
    {
        return false;
    }
    ++scope->_matching_allocations_observed;
    if (scope->_matching_allocations_before_failure > 0)
    {
        scope->_last_skipped_allocation_size = size;
        --scope->_matching_allocations_before_failure;
        return false;
    }

    scope->_armed = false;
    scope->_triggered = true;
    scope->_triggered_allocation_size = size;
    return true;
}

} // namespace detail
} // namespace test_support
} // namespace rasn
} // namespace dsn
