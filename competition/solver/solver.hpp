#pragma once
#include "supply.hpp"

namespace contest_solver {
struct SolveResult {
    DailyPlan plan;
    Objective objective{};
    SimulationResult replay;
    std::vector<int> selection;
    std::vector<Course> selected;
    std::vector<std::vector<Event>> supply_chains;
    bool completed = false;
    uint64_t nodes = 0, bound_prunes = 0, early_prunes = 0, individual_removed = 0, merges = 0,
             dominance = 0, updates = 0;
    Json metrics;
    Json improvements = Json::array();
};
SolveResult solve_day(const Case &c, int limit, const Deadline &deadline, bool detailed = false,
                      bool interned = true, bool indexed = true, GhrccOptions options = {});
SolveResult solve_team(const Case &c, const Travel &travel, const std::vector<Pool> &pools,
                       const Deadline &deadline, bool detailed = false, bool interned = true,
                       bool indexed = true, GhrccOptions options = {});
Json result_json(const SolveResult &result);
} // namespace contest_solver
