#pragma once

#include "replay/types.hpp"

#include <cstddef>
#include <vector>

namespace replay::detail {

struct ActiveAction {
    bool moving = false;
    int from_cell = -1;
    int to_cell = -1;
    int remaining = 0;
};

struct RuntimeState {
    WorldState world;
    std::vector<std::size_t> next_action;
    std::vector<ActiveAction> active;
};

struct OperationResult {
    bool ok = true;
    ValidationError error;
};

OperationResult reserveNextAction(
    const DailyInput& input,
    const DailyPlan& plan,
    RuntimeState& runtime,
    std::size_t agent_index,
    int step);

OperationResult transitionOneStep(
    const DailyInput& input,
    RuntimeState& runtime,
    int step,
    std::vector<std::size_t>& completing,
    StepSnapshot& snapshot);

}  // namespace replay::detail
