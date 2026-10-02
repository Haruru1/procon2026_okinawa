#pragma once

#include "replay/types.hpp"

namespace replay {

class RuleReplay {
public:
    SimulationResult runDailyPlan(const DailyInput& input, const DailyPlan& plan) const;
};

}  // namespace replay
