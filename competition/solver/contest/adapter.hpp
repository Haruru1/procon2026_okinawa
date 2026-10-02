#pragma once
#include "solver.hpp"
#include <optional>

namespace contest_solver::contest {
constexpr double solver_seconds = 55.0;
constexpr double finalization_seconds = 1.0;
constexpr const char *history_format = "c_staged.local_history.v1";

struct MapDocument {
    Json raw;
    int width = 0, height = 0, capacity = 0, players = 0;
    int64_t starts_at = 0;
    std::vector<int> terrain, initial_positions, day_steps;
    std::vector<int64_t> day_seconds, brands;
};

struct Input {
    MapDocument map;
    Json day, normalized;
    int day_index = 0;
    int64_t ends_at = 0;
};

struct Output {
    Json answer, report;
};

Json read_json(const std::string &path);
MapDocument parse_map(const Json &document);
void validate_contest_map(const MapDocument &map);
void validate_day_shape(const Json &document, size_t agents);
Json validate_kinds(const Json &document, size_t agents);
Input convert(const MapDocument &map, const Json &day,
              const std::optional<Json> &history, bool allow_synthetic = false);
DailyPlan parse_answer(const Json &answer, const Case &c);
Output validate_answer(const Input &input, const Json &answer, bool fallback = false);
Output solve(const Input &input, bool offline = false, int64_t test_check_budget = -1,
             double budget_seconds = solver_seconds);
} // namespace contest_solver::contest
