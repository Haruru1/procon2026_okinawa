// Directly exercise the existing private frontier/upper helpers without adding
// production test hooks. Do not link candidates.cpp or solver.cpp a second time.
#include "../candidates.cpp"
#include "../solver.cpp"
#include <iostream>

using namespace contest_solver;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::logic_error(message);
}

Json line_case(int cells, int patrols) {
    Json j = {{"map", {{"width", cells}, {"height", 1}, {"cells", Json::array()}}},
              {"day_steps", 6}, {"spots", Json::array()}, {"agents", Json::array()}};
    for (int i = 0; i < cells; ++i)
        j["map"]["cells"].push_back({{"terrain", "plain"}});
    for (int i = 0; i < patrols; ++i)
        j["agents"].push_back({{"id", i}, {"type", "patrol"}, {"cell", 0},
                                {"fuel", 20}, {"capacity", 20}});
    return j;
}

void effect_deduplication() {
    auto j = line_case(3, 1);
    j["spots"].push_back({{"id", 0}, {"cell", 1}, {"series", 7}, {"stock", 1}});
    const Case c(j);
    Deadline deadline;
    Travel travel(c, deadline);
    for (int limit : {2, 50}) {
        auto pool = generate_pool(c, travel, 0, limit, deadline);
        require(pool.movement.size() <= size_t(limit), "movement limit changed");
        require(pool.courses.size() <= size_t(limit + 1), "optional limit changed");
        std::set<std::pair<uint64_t, Formula>> keys;
        std::set<int> endpoints;
        bool wait = false;
        for (const auto &r : pool.courses) {
            require(keys.emplace(r.mask, r.requirements).second,
                    "same effect was not deduplicated across endpoints");
            wait |= r.actions == std::vector<int>{-6};
            if (r.mask == 1 && r.requirements.empty())
                endpoints.insert(r.cell);
        }
        require(wait, "all-wait fallback disappeared");
        if (limit == 50) {
            std::set<int> movement_endpoints;
            for (const auto &r : pool.movement)
                if (r.mask == 1)
                    movement_endpoints.insert(r.cell);
            require(movement_endpoints == std::set<int>{0, 1, 2},
                    "fixture must have different endpoints before final dedup");
            require(endpoints.size() == 1,
                    "stable final dedup must retain only one endpoint per effect");
        }
    }
    std::cout << "PASS stable effect dedup across endpoints, unchanged limits\n";
}

void frontier_dominance() {
    Course a, b, other_cell;
    a.cell = b.cell = 0;
    a.elapsed = 2; a.fuel = 5; a.mask = 3;
    b.elapsed = 3; b.fuel = 4; b.mask = 1;
    other_cell = b;
    other_cell.cell = 1;
    require(dominates(a, b), "safe dominance regressed");
    require(!dominates(a, other_cell), "cross-cell dominance is unsafe");
    auto kept = frontier({a, b, other_cell}, Deadline{});
    require(kept.size() == 2, "frontier must remove only dominated same-cell course");
    std::cout << "PASS unchanged frontier dominance\n";
}

void rank_at_depth(int depth, bool dynamic) {
    // Earlier patrol acquires A+B. Target can take A+B (static preference) or C
    // (dynamic preference). The final patrol can also take C, so both branches
    // attain the same optimum and the first incumbent reveals the actual order.
    auto j = line_case(5, depth + 2);
    for (int i = 0; i < 3; ++i)
        j["spots"].push_back({{"id", i}, {"cell", i == 2 ? 4 : i},
                               {"series", i}, {"stock", 1}});
    for (auto &a : j["agents"])
        a["cell"] = 2;
    j["agents"].back()["cell"] = 3;
    const Case c(j);
    Deadline deadline;
    Travel travel(c, deadline);
    std::vector<Pool> pools(depth + 2);
    for (int p = 0; p < int(pools.size()); ++p) {
        pools[p].patrol = p;
        pools[p].courses.push_back(profile(c, p, {-6}));
    }
    pools[0].courses.push_back(profile(c, 0, {5, 5, -2}));
    pools[depth].courses.push_back(profile(c, depth, {5, 5, -2}));
    pools[depth].courses.push_back(profile(c, depth, {2, 2, -2}));
    pools.back().courses.push_back(profile(c, depth + 1, {2, -4}));

    std::vector<std::vector<Course>> working;
    for (const auto &p : pools)
        working.push_back(p.courses);
    const Objective optimum{3, 3, 3};
    std::function<Objective(int, const std::vector<int> &)> exhaustive;
    exhaustive = [&](int d, const std::vector<int> &counts) {
        auto expected = c.objective(counts);
        if (d < int(working.size()))
            for (const auto &r : working[d])
                expected = std::max(expected, exhaustive(d + 1, add(c, counts, r)));
        require(upper(c, working, d, counts, deadline) >= expected,
                "upper bound excludes a completion");
        return expected;
    };
    require(exhaustive(0, {0, 0, 0}) == optimum, "fixture optimum differs");
    auto result = solve_team(c, travel, pools, deadline);
    require(result.completed && result.objective == optimum, "B&B optimum differs");
    require(result.replay.valid, "invalid final plan");
    require(result.selected[depth].mask == uint64_t(dynamic ? 4 : 3),
            "dynamic/static ranking boundary differs");
    std::cout << "PASS depth=" << depth << (dynamic ? " dynamic" : " static")
              << ", exhaustive upper bound, optimum and replay\n";
}

int main() {
    try {
        effect_deduplication();
        frontier_dominance();
        rank_at_depth(2, true);
        rank_at_depth(3, false);
        rank_at_depth(4, false);
        rank_at_depth(5, false);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
