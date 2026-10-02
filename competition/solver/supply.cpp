#include "supply.hpp"

namespace contest_solver {
Formula SupplyChecker::extend(const Formula &parent, const Formula &added) const {
    Formula result = parent;
    for (const auto &g : added) {
        deadline.check();
        if (std::any_of(result.begin(), result.end(),
                        [&](const Clause &h) { return subset(h, g); }))
            continue;
        result.erase(std::remove_if(result.begin(), result.end(),
                                    [&](const Clause &h) { return subset(g, h); }),
                     result.end());
        result.push_back(g);
    }
    std::sort(result.begin(), result.end(), [](const Clause &a, const Clause &b) {
        return std::make_tuple(a.size(), a) < std::make_tuple(b.size(), b);
    });
    return result;
}
bool SupplyChecker::reaches(const Event &a, const Event &b) const {
    return a.first < b.first && next_arrival(a, 0, b) <= b.first;
}
int SupplyChecker::next_arrival(const Event &a, int arrival, const Event &b) const {
    if (c.legacy_contacts)
        return a.first + travel.distance[a.second][b.second];
    if (a.second == b.second)
        return arrival;
    // A contact need not be an action boundary. Depart before a.first, but
    // complete the first edge strictly afterwards so reflection still sees A.
    int departure = std::max(arrival, a.first - c.tau[a.second] + 1);
    return departure + travel.distance[a.second][b.second];
}
unsigned SupplyChecker::root_mask(const Clause &g) {
    ++filter_profile.root_requests;
    auto it = root_cache.find(g);
    if (it != root_cache.end()) {
        ++filter_profile.root_hits;
        return it->second;
    }
    unsigned mask = 0;
    for (size_t r = 0; r < c.supplies.size(); ++r)
        for (auto e : g) {
            deadline.check();
            if (travel.distance[c.input.initial_state.agents[c.supplies[r]].cell_id][e.second] <=
                e.first) {
                mask |= 1U << r;
                break;
            }
        }
    root_cache[g] = mask;
    return mask;
}
uint32_t SupplyChecker::clause_id(const Clause &clause) {
    auto found = clause_ids.find(clause);
    if (found != clause_ids.end())
        return found->second;
    if (clause_table.size() >= UINT32_MAX)
        throw std::overflow_error("too many exact clauses");
    auto id = static_cast<uint32_t>(clause_table.size());
    clause_ids.emplace(clause, id);
    clause_table.push_back(clause);
    return id;
}
unsigned SupplyChecker::pair_mask(const Clause &a, const Clause &b, uint32_t a_id, uint32_t b_id) {
    ++filter_profile.pair_requests;
    auto lookup_start =
        filter_profile.detailed ? Deadline::Clock::now() : Deadline::Clock::time_point{};
    std::pair<Clause, Clause> key;
    uint64_t id_key = 0;
    uint32_t high = 0, low = 0;
    bool use_row = false;
    bool hit = false;
    unsigned cached = 0;
    if (interned_pairs) {
        if (a_id == UINT32_MAX)
            a_id = clause_id(a);
        if (b_id == UINT32_MAX)
            b_id = clause_id(b);
        high = std::max(a_id, b_id);
        low = std::min(a_id, b_id);
        use_row = indexed_pairs && high < indexed_id_limit;
        if (use_row) {
            hit = high < indexed_rows.size() && low < indexed_rows[high].size() &&
                  indexed_rows[high][low] != missing_pair;
            if (hit)
                cached = indexed_rows[high][low];
        } else {
            id_key = (uint64_t(low) << 32) | high;
            auto it = id_pair_cache.find(id_key);
            hit = it != id_pair_cache.end();
            if (hit)
                cached = it->second;
        }
    } else {
        key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        auto it = pair_cache.find(key);
        hit = it != pair_cache.end();
        if (hit)
            cached = it->second;
    }
    if (filter_profile.detailed)
        filter_profile.pair_lookup_seconds +=
            std::chrono::duration<double>(Deadline::Clock::now() - lookup_start).count();
    if (hit) {
        ++filter_profile.pair_hits;
        return cached;
    }
    SecondsScope compute_timer(filter_profile.pair_compute_seconds);
    unsigned mask = 0;
    for (size_t r = 0; r < c.supplies.size(); ++r) {
        int start = c.input.initial_state.agents[c.supplies[r]].cell_id;
        bool found = false;
        for (auto x : a) {
            deadline.check();
            if (travel.distance[start][x.second] > x.first)
                continue;
            for (auto y : b) {
                if (filter_profile.detailed)
                    ++filter_profile.event_pair_checks;
                if (x == y || (x.first < y.first &&
                    next_arrival(x, travel.distance[start][x.second], y) <= y.first)) {
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        if (!found)
            for (auto x : b) {
                deadline.check();
                if (travel.distance[start][x.second] > x.first)
                    continue;
                for (auto y : a) {
                    if (filter_profile.detailed)
                        ++filter_profile.event_pair_checks;
                    if (x.first < y.first &&
                        next_arrival(x, travel.distance[start][x.second], y) <= y.first) {
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        if (found)
            mask |= 1U << r;
    }
    // Only completed relation values reach this insertion (Timeout unwinds above).
    if (use_row) {
        if (mask >= missing_pair)
            throw std::logic_error("indexed root mask overflow");
        if (high >= indexed_rows.size())
            indexed_rows.resize(high + 1);
        auto &row = indexed_rows[high];
        if (low >= row.size())
            row.resize(low + 1, missing_pair);
        row[low] = static_cast<uint16_t>(mask);
        ++indexed_entries;
    } else if (interned_pairs)
        id_pair_cache.emplace(id_key, mask);
    else
        pair_cache[key] = mask;
    return mask;
}
std::string SupplyChecker::filter(const Formula &formula) {
    ++filter_calls;
    if (formula.empty())
        return "";
    int n = (int)formula.size();
    std::vector<uint32_t> ids(n, UINT32_MAX);
    if (interned_pairs) {
        SecondsScope timer(filter_profile.intern_seconds);
        for (int i = 0; i < n; ++i) {
            deadline.check();
            ids[i] = clause_id(formula[i]);
        }
    }
    std::vector<unsigned> roots;
    {
        SecondsScope timer(filter_profile.roots_seconds);
        for (const auto &g : formula)
            roots.push_back(root_mask(g));
    }
    if (std::find(roots.begin(), roots.end(), 0U) != roots.end())
        return "unreachable";
    std::vector<std::vector<bool>> adjacency(n, std::vector<bool>(n));
    std::vector<int> degree(n);
    {
        SecondsScope timer(filter_profile.pairs_seconds);
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                deadline.check();
                if (!pair_mask(formula[i], formula[j], ids[i], ids[j])) {
                    adjacency[i][j] = adjacency[j][i] = true;
                    ++degree[i];
                    ++degree[j];
                }
            }
    }
    if (c.supplies.size() == 1 && *std::max_element(degree.begin(), degree.end()) > 0)
        return "pair_conflict";
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return std::make_pair(degree[a], -a) > std::make_pair(degree[b], -b);
    });
    std::set<std::vector<int>> cliques;
    {
        SecondsScope timer(filter_profile.clique_seconds);
        for (int start : order) {
            deadline.check();
            std::vector<int> clique{start};
            auto candidates = adjacency[start];
            while (std::find(candidates.begin(), candidates.end(), true) != candidates.end()) {
                deadline.check();
                int chosen = -1;
                std::tuple<int, int, int> best{-1, -1, 0};
                for (int v : order)
                    if (candidates[v]) {
                        if (filter_profile.detailed) {
                            ++filter_profile.clique_rank_checks;
                            filter_profile.clique_cell_checks += n;
                        }
                        int count = 0;
                        for (int j = 0; j < n; ++j)
                            count += candidates[j] && adjacency[v][j];
                        auto rank = std::make_tuple(count, degree[v], -v);
                        if (rank > best) {
                            chosen = v;
                            best = rank;
                        }
                    }
                clique.push_back(chosen);
                for (int j = 0; j < n; ++j)
                    candidates[j] = candidates[j] && adjacency[chosen][j];
                candidates[chosen] = false;
            }
            std::sort(clique.begin(), clique.end());
            cliques.insert(clique);
        }
    }
    SecondsScope matching_timer(filter_profile.matching_seconds);
    std::vector<std::vector<int>> sorted(cliques.begin(), cliques.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return std::make_tuple(a.size(), a) > std::make_tuple(b.size(), b);
    });
    for (const auto &clique : sorted) {
        deadline.check();
        if (clique.size() > c.supplies.size())
            return "fleet_shortage";
        std::vector<int> owner(c.supplies.size(), -1);
        std::function<bool(int, unsigned &)> augment = [&](int req, unsigned &seen) {
            for (size_t r = 0; r < c.supplies.size(); ++r)
                if ((roots[req] & (1U << r)) && !(seen & (1U << r))) {
                    seen |= 1U << r;
                    if (owner[r] < 0 || augment(owner[r], seen)) {
                        owner[r] = req;
                        return true;
                    }
                }
            return false;
        };
        for (int req : clique) {
            unsigned seen = 0;
            if (!augment(req, seen))
                return "fleet_shortage";
        }
    }
    return "";
}
namespace {
struct RootProfileScope {
    DpProfile &p;
    bool enabled;
    Deadline::Clock::time_point start = Deadline::Clock::now();
    double dominance, witness;
    uint64_t generated, duplicate, transitions, dominated;
    RootProfileScope(DpProfile &value, bool active)
        : p(value), enabled(active), dominance(p.dominance_seconds), witness(p.witness_seconds),
          generated(p.generated), duplicate(p.duplicate), transitions(p.transitions),
          dominated(p.dominated) {}
    ~RootProfileScope() {
        if (!enabled)
            return;
        auto &row = p.roots.back();
        row["seconds"] = std::chrono::duration<double>(Deadline::Clock::now() - start).count();
        row["dominance_seconds"] = p.dominance_seconds - dominance;
        row["witness_seconds"] = p.witness_seconds - witness;
        row["generated"] = p.generated - generated;
        row["duplicate"] = p.duplicate - duplicate;
        row["transitions"] = p.transitions - transitions;
        row["dominated"] = p.dominated - dominated;
    }
};
template <class Value> struct OrderedMasks {
    std::vector<std::pair<Bits, Value>> rows;
    std::map<Bits, size_t> index;
    void add(const Bits &mask, const Value &value) {
        if (index.emplace(mask, rows.size()).second)
            rows.push_back({mask, value});
    }
    const Value &at(const Bits &mask) const {
        return rows.at(index.at(mask)).second;
    }
};
std::vector<Bits> maximal(std::vector<Bits> masks, const Deadline &deadline, DpProfile &profile) {
    SecondsScope timer(profile.dominance_seconds);
    uint64_t comparisons = 0;
    std::sort(masks.begin(), masks.end(), [&](const Bits &a, const Bits &b) {
        if ((++comparisons & 1023) == 0)
            deadline.check();
        return a.count() != b.count() ? a.count() > b.count() : a < b;
    });
    std::vector<Bits> result;
    for (auto &mask : masks) {
        deadline.check();
        bool dominated = false;
        for (auto &sup : result) {
            deadline.check();
            if (mask.subset_of(sup)) {
                ++profile.dominated;
                dominated = true;
                break;
            }
        }
        if (!dominated)
            result.push_back(mask);
    }
    return result;
}
} // namespace
SupplyResult SupplyChecker::solve(const Formula &formula, const Deadline *limit) {
    const Deadline &deadline = limit ? *limit : this->deadline;
    auto &p = current_dp;
    SupplyResult result;
    size_t k = formula.size();
    Bits zero(k), full(k);
    for (size_t i = 0; i < k; ++i)
        full.set(i);
    std::set<Event> event_set;
    {
        SecondsScope timer(p.events_seconds);
        for (const auto &g : formula) {
            deadline.check();
            event_set.insert(g.begin(), g.end());
        }
    }
    std::vector<Event> events(event_set.begin(), event_set.end());
    int n = (int)events.size();
    std::vector<Bits> masks(n, Bits(k));
    {
        SecondsScope timer(p.incidence_seconds);
        for (int e = 0; e < n; ++e) {
            deadline.check();
            for (size_t g = 0; g < k; ++g)
                if (std::binary_search(formula[g].begin(), formula[g].end(), events[e]))
                    masks[e].set(g);
        }
    }
    std::vector<std::vector<int>> successors(n), predecessors(n);
    std::vector<int> degree(n);
    {
        SecondsScope timer(p.reachability_seconds);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                if ((j & 255) == 0)
                    deadline.check();
                if (reaches(events[i], events[j])) {
                    successors[i].push_back(j);
                    predecessors[j].push_back(i);
                    ++degree[j];
                }
            }
    }
    std::vector<int> order;
    for (int i = 0; i < n; ++i)
        if (!degree[i])
            order.push_back(i);
    for (size_t i = 0; i < order.size(); ++i)
        for (int j : successors[order[i]])
            if (!--degree[j])
                order.push_back(j);
    if ((int)order.size() != n)
        throw std::logic_error("non-DAG contact relation");
    struct Parent {
        int event;
        Bits mask;
        int arrival;
    };
    struct Root {
        OrderedMasks<std::vector<int>> chains;
        std::vector<Bits> maximal_masks;
    };
    std::vector<Root> profiles;
    for (int supply : c.supplies) {
        if (options.profile)
            p.roots.push_back({{"supply", supply}, {"events", Json::array()}});
        RootProfileScope root_profile(p, options.profile);
        std::vector<OrderedMasks<Parent>> parents(n);
        int start = c.input.initial_state.agents[supply].cell_id;
        {
            SecondsScope timer(p.supply_seconds);
            for (int e : order) {
                deadline.check();
                auto &row = parents[e];
                if (travel.distance[start][events[e].second] <= events[e].first) {
                    row.add(masks[e], {-1, zero, c.legacy_contacts ? events[e].first :
                                               travel.distance[start][events[e].second]});
                    ++p.generated;
                }
                for (int prev : predecessors[e])
                    for (auto &entry : parents[prev].rows) {
                        if ((++result.transitions & 1023) == 0)
                            deadline.check();
                        ++p.transitions;
                        int arrival = next_arrival(events[prev], entry.second.arrival, events[e]);
                        if (arrival > events[e].first)
                            continue;
                        if (c.legacy_contacts)
                            arrival = events[e].first;
                        auto before = row.rows.size();
                        auto mask = entry.first | masks[e];
                        auto found = row.index.find(mask);
                        if (found == row.index.end())
                            row.add(mask, {prev, entry.first, arrival});
                        else if (arrival < row.rows[found->second].second.arrival)
                            row.rows[found->second].second = {prev, entry.first, arrival};
                        ++p.generated;
                        p.duplicate += before == row.rows.size();
                        p.peak_frontier = std::max<uint64_t>(p.peak_frontier, row.rows.size());
                    }
                if (options.profile)
                    p.roots.back()["events"].push_back({{"event", e}, {"before", row.rows.size()}});
                if (row.rows.size() >= 2) {
                    std::vector<Bits> keys;
                    for (auto &p : row.rows)
                        keys.push_back(p.first);
                    std::vector<Bits> keep;
                    if (c.legacy_contacts) {
                        keep = maximal(std::move(keys), deadline, p);
                    } else {
                        SecondsScope timer(p.dominance_seconds);
                        std::sort(keys.begin(), keys.end(), [&](const Bits &a, const Bits &b) {
                            return std::make_tuple(-a.count(), row.at(a).arrival, a) <
                                   std::make_tuple(-b.count(), row.at(b).arrival, b);
                        });
                        for (const auto &mask : keys) {
                            deadline.check();
                            bool dominated = false;
                            for (size_t i = 0; i < keep.size(); ++i) {
                                if ((i & 255) == 0)
                                    deadline.check();
                                if (row.at(keep[i]).arrival <= row.at(mask).arrival &&
                                    mask.subset_of(keep[i])) {
                                    dominated = true;
                                    ++p.dominated;
                                    break;
                                }
                            }
                            if (!dominated)
                                keep.push_back(mask);
                        }
                    }
                    if (keep.size() != row.rows.size()) {
                        OrderedMasks<Parent> replacement;
                        for (auto &mask : keep)
                            replacement.add(mask, row.at(mask));
                        row = std::move(replacement);
                    }
                }
                if (options.profile)
                    p.roots.back()["events"].back()["after"] = row.rows.size();
            }
        }
        Root root;
        {
            SecondsScope timer(p.witness_seconds);
            root.chains.add(zero, {});
            for (int e : order) {
                deadline.check();
                result.states += parents[e].rows.size();
                for (auto &p : parents[e].rows)
                    if (!root.chains.index.count(p.first)) {
                        std::vector<int> chain;
                        int current = e;
                        Bits mask = p.first;
                        while (current >= 0) {
                            deadline.check();
                            chain.push_back(current);
                            auto parent = parents[current].at(mask);
                            current = parent.event;
                            mask = parent.mask;
                        }
                        std::reverse(chain.begin(), chain.end());
                        root.chains.add(p.first, chain);
                    }
            }
        }
        std::vector<Bits> keys;
        for (auto &p : root.chains.rows)
            keys.push_back(p.first);
        root.maximal_masks = maximal(std::move(keys), deadline, p);
        if (options.profile) {
            p.roots.back()["masks"] = root.chains.rows.size();
            p.roots.back()["maximal_masks"] = root.maximal_masks.size();
        }
        profiles.push_back(std::move(root));
    }
    std::vector<std::map<Bits, std::pair<Bits, Bits>>> decisions;
    std::set<Bits> previous{zero};
    ++result.states;
    {
        SecondsScope timer(p.union_seconds);
        for (const auto &root : profiles) {
            std::map<Bits, std::pair<Bits, Bits>> layer;
            for (const auto &prev : previous)
                for (const auto &exact : root.maximal_masks) {
                    if ((++result.transitions & 1023) == 0)
                        deadline.check();
                    ++p.transitions;
                    ++p.generated;
                    p.duplicate += !layer.emplace(prev | exact, std::make_pair(prev, exact)).second;
                    p.peak_union = std::max<uint64_t>(p.peak_union, layer.size());
                }
            previous.clear();
            for (auto &p : layer)
                previous.insert(p.first);
            result.states += layer.size();
            if (options.profile)
                p.layers.push_back(layer.size());
            decisions.push_back(std::move(layer));
        }
    }
    deadline.check();
    if (!previous.count(full))
        return result;
    SecondsScope witness_timer(p.witness_seconds);
    std::vector<std::vector<int>> reconstructed(c.supplies.size());
    Bits combined = full;
    for (int r = (int)profiles.size() - 1; r >= 0; --r) {
        auto decision = decisions[r].at(combined);
        reconstructed[r] = profiles[r].chains.at(decision.second);
        combined = decision.first;
    }
    std::set<int> used;
    Bits covered(k);
    for (size_t r = 0; r < reconstructed.size(); ++r) {
        std::vector<Event> chain;
        int start = c.input.initial_state.agents[c.supplies[r]].cell_id;
        Event last{0, start};
        int arrival = 0;
        for (int e : reconstructed[r])
            if (!c.legacy_contacts || used.insert(e).second) {
                arrival = next_arrival(last, arrival, events[e]);
                if (last.first >= events[e].first || arrival > events[e].first)
                    throw std::logic_error("invalid supply witness");
                chain.push_back(events[e]);
                covered = covered | masks[e];
                last = events[e];
            }
        result.chains.push_back(chain);
    }
    if (!(covered == full))
        throw std::logic_error("witness does not cover formula");
    result.status = Status::Feasible;
    return result;
}
SupplyResult SupplyChecker::check(const Formula &formula, double local_seconds,
                                  int64_t local_checks) {
    ++requests;
    auto it = cache.find(formula);
    if (it != cache.end()) {
        ++hits;
        return it->second;
    }
    try {
        deadline.check();
        auto start = Deadline::Clock::now();
        auto root_before = filter_profile.root_requests,
             root_hits_before = filter_profile.root_hits;
        auto pair_before = filter_profile.pair_requests,
             pair_hits_before = filter_profile.pair_hits;
        auto before_roots = filter_profile.roots_seconds,
             before_pairs = filter_profile.pairs_seconds;
        auto before_clique = filter_profile.clique_seconds,
             before_matching = filter_profile.matching_seconds;
        auto record = [&](const std::string &reason, bool timeout) {
            double elapsed = std::chrono::duration<double>(Deadline::Clock::now() - start).count();
            filter_seconds += elapsed;
            if (!filter_profile.detailed)
                return;
            std::set<Event> events;
            size_t alternatives = 0;
            for (const auto &g : formula) {
                alternatives += g.size();
                events.insert(g.begin(), g.end());
            }
            filter_profile.rows.push_back(
                {{"seconds", elapsed},
                 {"obligations", formula.size()},
                 {"contacts", events.size()},
                 {"alternatives", alternatives},
                 {"reason", reason},
                 {"timeout", timeout},
                 {"root_requests", filter_profile.root_requests - root_before},
                 {"root_hits", filter_profile.root_hits - root_hits_before},
                 {"pair_requests", filter_profile.pair_requests - pair_before},
                 {"pair_hits", filter_profile.pair_hits - pair_hits_before},
                 {"roots_seconds", filter_profile.roots_seconds - before_roots},
                 {"pairs_seconds", filter_profile.pairs_seconds - before_pairs},
                 {"clique_seconds", filter_profile.clique_seconds - before_clique},
                 {"matching_seconds", filter_profile.matching_seconds - before_matching}});
        };
        std::string reason;
        try {
            reason = filter(formula);
        } catch (const Timeout &) {
            record("UNKNOWN", true);
            throw;
        }
        record(reason, false);
        if (!reason.empty()) {
            ++filter_rejects;
            SupplyResult result;
            result.filter_reason = reason;
            cache.emplace(formula, result);
            return result;
        }
        ++calls;
        current_dp = {};
        if (calls == options.capture_call)
            captured_formula = formula;
        start = Deadline::Clock::now();
        Deadline local;
        local.parent = &deadline;
        double budget = local_seconds >= 0 ? local_seconds : options.local_seconds;
        local.seconds =
            budget > 0 ? std::min(budget, std::max(0.0, deadline.seconds - deadline.elapsed()) / 16)
                       : std::numeric_limits<double>::infinity();
        local.remaining_checks = local_checks >= -1 ? local_checks : options.local_checks;
        if (calls == options.test_cancel_call)
            local.remaining_checks = 0;
        bool limited = budget > 0 || local.remaining_checks >= 0;
        auto record_dp = [&](bool timeout) {
            double elapsed = std::chrono::duration<double>(Deadline::Clock::now() - start).count();
            dp_seconds += elapsed;
            if (timeout)
                dp_timeout_seconds += elapsed;
            if (options.profile) {
                auto row = current_dp.json();
                row["call"] = calls;
                row["seconds"] = elapsed;
                row["timeout"] = timeout;
                row["obligations"] = formula.size();
                row["local_budget_seconds"] =
                    limited && budget > 0 ? Json(local.seconds) : Json(nullptr);
                std::set<Event> events;
                for (const auto &g : formula)
                    events.insert(g.begin(), g.end());
                row["contacts"] = events.size();
                dp_rows.push_back(std::move(row));
            }
            if (elapsed > dp_max_seconds) {
                dp_max_seconds = elapsed;
                std::set<Event> events;
                for (const auto &g : formula)
                    events.insert(g.begin(), g.end());
                slowest_dp = {{"call", calls},
                              {"seconds", elapsed},
                              {"timeout", timeout},
                              {"obligations", formula.size()},
                              {"contacts", events.size()}};
            }
        };
        SupplyResult result;
        try {
            result = solve(formula, limited ? &local : nullptr);
        } catch (const Timeout &) {
            record_dp(true);
            throw;
        }
        record_dp(false);
        states += result.states;
        transitions += result.transitions;
        cache.emplace(formula, result);
        return result;
    } catch (const Timeout &timeout) {
        ++timeouts;
        local_timeouts += timeout.local;
        SupplyResult r;
        r.status = Status::Unknown;
        r.local_timeout = timeout.local;
        return r;
    }
}
Json SupplyChecker::profile_json() const {
    const auto &p = filter_profile;
    size_t indexed_bytes = indexed_rows.capacity() * sizeof(std::vector<uint16_t>);
    for (const auto &row : indexed_rows)
        indexed_bytes += row.capacity() * sizeof(uint16_t);
    return {{"root_requests", p.root_requests},
            {"root_hits", p.root_hits},
            {"pair_requests", p.pair_requests},
            {"pair_hits", p.pair_hits},
            {"root_cache_entries", root_cache.size()},
            {"pair_cache_entries", pair_cache_entries()},
            {"indexed_allocated_bytes", indexed_bytes},
            {"dp_max_seconds", dp_max_seconds},
            {"dp_timeout_seconds", dp_timeout_seconds},
            {"local_timeouts", local_timeouts},
            {"slowest_dp", slowest_dp},
            {"dp_rows", dp_rows},
            {"captured_formula", captured_formula},
            {"intern_seconds", p.intern_seconds},
            {"interned_clauses", clause_table.size()},
            {"engine", indexed_pairs    ? "indexed"
                       : interned_pairs ? "interned"
                                        : "legacy"},
            {"exact_cache_entries", cache.size()},
            {"roots_seconds", p.roots_seconds},
            {"pairs_seconds", p.pairs_seconds},
            {"clique_seconds", p.clique_seconds},
            {"matching_seconds", p.matching_seconds},
            {"pair_lookup_seconds", p.pair_lookup_seconds},
            {"pair_compute_seconds", p.pair_compute_seconds},
            {"event_pair_checks", p.event_pair_checks},
            {"clique_rank_checks", p.clique_rank_checks},
            {"clique_cell_checks", p.clique_cell_checks},
            {"rows", p.rows}};
}
Json DpProfile::json() const {
    return {{"events_seconds", events_seconds},
            {"reachability_seconds", reachability_seconds},
            {"incidence_seconds", incidence_seconds},
            {"supply_inclusive_seconds", supply_seconds},
            {"dominance_seconds", dominance_seconds},
            {"union_seconds", union_seconds},
            {"witness_seconds", witness_seconds},
            {"generated", generated},
            {"duplicate", duplicate},
            {"dominated", dominated},
            {"transitions", transitions},
            {"peak_frontier", peak_frontier},
            {"peak_union", peak_union},
            {"roots", roots},
            {"union_layers", layers}};
}
std::vector<std::vector<int>> SupplyChecker::actions(const SupplyResult &result) const {
    if (result.status != Status::Feasible || result.chains.size() != c.supplies.size())
        throw std::logic_error("no completed witness");
    std::vector<std::vector<int>> output;
    for (size_t r = 0; r < c.supplies.size(); ++r) {
        int cell = c.input.initial_state.agents[c.supplies[r]].cell_id, t = 0;
        std::vector<int> commands;
        // Prefer the historical schedule when all contacts can be action
        // boundaries. Otherwise reconstruct the earliest-arrival witness.
        bool boundary_schedule = true;
        Event last{0, cell};
        for (auto e : result.chains[r]) {
            boundary_schedule &= travel.distance[last.second][e.second] <= e.first - last.first;
            last = e;
        }
        int contact_time = 0;
        for (auto e : result.chains[r]) {
            auto path = travel.path(cell, e.second);
            int departure = boundary_schedule ? e.first - path.first :
                (cell == e.second ? t : std::max(t, contact_time - c.tau[cell] + 1));
            int slack = departure - t;
            if (slack < 0)
                throw std::logic_error("negative witness slack");
            if (slack)
                commands.push_back(-slack);
            commands.insert(commands.end(), path.second.begin(), path.second.end());
            t = departure + path.first;
            if (t > e.first)
                throw std::logic_error("late reflected-contact witness");
            contact_time = e.first;
            cell = e.second;
        }
        if (t < c.input.day_steps)
            commands.push_back(-(c.input.day_steps - t));
        output.push_back(std::move(commands));
    }
    return output;
}
} // namespace contest_solver
