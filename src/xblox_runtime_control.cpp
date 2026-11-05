#include "runtime.hpp"

#include <utility>
#include <vector>

namespace media::xblox {

namespace {
thread_local std::vector<CancelPredicate> g_thread_cancel_overrides;
}

void push_thread_cancel_override(CancelPredicate predicate)
{
    g_thread_cancel_overrides.push_back(std::move(predicate));
}

void pop_thread_cancel_override()
{
    if (!g_thread_cancel_overrides.empty())
        g_thread_cancel_overrides.pop_back();
}

bool thread_cancel_requested()
{
    for (auto it = g_thread_cancel_overrides.rbegin(); it != g_thread_cancel_overrides.rend(); ++it) {
        if (*it && (*it)())
            return true;
    }
    return false;
}

} // namespace media::xblox
