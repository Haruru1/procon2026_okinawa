#include "core.hpp"

namespace contest_solver {
namespace {
using Key = std::tuple<uint64_t, int, int, int>;
Key key(const Course &r) {
    return {r.mask, r.elapsed, r.fuel, r.cell};
}
auto quality(const Case &c, const Course &r) {
    std::vector<int> counts(c.input.spots.size());
    for (size_t s = 0; s < counts.size(); ++s)
        counts[s] = bool(r.mask & (1ULL << s));
    Objective score = c.objective(counts);
    score[2] = pop(r.mask); // movement generator excludes initial balls
    return std::make_tuple(score, pop(r.series), pop(r.mask), -r.elapsed, r.fuel, -r.cell,
                           -(int64_t)r.mask);
}
bool dominates(const Course &a, const Course &b) {
    return a.cell == b.cell && a.elapsed <= b.elapsed && a.fuel >= b.fuel &&
           (a.mask | b.mask) == a.mask &&
           (a.elapsed < b.elapsed || a.fuel > b.fuel || a.mask != b.mask);
}
std::vector<Course> frontier(const std::vector<Course> &input, const Deadline &deadline) {
    std::vector<Course> v;
    std::map<Key, size_t> idx;
    for (auto &r : input) {
        auto k = key(r);
        auto it = idx.find(k);
        if (it == idx.end()) {
            idx[k] = v.size();
            v.push_back(r);
        } else
            v[it->second] = r;
    }
    std::vector<int> cells;
    for (auto &r : v)
        if (std::find(cells.begin(), cells.end(), r.cell) == cells.end())
            cells.push_back(r.cell);
    std::vector<Course> out;
    for (int cell : cells)
        for (auto &r : v)
            if (r.cell == cell) {
                deadline.check();
                bool dead = false;
                for (auto &a : v)
                    if (dominates(a, r)) {
                        dead = true;
                        break;
                    }
                if (!dead)
                    out.push_back(r);
            }
    return out;
}
std::vector<Course> diverse(const Case &c, const Travel &travel, std::vector<Course> v, int limit,
                            bool future, const Deadline &deadline) {
    auto better = [&](const Course &a, const Course &b) { return quality(c, a) > quality(c, b); };
    if ((int)v.size() <= limit) {
        std::stable_sort(v.begin(), v.end(), better);
        return v;
    }
    struct Group {
        uint64_t sig;
        std::vector<int> endpoints;
        std::map<int, std::vector<Course>> queues;
        std::vector<Course> sequence;
        size_t offset = 0;
    };
    std::vector<Group> groups;
    for (auto &r : v) {
        deadline.check();
        uint64_t sig = r.series;
        if (future)
            for (size_t s = 0; s < c.input.spots.size(); ++s)
                if (!(r.mask & (1ULL << s)) && travel.distance[r.cell][c.input.spots[s].cell_id] <=
                                                   c.input.day_steps - r.elapsed)
                    sig |= c.spot_series[s];
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const Group &g) { return g.sig == sig; });
        if (it == groups.end()) {
            groups.push_back(Group{});
            it = groups.end() - 1;
            it->sig = sig;
        }
        if (!it->queues.count(r.cell))
            it->endpoints.push_back(r.cell);
        it->queues[r.cell].push_back(r);
    }
    for (auto &g : groups) {
        for (auto &p : g.queues)
            std::stable_sort(p.second.begin(), p.second.end(), better);
        while (!g.endpoints.empty()) {
            auto order = g.endpoints;
            std::stable_sort(order.begin(), order.end(),
                             [&](int a, int b) { return better(g.queues[a][0], g.queues[b][0]); });
            for (int e : order) {
                auto &q = g.queues[e];
                g.sequence.push_back(q[0]);
                q.erase(q.begin());
                if (q.empty())
                    g.endpoints.erase(std::find(g.endpoints.begin(), g.endpoints.end(), e));
            }
        }
    }
    std::vector<int> sigs(groups.size());
    std::iota(sigs.begin(), sigs.end(), 0);
    std::stable_sort(sigs.begin(), sigs.end(), [&](int a, int b) {
        return better(groups[a].sequence[0], groups[b].sequence[0]);
    });
    if ((int)sigs.size() > limit) {
        std::vector<int> chosen{sigs[0]};
        sigs.erase(sigs.begin());
        while (!sigs.empty() && (int)chosen.size() < limit) {
            deadline.check();
            auto rank = [&](int i) {
                int h = 100;
                for (int j : chosen)
                    h = std::min(h, pop(groups[i].sig ^ groups[j].sig));
                return std::make_pair(h, quality(c, groups[i].sequence[0]));
            };
            auto best = std::max_element(sigs.begin(), sigs.end(),
                                         [&](int a, int b) { return rank(a) < rank(b); });
            chosen.push_back(*best);
            sigs.erase(best);
        }
        sigs = chosen;
    }
    std::vector<Course> out;
    while ((int)out.size() < limit) {
        auto available = sigs;
        available.erase(
            std::remove_if(available.begin(), available.end(),
                           [&](int s) { return groups[s].offset >= groups[s].sequence.size(); }),
            available.end());
        if (available.empty())
            break;
        std::stable_sort(available.begin(), available.end(), [&](int a, int b) {
            return better(groups[a].sequence[groups[a].offset],
                          groups[b].sequence[groups[b].offset]);
        });
        for (int s : available) {
            out.push_back(groups[s].sequence[groups[s].offset++]);
            if ((int)out.size() == limit)
                break;
        }
    }
    return out;
}
} // namespace
Pool generate_pool(const Case &c, const Travel &travel, int patrol, int limit,
                   const Deadline &deadline) {
    if (limit <= 0)
        throw std::invalid_argument("limit must be positive");
    Pool pool;
    pool.patrol = patrol;
    auto select_diverse = [&](std::vector<Course> input, bool future) {
        SecondsScope timer(pool.diverse_seconds);
        return diverse(c, travel, std::move(input), limit, future, deadline);
    };
    using LayerKey = std::tuple<int, uint64_t, int>;
    std::vector<std::vector<Course>> layers(c.input.day_steps + 1);
    std::vector<std::map<LayerKey, size_t>> indices(layers.size());
    Course initial;
    initial.cell = c.input.initial_state.agents[patrol].cell_id;
    initial.fuel = std::max(c.input.agents[patrol].fuel_capacity, 2 * c.input.day_steps);
    initial.patrol = patrol;
    layers[0].push_back(initial);
    pool.generated = 1;
    std::vector<Course> output;
    for (int elapsed = 0; elapsed <= c.input.day_steps; ++elapsed) {
        deadline.check();
        auto active = frontier(layers[elapsed], deadline);
        if ((int)active.size() > limit)
            active = select_diverse(std::move(active), true);
        output.insert(output.end(), active.begin(), active.end());
        if ((int)output.size() > limit)
            output = select_diverse(std::move(output), false);
        for (auto &r : active) {
            int duration = c.tau[r.cell], burn = c.burn[r.cell];
            if (elapsed + duration > c.input.day_steps || r.fuel < burn)
                continue;
            for (int d = 0; d < 6; ++d) {
                int dest = c.neighbors[r.cell][d];
                if (dest < 0)
                    continue;
                ++pool.generated;
                Course n = r;
                n.elapsed = elapsed + duration;
                n.cell = dest;
                n.fuel -= burn;
                n.actions.push_back(d);
                // Q6: the source is still occupied at the first reflection of a
                // multi-step move. In particular a later-day starting Spot is acquired.
                int source_spot = c.spot_at[r.cell];
                if (!c.legacy_contacts && duration > 1 && source_spot >= 0 &&
                    c.input.initial_state.spot_stock[source_spot] > 0 &&
                    !c.input.initial_state.agents[patrol].visited_today.count(
                        c.input.spots[source_spot].id))
                    n.mask |= 1ULL << source_spot;
                int s = c.spot_at[dest];
                if (s >= 0 && c.input.initial_state.spot_stock[s] > 0 &&
                    !c.input.initial_state.agents[patrol].visited_today.count(c.input.spots[s].id))
                    n.mask |= 1ULL << s;
                n.series = c.series(n.mask);
                auto k = LayerKey{dest, n.mask, n.fuel};
                auto &ix = indices[n.elapsed];
                auto &layer = layers[n.elapsed];
                auto it = ix.find(k);
                if (it == ix.end()) {
                    ix[k] = layer.size();
                    layer.push_back(std::move(n));
                } else if (quality(c, n) > quality(c, layer[it->second]))
                    layer[it->second] = std::move(n);
            }
        }
    }
    pool.movement = select_diverse(frontier(output, deadline), false);
    std::vector<std::pair<std::vector<int>, bool>> raw{{{-c.input.day_steps}, true}};
    for (auto &r : pool.movement) {
        auto actions = r.actions;
        int remaining = c.input.day_steps - r.elapsed;
        if (remaining)
            actions.push_back(-remaining);
        raw.push_back({actions, false});
        if (remaining && !r.actions.empty()) {
            actions = r.actions;
            actions.insert(actions.begin(), -remaining);
            raw.push_back({actions, false});
        }
    }
    std::set<std::vector<int>> seen;
    std::vector<Course> profiles;
    for (auto &entry : raw) {
        deadline.check();
        if (!seen.insert(entry.first).second)
            continue;
        auto p = profile(c, patrol, entry.first);
        p.mandatory = entry.second;
        profiles.push_back(std::move(p));
    }
    std::stable_sort(profiles.begin(), profiles.end(), [](const Course &a, const Course &b) {
        return std::make_tuple(pop(a.series), pop(a.mask), -(int)a.requirements.size(), -a.burn,
                               a.cell) > std::make_tuple(pop(b.series), pop(b.mask),
                                                         -(int)b.requirements.size(), -b.burn,
                                                         b.cell);
    });
    std::set<std::pair<uint64_t, Formula>> effects;
    std::vector<Course> mandatory, optional;
    for (auto &p : profiles)
        if (effects.insert({p.mask, p.requirements}).second) {
            if (p.mandatory)
                mandatory.push_back(p);
            else if ((int)optional.size() < limit)
                optional.push_back(p);
        }
    pool.courses = mandatory;
    pool.courses.insert(pool.courses.end(), optional.begin(), optional.end());
    return pool;
}
} // namespace contest_solver
