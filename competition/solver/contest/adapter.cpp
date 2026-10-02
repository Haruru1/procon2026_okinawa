#include "adapter.hpp"
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>

namespace contest_solver::contest {
namespace {
[[noreturn]] void fail(const std::string &path, const std::string &message) {
    throw std::invalid_argument(path + ": " + message);
}

void object(const Json &j, const std::string &path,
            std::initializer_list<const char *> keys) {
    if (!j.is_object())
        fail(path, "expected object");
    std::set<std::string> allowed;
    for (const auto *key : keys) {
        allowed.insert(key);
        if (!j.contains(key))
            fail(path + "." + key, "required field missing");
    }
    for (auto i = j.begin(); i != j.end(); ++i)
        if (!allowed.count(i.key()))
            fail(path + "." + i.key(), "field not in supported PDF schema");
}

void array(const Json &j, const std::string &path, size_t low, size_t high) {
    if (!j.is_array() || j.size() < low || j.size() > high)
        fail(path, "expected array with " + std::to_string(low) + ".." +
                       std::to_string(high) + " elements");
}

int64_t integer(const Json &j, const std::string &path, int64_t low, int64_t high) {
    if (!j.is_number_integer())
        fail(path, "expected integer (not bool, string or floating point)");
    if (j.is_number_unsigned() && j.get<uint64_t>() > uint64_t(INT64_MAX))
        fail(path, "integer exceeds signed 64-bit range");
    const int64_t value = j.get<int64_t>();
    if (value < low || value > high)
        fail(path, "integer outside [" + std::to_string(low) + "," +
                       std::to_string(high) + "]");
    return value;
}

std::string index_path(const std::string &base, size_t i) {
    return base + "[" + std::to_string(i) + "]";
}

std::vector<int64_t> integers(const Json &j, const std::string &path, size_t low_count,
                              size_t high_count, int64_t low, int64_t high) {
    array(j, path, low_count, high_count);
    std::vector<int64_t> out;
    for (size_t i = 0; i < j.size(); ++i)
        out.push_back(integer(j[i], index_path(path, i), low, high));
    return out;
}

void agent_shapes(const Json &agents, const std::string &path, size_t count) {
    array(agents, path, count, count);
    for (size_t i = 0; i < agents.size(); ++i) {
        const auto p = index_path(path, i);
        object(agents[i], p, {"kind", "pos", "fuel"});
        integer(agents[i]["kind"], p + ".kind", 0, 1);
        integer(agents[i]["pos"], p + ".pos", 0, 1023);
        integer(agents[i]["fuel"], p + ".fuel", 0, INT_MAX);
    }
}

void agent_physics(const MapDocument &map, const Json &agents, const std::string &path) {
    for (size_t i = 0; i < agents.size(); ++i) {
        int pos = agents[i]["pos"], kind = agents[i]["kind"], fuel = agents[i]["fuel"];
        if (pos >= int(map.terrain.size()) || map.terrain[pos] == 3)
            fail(index_path(path, i) + ".pos", "agent must occupy a traversable cell");
        if (kind == 0 && fuel > map.capacity)
            fail(index_path(path, i) + ".fuel", "patrol fuel exceeds fuelLimits");
    }
}

std::vector<std::vector<int>> history_sets(const MapDocument &map, const Json &day,
                                          const std::optional<Json> &history) {
    const size_t n = map.initial_positions.size();
    const int d = day["day"];
    std::vector<std::vector<int>> result(n);
    if (!history) {
        if (d != 0)
            fail("history", "required for day > 0: PDF daily JSON has no past acquired brands");
        return result;
    }
    const auto &h = *history;
    object(h, "history", {"format", "map_config", "completed_days", "confirmed",
                           "agent_kinds", "collected_brands"});
    if (h["format"] != history_format)
        fail("history.format", "unsupported local history format");
    if (!h["confirmed"].is_boolean() || h["confirmed"] != true)
        fail("history.confirmed", "must be true only for confirmed past results, not drafts");
    // Full structured equality, not a collision-prone hash or file identity.
    if (h["map_config"] != map.raw)
        fail("history.map_config", "does not match this match's exact map configuration");
    if (integer(h["completed_days"], "history.completed_days", 0, 10) != d)
        fail("history.completed_days", "does not match daily day index");
    auto kinds = validate_kinds(h["agent_kinds"], n);
    array(h["collected_brands"], "history.collected_brands", n, n);
    for (size_t i = 0; i < n; ++i) {
        if (kinds[i] != day["agents"][i]["kind"])
            fail("history.agent_kinds", "agent type changed within a match");
        auto values = integers(h["collected_brands"][i], index_path("history.collected_brands", i),
                               0, map.brands.size(), INT64_MIN, INT64_MAX);
        std::set<int64_t> seen;
        for (int64_t brand : values) {
            auto found = std::lower_bound(map.brands.begin(), map.brands.end(), brand);
            if (found == map.brands.end() || *found != brand || !seen.insert(brand).second)
                fail("history.collected_brands", "unknown or repeated brand");
            result[i].push_back(int(found - map.brands.begin()));
        }
        if (!values.empty() && (d == 0 || kinds[i] == 1))
            fail("history.collected_brands", "day zero and supply agents cannot have acquisitions");
    }
    return result;
}

DailyPlan wait_plan(const Case &c) {
    return {std::vector<std::vector<int>>(c.input.agents.size(), {-c.input.day_steps})};
}

Json history_draft(const Input &input, const SimulationResult &replay) {
    Json kinds = Json::array(), acquired = Json::array();
    for (size_t i = 0; i < replay.committed_state.agents.size(); ++i) {
        kinds.push_back(input.day["agents"][i]["kind"]);
        Json row = Json::array();
        for (int dense : replay.committed_state.agents[i].collected_series)
            row.push_back(input.map.brands.at(size_t(dense)));
        acquired.push_back(row);
    }
    return {{"format", history_format}, {"map_config", input.map.raw},
            {"completed_days", input.day_index + 1}, {"confirmed", false},
            {"agent_kinds", kinds}, {"collected_brands", acquired}};
}

Output checked_output(const Input &input, const Case &c, const Json &proposed,
                      bool fallback, std::optional<Objective> expected = {}) {
    DailyPlan plan;
    SimulationResult replay;
    std::string reason;
    try {
        plan = parse_answer(proposed, c);
        replay = RuleReplay().runDailyPlan(c.input, plan);
        if (!replay.valid)
            fail("answer.replay", toString(replay.error.code) + " step=" +
                                     std::to_string(replay.error.step) + " agent=" +
                                     std::to_string(replay.error.agent_id) + ": " + replay.error.message);
        if (expected && replay_objective(c, replay) != *expected)
            fail("answer.objective", "solver objective disagrees with independent final replay");
    } catch (const std::invalid_argument &e) {
        if (!fallback)
            throw;
        reason = e.what();
        plan = wait_plan(c);
        replay = RuleReplay().runDailyPlan(c.input, plan);
    }
    if (!replay.valid)
        throw std::logic_error("all-wait fallback failed replay");
    Json answer = plan.actions;
    if (parse_answer(answer, c).actions != plan.actions)
        throw std::logic_error("answer serialization round-trip mismatch");
    std::vector<int> residence(c.input.map.cells.size(), 0);
    for (const auto &step : replay.trace)
        for (const auto &agent : step.state.agents)
            if (c.input.map.cells[agent.cell_id].terrain == Terrain::Road)
                ++residence[agent.cell_id];
    Json report = {{"format", "c_staged.local_report.v1"}, {"day", input.day_index},
                   {"objective", replay_objective(c, replay)}, {"replay", replay_json(replay)},
                   {"brand_dictionary", input.map.brands}, {"used_fallback", !reason.empty()},
                   {"fallback_reason", reason}, {"own_road_residence", residence},
                   {"next_history_draft", history_draft(input, replay)}};
    return {answer, report};
}

double unix_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

Json read_json(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        fail(path, "cannot open JSON file");
    input.seekg(0, std::ios::end);
    auto size = input.tellg();
    if (size < 0 || size > 16 * 1024 * 1024)
        fail(path, "JSON file exceeds adapter 16 MiB limit or cannot be read");
    input.seekg(0);
    std::vector<std::set<std::string>> keys;
    auto check = [&](int, Json::parse_event_t event, Json &parsed) {
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            fail(path, "duplicate JSON key: " + parsed.get<std::string>());
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    };
    try {
        return Json::parse(input, check);
    } catch (const Json::exception &e) {
        fail(path, std::string("malformed JSON: ") + e.what());
    }
}

MapDocument parse_map(const Json &j) {
    object(j, "map_config", {"startsAt", "daySeconds", "daySteps", "map", "spots",
                              "agents", "fuelLimits", "players", "busyThreshold", "jammedThreshold"});
    MapDocument m;
    m.raw = j;
    m.starts_at = integer(j["startsAt"], "startsAt", 0, 9007199254740991LL);
    auto steps = integers(j["daySteps"], "daySteps", 1, 10, 1, 256);
    m.day_steps.assign(steps.begin(), steps.end());
    m.day_seconds = integers(j["daySeconds"], "daySeconds", steps.size(), steps.size(), 1, INT_MAX);
    object(j["map"], "map", {"height", "width", "cells"});
    m.width = int(integer(j["map"]["width"], "map.width", 1, 32));
    m.height = int(integer(j["map"]["height"], "map.height", 1, 32));
    array(j["map"]["cells"], "map.cells", m.height, m.height);
    for (int y = 0; y < m.height; ++y) {
        auto row = integers(j["map"]["cells"][y], index_path("map.cells", y), m.width, m.width, 0, 3);
        m.terrain.insert(m.terrain.end(), row.begin(), row.end());
    }
    auto positions = integers(j["agents"], "agents", 1, 8, 0, m.width * m.height - 1);
    m.initial_positions.assign(positions.begin(), positions.end());
    m.capacity = int(integer(j["fuelLimits"], "fuelLimits", 1, 768));
    m.players = int(integer(j["players"], "players", 1, INT_MAX));
    integer(j["busyThreshold"], "busyThreshold", 1, 5);
    integer(j["jammedThreshold"], "jammedThreshold", 2, 10);
    array(j["spots"], "spots", 0, 32);
    std::set<int> cells;
    for (size_t i = 0; i < j["spots"].size(); ++i) {
        const auto &s = j["spots"][i];
        auto p = index_path("spots", i);
        object(s, p, {"brand", "pos", "stocks"});
        m.brands.push_back(integer(s["brand"], p + ".brand", INT64_MIN, INT64_MAX));
        int pos = int(integer(s["pos"], p + ".pos", 0, m.width * m.height - 1));
        if (m.terrain[pos] != 0 || !cells.insert(pos).second)
            fail(p + ".pos", "Spot requires its own plain cell");
        integer(s["stocks"], p + ".stocks", 1, positions.size());
    }
    std::sort(m.brands.begin(), m.brands.end());
    m.brands.erase(std::unique(m.brands.begin(), m.brands.end()), m.brands.end());
    for (int pos : m.initial_positions)
        if (m.terrain[pos] == 3)
            fail("agents", "initial position is a pond");
    return m;
}

void validate_contest_map(const MapDocument &m) {
    if (m.width < 8 || m.height < 8 || m.day_steps.size() < 4 || m.initial_positions.size() < 3)
        fail("contest", "requires H,W>=8, days>=4 and agents>=3; synthetic tests need --allow-synthetic");
    if (m.raw["spots"].size() < m.initial_positions.size() ||
        m.raw["spots"].size() > size_t(std::max(m.width, m.height)))
        fail("contest.spots", "Spot count must be agents..max(height,width)");
    for (int t : m.day_steps)
        if (t < m.width + m.height || t > 4 * (m.width + m.height))
            fail("contest.daySteps", "horizon violates PDF Q20");
    if (m.capacity < m.day_steps[0] || m.capacity > 3 * m.day_steps[0])
        fail("contest.fuelLimits", "capacity violates PDF Q14/Q60");
    for (int terrain = 0; terrain < 4; ++terrain)
        if (std::find(m.terrain.begin(), m.terrain.end(), terrain) == m.terrain.end())
            fail("contest.map", "each terrain must exist (Q35)");
    std::set<int> initial;
    for (int pos : m.initial_positions) {
        if (m.terrain[pos] != 0 || !initial.insert(pos).second)
            fail("contest.agents", "initial positions must be distinct plain cells");
        for (const auto &s : m.raw["spots"])
            if (s["pos"] == pos)
                fail("contest.agents", "initial positions must not be Spots");
    }
    Map geometry;
    geometry.width = m.width;
    geometry.height = m.height;
    geometry.cells.resize(m.terrain.size());
    std::set<int> reached{m.initial_positions.front()};
    std::vector<int> queue{m.initial_positions.front()};
    for (size_t i = 0; i < queue.size(); ++i)
        for (int d = 0; d < 6; ++d) {
            int next = geometry.neighbor(queue[i], d);
            if (next >= 0 && m.terrain[next] != 3 && reached.insert(next).second)
                queue.push_back(next);
        }
    if (reached.size() != size_t(std::count_if(m.terrain.begin(), m.terrain.end(), [](int t) { return t != 3; })))
        fail("contest.map", "traversable cells must be connected");
}

void validate_day_shape(const Json &j, size_t n) {
    object(j, "daily", {"endsAt", "day", "agents", "others", "traffics"});
    integer(j["endsAt"], "endsAt", 0, 9007199254740991LL);
    integer(j["day"], "day", 0, 9);
    agent_shapes(j["agents"], "daily.agents", n);
    array(j["others"], "others", 0, 10000);
    std::set<int64_t> ids;
    for (size_t i = 0; i < j["others"].size(); ++i) {
        auto p = index_path("others", i);
        const auto &other = j["others"][i];
        object(other, p, {"id", "agents"});
        if (!ids.insert(integer(other["id"], p + ".id", INT64_MIN, INT64_MAX)).second)
            fail(p + ".id", "duplicate other-team ID");
        agent_shapes(other["agents"], p + ".agents", n);
    }
    array(j["traffics"], "traffics", 0, 1024);
    std::set<int> positions;
    for (size_t i = 0; i < j["traffics"].size(); ++i) {
        auto p = index_path("traffics", i);
        object(j["traffics"][i], p, {"pos", "status"});
        int pos = int(integer(j["traffics"][i]["pos"], p + ".pos", 0, 1023));
        if (!positions.insert(pos).second)
            fail(p + ".pos", "duplicate traffic cell");
        integer(j["traffics"][i]["status"], p + ".status", 0, 2);
    }
}

Json validate_kinds(const Json &j, size_t n) {
    integers(j, "kinds", n, n, 0, 1);
    return j;
}

Input convert(const MapDocument &m, const Json &day, const std::optional<Json> &history,
              bool allow_synthetic) {
    validate_day_shape(day, m.initial_positions.size());
    if (!allow_synthetic)
        validate_contest_map(m);
    int d = day["day"];
    if (d >= int(m.day_steps.size()))
        fail("day", "index is outside daySteps; first day is zero");
    if (day["others"].size() >= size_t(m.players))
        fail("others", "other-team count must be less than players");
    agent_physics(m, day["agents"], "daily.agents");
    for (size_t i = 0; i < day["others"].size(); ++i)
        agent_physics(m, day["others"][i]["agents"], index_path("others", i) + ".agents");
    if (!allow_synthetic && d == 0)
        for (size_t i = 0; i < m.initial_positions.size(); ++i) {
            const auto &a = day["agents"][i];
            if (a["pos"] != m.initial_positions[i] || (a["kind"] == 0 && a["fuel"] != m.capacity))
                fail("daily.agents", "first-day position/fuel disagrees with initial conditions");
        }
    std::vector<int> status(m.terrain.size(), -1);
    for (const auto &x : day["traffics"]) {
        int pos = x["pos"];
        if (pos >= int(m.terrain.size()) || m.terrain[pos] != 1)
            fail("traffics.pos", "entry is not a road cell; PDF p17/p18 samples are inconsistent");
        status[pos] = x["status"];
        if (!allow_synthetic && d == 0 && status[pos] != 0)
            fail("traffics.status", "first-day roads must be free");
    }
    Json cells = Json::array(), spots = Json::array(), agents = Json::array();
    static const char *terrains[] = {"plain", "road", "mountain", "lake"};
    static const char *roads[] = {"clear", "crowded", "jammed"};
    for (size_t i = 0; i < m.terrain.size(); ++i) {
        if (m.terrain[i] == 1 && status[i] < 0)
            fail("traffics", "missing road status for cell " + std::to_string(i) + "; no default specified");
        cells.push_back({{"terrain", terrains[m.terrain[i]]},
                         {"road", roads[m.terrain[i] == 1 ? status[i] : 0]}});
    }
    for (size_t i = 0; i < m.raw["spots"].size(); ++i) {
        const auto &s = m.raw["spots"][i];
        int64_t brand = s["brand"];
        int dense = int(std::lower_bound(m.brands.begin(), m.brands.end(), brand) - m.brands.begin());
        spots.push_back({{"id", i}, {"cell", s["pos"]}, {"series", dense},
                         {"stock", s["stocks"]}, {"stock_max", s["stocks"]}});
    }
    auto past = history_sets(m, day, history);
    for (size_t i = 0; i < m.initial_positions.size(); ++i) {
        const auto &a = day["agents"][i];
        bool patrol = a["kind"] == 0;
        agents.push_back({{"id", i}, {"type", patrol ? "patrol" : "supply"},
                          {"cell", a["pos"]}, {"fuel", a["fuel"]},
                          {"capacity", patrol ? m.capacity : 0}, {"udon", 0},
                          {"visited_today", Json::array()}, {"series_today", Json::array()},
                          {"collected_series", past[i]}});
    }
    Json normalized = {{"id", "official_day_" + std::to_string(d)},
                        {"day_steps", m.day_steps[d]}, {"initial_balls_today", 0},
                        {"map", {{"width", m.width}, {"height", m.height}, {"cells", cells}}},
                        {"spots", spots}, {"agents", agents}};
    return {m, day, normalized, d, day["endsAt"].get<int64_t>()};
}

DailyPlan parse_answer(const Json &answer, const Case &c) {
    const size_t n = c.input.agents.size();
    array(answer, "answer", n, n);
    DailyPlan plan;
    for (size_t i = 0; i < n; ++i) {
        const auto p = index_path("answer", i);
        auto values = integers(answer[i], p, 1, c.input.day_steps, -c.input.day_steps, 5);
        int pos = c.input.initial_state.agents[i].cell_id, elapsed = 0;
        std::vector<int> commands;
        for (size_t k = 0; k < values.size(); ++k) {
            int value = int(values[k]);
            if (value < 0)
                elapsed -= value;
            else {
                int next = c.input.map.neighbor(pos, value);
                if (!c.walk(next))
                    fail(index_path(p, k), "move leaves map or enters pond");
                elapsed += c.tau[pos];
                pos = next;
            }
            if (elapsed > c.input.day_steps)
                fail(p, "action durations exceed daySteps");
            commands.push_back(value);
        }
        if (elapsed != c.input.day_steps)
            fail(p, "action durations do not fill daySteps; append an explicit wait");
        plan.actions.push_back(std::move(commands));
    }
    return plan;
}

Output validate_answer(const Input &input, const Json &answer, bool fallback) {
    const Case c(input.normalized);
    return checked_output(input, c, answer, fallback);
}

Output solve(const Input &input, bool offline, int64_t test_check_budget, double budget_seconds) {
    if (!std::isfinite(budget_seconds) || budget_seconds < 0 || budget_seconds > solver_seconds)
        fail("budget_seconds", "must be finite and within 0..55");
    Deadline deadline;
    deadline.seconds = budget_seconds; // Caller may shorten, never enlarge, the 55s cap.
    const Case c(input.normalized);
    double allowed = budget_seconds;
    if (!offline)
        allowed = std::max(0.0, std::min({allowed, double(input.map.day_seconds[input.day_index]),
                                        double(input.ends_at) - unix_now()}));
    deadline.seconds = std::max(0.0, allowed - finalization_seconds);
    deadline.remaining_checks = test_check_budget;
    GhrccOptions options;
    options.local_seconds = 0.5;
    auto result = solve_day(c, 50, deadline, false, true, true, options);
    auto output = checked_output(input, c, Json(result.plan.actions), true, result.objective);
    output.report["requested_seconds"] = budget_seconds;
    output.report["policy_ceiling_seconds"] = solver_seconds;
    output.report["search_deadline_seconds"] = deadline.seconds;
    output.report["finalization_reserve_seconds"] = finalization_seconds;
    output.report["offline"] = offline;
    output.report["submission_window_open"] = !offline && unix_now() < double(input.ends_at);
    output.report["solver_completed"] = result.completed;
    output.report["solver_metrics"] = result.metrics;
    output.report["nodes"] = result.nodes;
    output.report["elapsed_seconds"] = deadline.elapsed();
    output.report["solver_supply_chains"] = result.supply_chains;
    return output;
}
} // namespace contest_solver::contest
