#include "core.hpp"

namespace contest_solver {
Case::Case(const Json &j, bool legacy) : legacy_contacts(legacy) {
    auto &m = j.at("map");
    input.map.width = m.at("width");
    input.map.height = m.at("height");
    input.day_steps = j.at("day_steps");
    initial_balls = j.value("initial_balls_today", 0);
    for (auto &x : m.at("cells")) {
        std::string terrain = x.at("terrain"), road = x.value("road", std::string("clear"));
        const std::map<std::string, Terrain> ts = {{"plain", Terrain::Plain},
                                                   {"road", Terrain::Road},
                                                   {"mountain", Terrain::Mountain},
                                                   {"lake", Terrain::Lake}};
        const std::map<std::string, RoadLevel> rs = {{"clear", RoadLevel::Clear},
                                                     {"crowded", RoadLevel::Crowded},
                                                     {"jammed", RoadLevel::Jammed}};
        input.map.cells.push_back({ts.at(terrain), rs.at(road)});
    }
    for (auto &x : j.at("spots")) {
        input.spots.push_back({x.at("id"), x.at("cell"), x.at("series"),
                               x.value("stock_max", x.at("stock").get<int>())});
        input.initial_state.spot_stock.push_back(x.at("stock"));
        series_values.push_back(x.at("series"));
    }
    std::sort(series_values.begin(), series_values.end());
    series_values.erase(std::unique(series_values.begin(), series_values.end()),
                        series_values.end());
    if (input.spots.size() > 32 || series_values.size() > 32)
        throw std::invalid_argument("contest supports at most 32 Spots");
    auto getset = [](const Json &x, const char *key) {
        auto v = x.value(key, std::vector<int>{});
        return std::set<int>(v.begin(), v.end());
    };
    for (auto &x : j.at("agents")) {
        bool patrol = x.at("type") == "patrol";
        if (!patrol && x.at("type") != "supply")
            throw std::invalid_argument("invalid agent type");
        (patrol ? patrols : supplies).push_back((int)input.agents.size());
        input.agents.push_back(
            {x.at("id"), patrol ? AgentType::Patrol : AgentType::Supply, x.value("capacity", 0)});
        AgentState st;
        st.cell_id = x.at("cell");
        st.fuel = x.value("fuel", 0);
        st.udon = x.value("udon", 0);
        st.visited_today = getset(x, "visited_today");
        st.series_today = getset(x, "series_today");
        st.collected_series = getset(x, "collected_series");
        initial_global.insert(st.collected_series.begin(), st.collected_series.end());
        initial_daily.insert(st.series_today.begin(), st.series_today.end());
        input.initial_state.agents.push_back(st);
    }
    if (input.map.width <= 0 || input.map.height <= 0 ||
        input.map.width * input.map.height != (int)input.map.cells.size() || input.day_steps <= 0 ||
        input.day_steps > 256)
        throw std::invalid_argument("invalid contest dimensions/horizon");
    for (auto &cell : input.map.cells) {
        tau.push_back(cell.terrain == Terrain::Lake ? INF : stepCost(cell));
        burn.push_back(cell.terrain == Terrain::Lake ? INF : fuelCost(cell));
    }
    spot_at.assign(tau.size(), -1);
    neighbors.resize(tau.size());
    for (size_t i = 0; i < tau.size(); ++i)
        for (int d = 0; d < 6; ++d) {
            int dest = input.map.neighbor((int)i, d);
            neighbors[i][d] = walk(dest) ? dest : -1;
        }
    for (size_t s = 0; s < input.spots.size(); ++s) {
        const auto &sp = input.spots[s];
        if (!walk(sp.cell_id))
            throw std::invalid_argument("invalid Spot cell");
        spot_at[sp.cell_id] = (int)s;
        spot_series.push_back(
            1ULL << (std::lower_bound(series_values.begin(), series_values.end(), sp.series) -
                     series_values.begin()));
    }
}
Objective Case::objective(const std::vector<int> &counts) const {
    std::set<int> acquired, daily = initial_daily;
    int balls = initial_balls, fresh = 0;
    for (size_t s = 0; s < counts.size(); ++s)
        if (counts[s]) {
            acquired.insert(input.spots[s].series);
            balls += counts[s];
        }
    for (int b : acquired) {
        fresh += !initial_global.count(b);
        daily.insert(b);
    }
    return {fresh, (int)daily.size(), balls};
}
Travel::Travel(const Case &cc, const Deadline &deadline) : c(cc) {
    distance.assign(c.tau.size(), std::vector<int>(c.tau.size(), INF));
    for (int source = 0; source < (int)c.tau.size(); ++source) {
        deadline.check();
        auto &row = distance[source];
        row[source] = 0;
        if (!c.walk(source))
            continue;
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>,
                            std::greater<std::pair<int, int>>>
            q;
        q.push({0, source});
        while (!q.empty()) {
            deadline.check();
            auto [v, cell] = q.top();
            q.pop();
            if (v != row[cell])
                continue;
            for (int dest : c.neighbors[cell])
                if (dest >= 0 && v + c.tau[cell] < row[dest]) {
                    row[dest] = v + c.tau[cell];
                    q.push({row[dest], dest});
                }
        }
    }
}
std::pair<int, std::vector<int>> Travel::path(int source, int target) const {
    std::vector<int> dist(c.tau.size(), INF), parent(c.tau.size(), -1), direction(c.tau.size(), -1);
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>,
                        std::greater<std::pair<int, int>>>
        q;
    q.push({0, source});
    dist[source] = 0;
    while (!q.empty()) {
        auto [v, cell] = q.top();
        q.pop();
        if (v != dist[cell])
            continue;
        if (cell == target)
            break;
        for (int d = 0; d < 6; ++d) {
            int dest = c.neighbors[cell][d];
            if (dest < 0 || v + c.tau[cell] >= dist[dest])
                continue;
            dist[dest] = v + c.tau[cell];
            parent[dest] = cell;
            direction[dest] = d;
            q.push({dist[dest], dest});
        }
    }
    if (dist[target] >= INF)
        throw std::runtime_error("unreachable witness");
    std::vector<int> actions;
    for (int cell = target; cell != source; cell = parent[cell])
        actions.push_back(direction[cell]);
    std::reverse(actions.begin(), actions.end());
    return {dist[target], actions};
}
Trace trace(const Case &c, int patrol, const std::vector<int> &actions) {
    Trace tr;
    int cell = c.input.initial_state.agents.at(patrol).cell_id;
    tr.positions.push_back(cell);
    tr.burns.push_back(0);
    tr.cumulative.push_back(0);
    for (int a : actions) {
        if (a >= 6)
            throw std::invalid_argument("invalid action");
        int duration = a < 0 ? -a : c.tau[cell], dest = a < 0 ? cell : c.neighbors[cell][a];
        if (dest < 0 || duration <= 0 || duration > c.input.day_steps ||
            (int)tr.positions.size() - 1 + duration > c.input.day_steps)
            throw std::invalid_argument("invalid trace");
        for (int k = 1; k <= duration; ++k) {
            tr.positions.push_back(a < 0 ? cell : (k == duration ? dest :
                                                  (c.legacy_contacts ? -1 : cell)));
            int b = a >= 0 && k == duration ? c.burn[cell] : 0;
            tr.burns.push_back(b);
            tr.cumulative.push_back(tr.cumulative.back() + b);
        }
        cell = dest;
    }
    if ((int)tr.positions.size() != c.input.day_steps + 1)
        throw std::invalid_argument("trace must fill day_steps");
    return tr;
}
Formula critical(const Case &c, int patrol, const Trace &tr) {
    Formula raw;
    int T = c.input.day_steps;
    for (int anchor = 0; anchor < T; ++anchor) {
        int capacity = anchor ? c.input.agents[patrol].fuel_capacity
                              : c.input.initial_state.agents[patrol].fuel;
        int over = anchor + 1;
        while (over <= T && tr.cumulative[over] - tr.cumulative[anchor] <= capacity)
            ++over;
        if (over > T)
            continue;
        Clause g;
        for (int t = anchor + 1; t < over; ++t)
            if (tr.positions[t] >= 0)
                g.push_back({t, tr.positions[t]});
        raw.push_back(std::move(g));
    }
    return canonical(std::move(raw));
}
Course profile(const Case &c, int patrol, const std::vector<int> &actions) {
    Course r;
    r.patrol = patrol;
    r.actions = actions;
    r.trace = trace(c, patrol, actions);
    r.requirements = critical(c, patrol, r.trace);
    r.burn = r.trace.cumulative.back();
    r.cell = r.trace.positions.back();
    int cell = c.input.initial_state.agents[patrol].cell_id, t = 0;
    std::set<int> seen;
    auto acquire = [&](int reflection_time) {
        int s = c.spot_at[cell];
        if (s < 0 || !seen.insert(s).second)
            return;
        if (c.input.initial_state.spot_stock[s] <= 0 ||
            c.input.initial_state.agents[patrol].visited_today.count(c.input.spots[s].id))
            return;
        r.spot_events.push_back({s, reflection_time});
        r.mask |= 1ULL << s;
    };
    for (int a : actions) {
        if (a < 0) {
            // No acquisition at step zero; a wait acquires at its first reflection,
            // not at the end of the wait command (important on later-day starts).
            acquire(t + 1);
            t -= a;
            continue;
        }
        // Until arrival the official reflected position is still the source.
        if (c.tau[cell] > 1)
            acquire(t + 1);
        t += c.tau[cell];
        r.elapsed += c.tau[cell];
        cell = c.neighbors[cell][a];
        acquire(t);
    }
    r.series = c.series(r.mask);
    r.fuel = c.input.initial_state.agents[patrol].fuel - r.burn;
    return r;
}
Json course_json(const Course &r, bool full) {
    Json j = {{"spot_mask", r.mask}, {"series_mask", r.series}, {"elapsed", r.elapsed},
              {"fuel", r.fuel},      {"end_cell", r.cell},      {"actions", r.actions}};
    if (full) {
        j["spot_events"] = r.spot_events;
        j["requirements"] = r.requirements;
        j["positions"] = r.trace.positions;
        j["burns"] = r.trace.burns;
        j["cumulative_burn"] = r.trace.cumulative;
        j["total_burn"] = r.burn;
    }
    return j;
}
Json replay_json(const SimulationResult &r) {
    auto world = [](const WorldState &w) {
        Json agents = Json::array();
        for (const auto &a : w.agents)
            agents.push_back({{"cell", a.cell_id},
                              {"fuel", a.fuel},
                              {"udon", a.udon},
                              {"visited_today", a.visited_today},
                              {"series_today", a.series_today},
                              {"collected_series", a.collected_series}});
        return Json{{"agents", agents}, {"spot_stock", w.spot_stock}};
    };
    Json frames = Json::array();
    for (const auto &f : r.trace) {
        Json acq = Json::array(), active = Json::array();
        for (const auto &a : f.acquisitions)
            acq.push_back({{"agent_id", a.agent_id}, {"spot_id", a.spot_id}});
        for (const auto &a : f.active_actions)
            active.push_back({{"moving", a.moving},
                              {"from_cell", a.from_cell},
                              {"to_cell", a.to_cell},
                              {"remaining", a.remaining}});
        frames.push_back({{"step", f.step},
                          {"state", world(f.state)},
                          {"acquisitions", acq},
                          {"active_actions", active},
                          {"refueled_agent_ids", f.refueled_agent_ids}});
    }
    return {{"valid", r.valid},
            {"error", toString(r.error.code)},
            {"committed_state", world(r.committed_state)},
            {"trace", frames}};
}
Objective replay_objective(const Case &c, const SimulationResult &r) {
    std::set<int> global, day;
    int balls = c.initial_balls;
    for (size_t i = 0; i < r.committed_state.agents.size(); ++i) {
        const auto &a = r.committed_state.agents[i];
        global.insert(a.collected_series.begin(), a.collected_series.end());
        day.insert(a.series_today.begin(), a.series_today.end());
        balls += a.udon - c.input.initial_state.agents[i].udon;
    }
    int fresh = 0;
    for (int b : global)
        fresh += !c.initial_global.count(b);
    return {fresh, (int)day.size(), balls};
}
} // namespace contest_solver
