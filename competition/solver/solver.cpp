#include "solver.hpp"

namespace contest_solver {
namespace {
SolveResult fallback(const Case &c) {
    SolveResult r;
    r.plan.actions.assign(c.input.agents.size(), {-c.input.day_steps});
    r.replay = RuleReplay().runDailyPlan(c.input, r.plan);
    if (!r.replay.valid)
        throw std::invalid_argument("invalid one-day input: " + r.replay.error.message);
    r.objective = replay_objective(c, r.replay);
    return r;
}
std::vector<int> add(const Case &c, const std::vector<int> &counts, const Course &course) {
    auto out = counts;
    for (size_t s = 0; s < out.size(); ++s)
        out[s] = std::min(c.input.initial_state.spot_stock[s],
                          out[s] + int(bool(course.mask & (1ULL << s))));
    return out;
}
Objective upper(const Case &c, const std::vector<std::vector<Course>> &pools, int depth,
                const std::vector<int> &counts, const Deadline &deadline) {
    auto current = c.objective(counts);
    std::set<int> acquired, global, day;
    int independent_global = 0, independent_day = 0, balls = 0, residual_total = 0;
    for (size_t s = 0; s < counts.size(); ++s) {
        if (counts[s])
            acquired.insert(c.input.spots[s].series);
        residual_total += c.input.initial_state.spot_stock[s] - counts[s];
    }
    for (size_t p = depth; p < pools.size(); ++p) {
        int best_g = 0, best_d = 0, best_b = 0;
        for (const auto &course : pools[p]) {
            deadline.check();
            std::set<int> gs, ds;
            int bs = 0;
            for (size_t s = 0; s < counts.size(); ++s)
                if ((course.mask & (1ULL << s)) &&
                    counts[s] < c.input.initial_state.spot_stock[s]) {
                    ++bs;
                    int b = c.input.spots[s].series;
                    if (!acquired.count(b)) {
                        if (!c.initial_global.count(b))
                            gs.insert(b);
                        if (!c.initial_daily.count(b))
                            ds.insert(b);
                    }
                }
            global.insert(gs.begin(), gs.end());
            day.insert(ds.begin(), ds.end());
            best_g = std::max(best_g, (int)gs.size());
            best_d = std::max(best_d, (int)ds.size());
            best_b = std::max(best_b, bs);
        }
        independent_global += best_g;
        independent_day += best_d;
        balls += best_b;
    }
    return {current[0] + std::min((int)global.size(), independent_global),
            current[1] + std::min((int)day.size(), independent_day),
            current[2] + std::min(residual_total, balls)};
}
} // namespace
SolveResult solve_team(const Case &c, const Travel &travel, const std::vector<Pool> &pools,
                       const Deadline &deadline, bool detailed, bool interned, bool indexed,
                       GhrccOptions options) {
    auto team_start = Deadline::Clock::now();
    SolveResult best = fallback(c);
    SupplyChecker checker(c, travel, deadline, detailed, interned, indexed, options);
    std::vector<std::vector<Course>> working;
    for (const auto &p : pools)
        working.push_back(p.courses);
    auto wait_selection = [&]() {
        std::vector<int> indices;
        for (const auto &p : working) {
            auto it = std::find_if(p.begin(), p.end(), [&](const Course &r) {
                return r.actions == std::vector<int>{-c.input.day_steps};
            });
            if (it == p.end())
                throw std::logic_error("missing all-wait");
            indices.push_back((int)(it - p.begin()));
        }
        return indices;
    };
    best.selection = wait_selection();
    for (size_t i = 0; i < working.size(); ++i)
        best.selected.push_back(working[i][best.selection[i]]);
    // Resolve the empty all-wait formula without the search deadline.
    Deadline bootstrap_deadline;
    bootstrap_deadline.seconds = std::numeric_limits<double>::infinity();
    SupplyChecker bootstrap(c, travel, bootstrap_deadline);
    auto best_supply = bootstrap.check({});
    checker.cache[{}] = best_supply;
    checker.requests = bootstrap.requests;
    checker.calls = bootstrap.calls;
    checker.filter_calls = bootstrap.filter_calls;
    checker.states = bootstrap.states;
    checker.transitions = bootstrap.transitions;
    checker.dp_seconds = bootstrap.dp_seconds;
    checker.filter_seconds = bootstrap.filter_seconds;
    // Scheduling state is not a feasibility cache: membership means "not proved".
    std::set<Formula> unresolved;
    uint64_t unknown_reused = 0, deferred_count = 0, retry_count = 0, retry_feasible = 0,
             retry_infeasible = 0, retry_unknown = 0, deferred_peak = 0;
    struct Deferred {
        Objective objective;
        std::vector<int> selection;
        std::vector<Course> selected;
        unsigned attempts = 0;
        uint64_t sequence = 0;
    };
    std::map<Formula, Deferred> deferred;
    Deferred reserve;
    SupplyResult reserve_supply;
    reserve.objective = best.objective;
    uint64_t reserve_updates = 0;
    double reserve_replay_seconds = 0, retry_seconds = 0;
    auto query = [&](const Formula &formula) {
        deadline.check();
        if (unresolved.count(formula)) {
            ++unknown_reused;
            SupplyResult pending;
            pending.status = Status::Unknown;
            pending.local_timeout = true;
            return pending;
        }
        auto result = checker.check(formula);
        if (result.status == Status::Unknown) {
            if (!result.local_timeout)
                throw Timeout{};
            unresolved.insert(formula);
        }
        return result;
    };
    auto adopt = [&](Objective objective, const std::vector<int> &selection,
                     const std::vector<Course> &selected, const SupplyResult &decision,
                     bool retry) {
        if (decision.status != Status::Feasible)
            throw std::logic_error("cannot adopt an unproved supply plan");
        if (objective <= best.objective)
            return;
        best.objective = objective;
        best.selection = selection;
        best.selected = selected;
        best_supply = decision;
        ++best.updates;
        best.improvements.push_back({{"seconds", deadline.elapsed()},
                                     {"node", best.nodes},
                                     {"objective", best.objective},
                                     {"retry", retry}});
    };
    auto activate_reserve = [&]() {
        if (reserve.objective <= best.objective)
            return;
        SecondsScope timer(reserve_replay_seconds);
        // This is a different, completed pool combination, not the UNKNOWN candidate.
        DailyPlan plan;
        plan.actions.assign(c.input.agents.size(), {-c.input.day_steps});
        for (const auto &r : reserve.selected)
            plan.actions[r.patrol] = r.actions;
        auto supplies = checker.actions(reserve_supply);
        for (size_t i = 0; i < c.supplies.size(); ++i)
            plan.actions[c.supplies[i]] = supplies[i];
        auto replay = RuleReplay().runDailyPlan(c.input, plan);
        if (!replay.valid || replay_objective(c, replay) != reserve.objective)
            throw std::logic_error("proved prefix plus all-wait completion replay mismatch");
        adopt(reserve.objective, reserve.selection, reserve.selected, reserve_supply, false);
        best.improvements.back()["source"] = "proved_prefix_wait_completion";
        ++reserve_updates;
    };
    try {
        for (auto &pool : working) {
            std::vector<Course> kept;
            for (const auto &course : pool) {
                deadline.check();
                auto decision = query(course.requirements);
                if (decision.status != Status::Infeasible)
                    kept.push_back(course);
                else
                    ++best.individual_removed;
            }
            pool = std::move(kept);
        }
        best.selection = wait_selection();
        best.selected.clear();
        for (size_t i = 0; i < working.size(); ++i)
            best.selected.push_back(working[i][best.selection[i]]);
        std::vector<std::vector<int>> static_ranked;
        std::vector<int> frequency(c.series_values.size());
        for (const auto &pool : working) {
            std::vector<int> indices(pool.size());
            std::iota(indices.begin(), indices.end(), 0);
            std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
                return std::make_tuple(pop(pool[a].series), pop(pool[a].mask),
                                       -(int)pool[a].requirements.size()) >
                       std::make_tuple(pop(pool[b].series), pop(pool[b].mask),
                                       -(int)pool[b].requirements.size());
            });
            static_ranked.push_back(indices);
            uint64_t mask = 0;
            for (auto &r : pool)
                mask |= r.series;
            for (size_t s = 0; s < frequency.size(); ++s)
                frequency[s] += bool(mask & (1ULL << s));
        }
        std::map<std::pair<int, std::vector<int>>, std::vector<int>> ranking_cache;
        using Interface = std::vector<std::pair<int, Formula>>;
        std::vector<std::map<Interface, std::vector<std::vector<int>>>> frontiers(working.size() +
                                                                                  1);
        std::vector<Course> selected;
        std::vector<int> selection;
        double next_retry_at = deadline.elapsed() + 8 * options.local_seconds;
        auto retry_one = [&]() {
            deadline.check();
            for (auto it = deferred.begin(); it != deferred.end();) {
                if (it->second.objective <= best.objective)
                    it = deferred.erase(it);
                else
                    ++it;
            }
            if (deferred.empty())
                return;
            auto chosen = deferred.begin();
            for (auto it = deferred.begin(); it != deferred.end(); ++it) {
                deadline.check();
                const auto &a = it->second, &b = chosen->second;
                if (a.objective > b.objective ||
                    (a.objective == b.objective && std::make_pair(a.attempts, a.sequence) <
                                                       std::make_pair(b.attempts, b.sequence)))
                    chosen = it;
            }
            auto &pending = chosen->second;
            double budget =
                options.local_seconds > 0
                    ? options.local_seconds * (1ULL << std::min(10U, ++pending.attempts))
                    : 0;
            ++retry_count;
            SecondsScope timer(retry_seconds);
            auto decision = checker.check(chosen->first, budget, -1);
            if (decision.status == Status::Unknown) {
                ++retry_unknown;
                if (!decision.local_timeout)
                    throw Timeout{};
                return;
            }
            unresolved.erase(chosen->first);
            if (decision.status == Status::Feasible) {
                ++retry_feasible;
                adopt(pending.objective, pending.selection, pending.selected, decision, true);
            } else
                ++retry_infeasible;
            deferred.erase(chosen);
        };
        auto rank = [&](int depth, const std::vector<int> &counts) {
            if (depth >= 3)
                return static_ranked[depth];
            auto key = std::make_pair(depth, counts);
            auto it = ranking_cache.find(key);
            if (it != ranking_cache.end())
                return it->second;
            uint64_t acquired = 0;
            for (size_t s = 0; s < counts.size(); ++s)
                if (counts[s])
                    acquired |= c.spot_series[s];
            auto score = [&](int i) {
                const auto &r = working[depth][i];
                auto next = add(c, counts, r);
                int rarity = 0, wasted = 0, flex = 0;
                uint64_t newly = r.series & ~acquired;
                for (size_t s = 0; s < frequency.size(); ++s)
                    if (newly & (1ULL << s))
                        rarity += (int)working.size() + 1 - frequency[s];
                for (size_t s = 0; s < counts.size(); ++s)
                    wasted += bool(r.mask & (1ULL << s)) &&
                              counts[s] >= c.input.initial_state.spot_stock[s];
                for (auto &g : r.requirements)
                    flex += (int)g.size();
                bool no_refill = r.burn <= c.input.initial_state.agents[r.patrol].fuel;
                return std::make_tuple(c.objective(next), rarity, -wasted, int(no_refill),
                                       -(int)r.requirements.size(), flex, -r.burn, -r.elapsed, -i);
            };
            std::vector<int> indices(working[depth].size());
            std::iota(indices.begin(), indices.end(), 0);
            std::stable_sort(indices.begin(), indices.end(),
                             [&](int a, int b) { return score(a) > score(b); });
            ranking_cache[key] = indices;
            return indices;
        };
        std::function<void(int, const std::vector<int> &, const Formula &)> visit;
        visit = [&](int depth, const std::vector<int> &counts, const Formula &formula) {
            deadline.check();
            ++best.nodes;
            if (options.proved_wait_fallback && options.local_seconds > 0 &&
                c.objective(counts) > reserve.objective) {
                auto proof = checker.cache.find(formula);
                if (proof != checker.cache.end() && proof->second.status == Status::Feasible) {
                    reserve = Deferred{c.objective(counts), selection, selected, 0, 0};
                    auto completed_counts = counts;
                    auto waits = wait_selection();
                    for (size_t i = selected.size(); i < working.size(); ++i) {
                        reserve.selection.push_back(waits[i]);
                        reserve.selected.push_back(working[i][waits[i]]);
                        completed_counts = add(c, completed_counts, working[i][waits[i]]);
                    }
                    reserve.objective = c.objective(completed_counts);
                    reserve_supply = proof->second;
                }
            }
            if (checker.local_timeouts)
                activate_reserve();
            if (!deferred.empty() && deadline.elapsed() >= next_retry_at &&
                retry_seconds <=
                    .1 * std::chrono::duration<double>(Deadline::Clock::now() - team_start)
                             .count()) {
                retry_one();
                next_retry_at = deadline.elapsed() + 8 * options.local_seconds;
            }
            Interface interface;
            for (const auto &r : selected)
                interface.push_back({r.patrol, r.requirements});
            std::sort(interface.begin(), interface.end());
            auto &frontier = frontiers[depth][interface];
            if (std::find(frontier.begin(), frontier.end(), counts) != frontier.end()) {
                ++best.merges;
                return;
            }
            auto dominates = [](const std::vector<int> &a, const std::vector<int> &b) {
                for (size_t i = 0; i < a.size(); ++i)
                    if (a[i] < b[i])
                        return false;
                return true;
            };
            if (std::any_of(frontier.begin(), frontier.end(),
                            [&](const auto &x) { return dominates(x, counts); })) {
                ++best.dominance;
                return;
            }
            auto end = std::remove_if(frontier.begin(), frontier.end(),
                                      [&](const auto &x) { return dominates(counts, x); });
            best.dominance += frontier.end() - end;
            frontier.erase(end, frontier.end());
            frontier.push_back(counts);
            if (upper(c, working, depth, counts, deadline) <= best.objective) {
                ++best.bound_prunes;
                return;
            }
            if (depth == (int)working.size()) {
                auto objective = c.objective(counts);
                if (objective <= best.objective)
                    return;
                auto decision = query(formula);
                if (decision.status == Status::Unknown) {
                    auto found = deferred.find(formula);
                    if (found == deferred.end()) {
                        deferred.emplace(
                            formula, Deferred{objective, selection, selected, 0, deferred_count++});
                        deferred_peak = std::max<uint64_t>(deferred_peak, deferred.size());
                    } else if (objective > found->second.objective) {
                        found->second.objective = objective;
                        found->second.selection = selection;
                        found->second.selected = selected;
                    }
                    return;
                }
                if (decision.status == Status::Infeasible) {
                    ++best.early_prunes;
                    return;
                }
                adopt(objective, selection, selected, decision, false);
                return;
            }
            for (int index : rank(depth, counts)) {
                deadline.check();
                const auto &course = working[depth][index];
                selection.push_back(index);
                selected.push_back(course);
                auto child = checker.extend(formula, course.requirements);
                int next_depth = depth + 1;
                bool has_requirements =
                    std::any_of(selected.begin(), selected.end(),
                                [](const Course &r) { return !r.requirements.empty(); });
                bool possible = true;
                if (has_requirements &&
                    (next_depth % 2 == 0 || next_depth == (int)working.size())) {
                    auto decision = query(child);
                    possible = decision.status != Status::Infeasible;
                    if (!possible)
                        ++best.early_prunes;
                }
                if (possible)
                    visit(next_depth, add(c, counts, course), child);
                selected.pop_back();
                selection.pop_back();
            }
        };
        visit(0, std::vector<int>(c.input.spots.size()), {});
        // Only proved complete plans are incumbents. Deferred formulas have no negative proof.
        while (!deferred.empty()) {
            retry_one();
        }
        best.completed = deferred.empty();
    } catch (const Timeout &) {
        best.completed = false;
        activate_reserve();
    }
    double team_seconds =
        std::chrono::duration<double>(Deadline::Clock::now() - team_start).count();
    auto restore_start = Deadline::Clock::now();
    best.supply_chains = best_supply.chains;
    auto supplies = checker.actions(best_supply);
    best.plan.actions.assign(c.input.agents.size(), {-c.input.day_steps});
    for (const auto &r : best.selected)
        best.plan.actions[r.patrol] = r.actions;
    for (size_t i = 0; i < c.supplies.size(); ++i)
        best.plan.actions[c.supplies[i]] = supplies[i];
    double restore_seconds =
        std::chrono::duration<double>(Deadline::Clock::now() - restore_start).count();
    auto replay_start = Deadline::Clock::now();
    best.replay = RuleReplay().runDailyPlan(c.input, best.plan);
    if (!best.replay.valid || replay_objective(c, best.replay) != best.objective) {
        auto diagnostic = result_json(best);
        diagnostic["replay_objective"] = replay_objective(c, best.replay);
        diagnostic["replay_error_detail"] = {{"step", best.replay.error.step},
                                             {"agent_id", best.replay.error.agent_id},
                                             {"message", best.replay.error.message}};
        throw std::logic_error("solver final official replay mismatch: " + diagnostic.dump());
    }
    double replay_seconds =
        std::chrono::duration<double>(Deadline::Clock::now() - replay_start).count();
    best.metrics = {{"team_inclusive_seconds", team_seconds},
                    {"team_exclusive_seconds",
                     std::max(0.0, team_seconds - checker.filter_seconds - checker.dp_seconds)},
                    {"restore_seconds", restore_seconds},
                    {"replay_seconds", replay_seconds},
                    {"filter_profile", checker.profile_json()},
                    {"refuel_requests", checker.requests},
                    {"ghrcc_calls", checker.calls},
                    {"ghrcc_states", checker.states},
                    {"ghrcc_transitions", checker.transitions},
                    {"cache_hits", checker.hits},
                    {"filter_calls", checker.filter_calls},
                    {"filter_rejects", checker.filter_rejects},
                    {"unknown", checker.timeouts},
                    {"local_unknown", checker.local_timeouts},
                    {"unknown_scheduling_reuse", unknown_reused},
                    {"deferred_count", deferred_count},
                    {"deferred_peak", deferred_peak},
                    {"deferred_remaining", deferred.size()},
                    {"retries", retry_count},
                    {"retry_feasible", retry_feasible},
                    {"retry_infeasible", retry_infeasible},
                    {"retry_unknown", retry_unknown},
                    {"retry_seconds", retry_seconds},
                    {"proved_wait_updates", reserve_updates},
                    {"proved_wait_replay_seconds", reserve_replay_seconds},
                    {"ghrcc_seconds", checker.dp_seconds},
                    {"filter_seconds", checker.filter_seconds}};
    return best;
}
SolveResult solve_day(const Case &c, int limit, const Deadline &deadline, bool detailed,
                      bool interned, bool indexed, GhrccOptions options) {
    auto best = fallback(c);
    double travel_seconds = 0, candidate_seconds = 0, diverse_seconds = 0;
    Json pool_stats = Json::array();
    try {
        deadline.check();
        Travel travel(c, deadline);
        travel_seconds = deadline.elapsed();
        auto start = Deadline::Clock::now();
        std::vector<Pool> pools;
        for (int patrol : c.patrols) {
            deadline.check();
            auto pool = generate_pool(c, travel, patrol, limit, deadline);
            diverse_seconds += pool.diverse_seconds;
            pool_stats.push_back({{"patrol", patrol},
                                  {"generated", pool.generated},
                                  {"movement_count", pool.movement.size()},
                                  {"final_count", pool.courses.size()}});
            pools.push_back(std::move(pool));
        }
        candidate_seconds = std::chrono::duration<double>(Deadline::Clock::now() - start).count();
        best = solve_team(c, travel, pools, deadline, detailed, interned, indexed, options);
    } catch (const Timeout &) {
        best.completed = false;
        candidate_seconds = std::max(0.0, deadline.elapsed() - travel_seconds);
    }
    best.metrics["travel_seconds"] = travel_seconds;
    best.metrics["candidate_seconds"] = candidate_seconds;
    best.metrics["diverse_seconds"] = diverse_seconds;
    best.metrics["candidate_exclusive_seconds"] =
        std::max(0.0, candidate_seconds - diverse_seconds);
    best.metrics["pools"] = pool_stats;
    best.metrics["elapsed_seconds"] = deadline.elapsed();
    best.metrics["deadline_overrun"] = std::max(0.0, deadline.elapsed() - deadline.seconds);
    return best;
}
Json result_json(const SolveResult &r) {
    Json courses = Json::array();
    for (const auto &p : r.selected) {
        auto j = course_json(p);
        j["patrol"] = p.patrol;
        courses.push_back(j);
    }
    return {{"plan", {{"actions", r.plan.actions}}},
            {"objective", r.objective},
            {"completed", r.completed},
            {"selection", r.selection},
            {"selected_courses", courses},
            {"supply_chains", r.supply_chains},
            {"replay", replay_json(r.replay)},
            {"nodes", r.nodes},
            {"bound_prunes", r.bound_prunes},
            {"early_refuel_prunes", r.early_prunes},
            {"individually_removed", r.individual_removed},
            {"exact_state_merges", r.merges},
            {"state_dominance_prunes", r.dominance},
            {"incumbent_updates", r.updates},
            {"improvements", r.improvements},
            {"metrics", r.metrics}};
}
} // namespace contest_solver
