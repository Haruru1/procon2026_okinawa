#pragma once
#include "core.hpp"
#include <unordered_map>

namespace contest_solver {
// Python integer mask semantics, including numeric ordering above 64 obligations.
struct Bits {
    std::vector<uint64_t> words;
    explicit Bits(size_t bits = 0) : words((bits + 63) / 64, 0) {}
    void set(size_t bit) {
        words.at(bit / 64) |= 1ULL << (bit % 64);
    }
    int count() const {
        int n = 0;
        for (auto w : words)
            n += pop(w);
        return n;
    }
    bool subset_of(const Bits &b) const {
        for (size_t i = 0; i < words.size(); ++i)
            if (words[i] & ~b.words[i])
                return false;
        return true;
    }
    Bits operator|(const Bits &b) const {
        Bits r = *this;
        for (size_t i = 0; i < words.size(); ++i)
            r.words[i] |= b.words[i];
        return r;
    }
    bool operator==(const Bits &b) const {
        return words == b.words;
    }
    bool operator<(const Bits &b) const {
        if (words.size() != b.words.size())
            return words.size() < b.words.size();
        return std::lexicographical_compare(words.rbegin(), words.rend(), b.words.rbegin(),
                                            b.words.rend());
    }
};
enum class Status { Feasible, Infeasible, Unknown };
struct GhrccOptions {
    bool profile = false;
    uint64_t capture_call = 0;
    // Measured ordinary max 0.387s; 0 disables local cancellation for parity audits.
    double local_seconds = 0;
    int64_t local_checks = -1; // deterministic test-only interruption inside the DP
    bool proved_wait_fallback = true;
    uint64_t test_cancel_call = 0;
};
struct DpProfile {
    double events_seconds = 0, reachability_seconds = 0, incidence_seconds = 0, supply_seconds = 0,
           dominance_seconds = 0, union_seconds = 0, witness_seconds = 0;
    uint64_t generated = 0, duplicate = 0, dominated = 0, transitions = 0, peak_frontier = 0,
             peak_union = 0;
    Json roots = Json::array(), layers = Json::array();
    Json json() const;
};
struct SupplyResult {
    Status status = Status::Infeasible;
    bool local_timeout = false;
    std::string filter_reason;
    std::vector<std::vector<Event>> chains;
    uint64_t states = 0, transitions = 0;
};
struct FilterProfile {
    bool detailed = false;
    uint64_t root_requests = 0, root_hits = 0, pair_requests = 0, pair_hits = 0,
             event_pair_checks = 0, clique_rank_checks = 0, clique_cell_checks = 0;
    double roots_seconds = 0, pairs_seconds = 0, clique_seconds = 0, matching_seconds = 0,
           pair_lookup_seconds = 0, pair_compute_seconds = 0, intern_seconds = 0;
    Json rows = Json::array();
};
struct SupplyChecker {
    const Case &c;
    const Travel &travel;
    const Deadline &deadline;
    std::map<Formula, SupplyResult> cache;
    std::map<Clause, unsigned> root_cache;
    std::map<std::pair<Clause, Clause>, unsigned> pair_cache;
    // IDs are injective within this checker: map equality compares the full Clause.
    std::map<Clause, uint32_t> clause_ids;
    std::vector<Clause> clause_table;
    std::unordered_map<uint64_t, unsigned> id_pair_cache;
    bool interned_pairs;
    bool indexed_pairs;
    // 0..255 are complete root masks; 256 means not computed, never infeasible.
    static constexpr uint16_t missing_pair = 256;
    static constexpr uint32_t indexed_id_limit = 8192;
    std::vector<std::vector<uint16_t>> indexed_rows;
    uint64_t indexed_entries = 0;
    uint64_t requests = 0, calls = 0, hits = 0, filter_calls = 0, filter_rejects = 0, states = 0,
             transitions = 0, timeouts = 0;
    double dp_seconds = 0, filter_seconds = 0;
    FilterProfile filter_profile;
    double dp_max_seconds = 0, dp_timeout_seconds = 0;
    Json slowest_dp;
    GhrccOptions options;
    DpProfile current_dp;
    Json dp_rows = Json::array(), captured_formula;
    uint64_t local_timeouts = 0;
    SupplyChecker(const Case &cc, const Travel &tt, const Deadline &dd, bool detailed = false,
                  bool interned = true, bool indexed = true, GhrccOptions opts = {})
        : c(cc), travel(tt), deadline(dd), interned_pairs(interned),
          indexed_pairs(indexed && interned && cc.supplies.size() <= 8) {
        filter_profile.detailed = detailed;
        options = opts;
    }
    Formula extend(const Formula &parent, const Formula &added) const;
    bool reaches(const Event &a, const Event &b) const;
    int next_arrival(const Event &a, int arrival, const Event &b) const;
    unsigned root_mask(const Clause &clause);
    uint32_t clause_id(const Clause &clause);
    size_t pair_cache_entries() const {
        return indexed_pairs    ? indexed_entries + id_pair_cache.size()
               : interned_pairs ? id_pair_cache.size()
                                : pair_cache.size();
    }
    unsigned pair_mask(const Clause &left, const Clause &right, uint32_t left_id = UINT32_MAX,
                       uint32_t right_id = UINT32_MAX);
    std::string filter(const Formula &formula);
    SupplyResult solve(const Formula &formula, const Deadline *limit = nullptr);
    SupplyResult check(const Formula &formula, double local_seconds = -1,
                       int64_t local_checks = -2);
    std::vector<std::vector<int>> actions(const SupplyResult &result) const;
    Json profile_json() const;
};
} // namespace contest_solver
