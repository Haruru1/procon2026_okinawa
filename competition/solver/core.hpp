#pragma once
#include "replay/replay.hpp"
#include "third_party/json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>

namespace contest_solver {
using Json = nlohmann::json;
using namespace replay;
using Objective = std::array<int, 3>;
constexpr int INF = 1000000000;
inline int pop(uint64_t x) {
    int n = 0;
    while (x) {
        x &= x - 1;
        ++n;
    }
    return n;
}
struct Timeout {
    bool local = false;
};
struct SecondsScope {
    double &total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit SecondsScope(double &value) : total(value) {}
    ~SecondsScope() {
        total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
};
struct Deadline {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now();
    double seconds = 60;
    const Deadline *parent = nullptr;
    // Test-only deterministic cancellation; negative means wall-clock only.
    mutable int64_t remaining_checks = -1;
    bool expired() const {
        return elapsed() >= seconds;
    }
    double elapsed() const {
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
    void check() const {
        if (parent)
            parent->check();
        if (remaining_checks == 0 || expired())
            throw Timeout{parent != nullptr};
        if (remaining_checks > 0)
            --remaining_checks;
    }
};
struct Case {
    DailyInput input;
    int initial_balls = 0;
    std::vector<int> patrols, supplies, tau, burn, spot_at, series_values;
    std::vector<std::array<int, 6>> neighbors;
    std::vector<uint64_t> spot_series;
    std::set<int> initial_global, initial_daily;
    // Only the normalized research CLI can select the frozen Python contact model.
    bool legacy_contacts = false;
    explicit Case(const Json &j, bool legacy = false);
    bool walk(int cell) const {
        return cell >= 0 && cell < (int)tau.size() && tau[cell] < INF;
    }
    uint64_t series(uint64_t mask) const {
        uint64_t result = 0;
        for (size_t s = 0; s < spot_series.size(); ++s)
            if (mask & (1ULL << s))
                result |= spot_series[s];
        return result;
    }
    Objective objective(const std::vector<int> &counts) const;
};
struct Travel {
    const Case &c;
    std::vector<std::vector<int>> distance;
    Travel(const Case &c, const Deadline &deadline);
    std::pair<int, std::vector<int>> path(int source, int target) const;
};
using Event = std::pair<int, int>; // reflection time, cell
using Clause = std::vector<Event>;
using Formula = std::vector<Clause>;
inline bool subset(const Clause &a, const Clause &b) {
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}
inline Formula canonical(Formula raw) {
    for (auto &g : raw) {
        std::sort(g.begin(), g.end());
        g.erase(std::unique(g.begin(), g.end()), g.end());
    }
    std::sort(raw.begin(), raw.end(), [](const Clause &a, const Clause &b) {
        return std::make_tuple(a.size(), a) < std::make_tuple(b.size(), b);
    });
    Formula result;
    for (auto &g : raw)
        if (std::none_of(result.begin(), result.end(),
                         [&](const Clause &h) { return subset(h, g); }))
            result.push_back(std::move(g));
    return result;
}
struct Trace {
    std::vector<int> positions, burns, cumulative;
};
struct Course {
    uint64_t mask = 0, series = 0;
    int elapsed = 0, fuel = 0, cell = 0, patrol = 0, burn = 0;
    std::vector<int> actions;
    std::vector<std::pair<int, int>> spot_events; // Spot index, arrival time
    Formula requirements;
    Trace trace;
    bool mandatory = false;
};
Trace trace(const Case &c, int patrol, const std::vector<int> &actions);
Formula critical(const Case &c, int patrol, const Trace &tr);
Course profile(const Case &c, int patrol, const std::vector<int> &actions);
Json course_json(const Course &course, bool full = true);
struct Pool {
    int patrol = 0;
    uint64_t generated = 0;
    std::vector<Course> movement, courses;
    double diverse_seconds = 0;
};
Pool generate_pool(const Case &c, const Travel &travel, int patrol, int limit,
                   const Deadline &deadline);
Json replay_json(const SimulationResult &r);
Objective replay_objective(const Case &c, const SimulationResult &r);
} // namespace contest_solver
