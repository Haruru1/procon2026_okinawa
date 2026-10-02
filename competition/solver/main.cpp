#include "core.hpp"
#include "supply.hpp"
#include "solver.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
using namespace contest_solver;
int main(int argc, char **argv) {
    try {
        bool plan_only = false;
        std::string input_path;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--plan-only")
                plan_only = true;
            else if (arg == "--help") {
                std::cout << "Usage: solver [case.json] [--plan-only]\nJSON on stdin when "
                             "case.json is omitted.\n";
                return 0;
            } else if (input_path.empty())
                input_path = arg;
            else
                throw std::invalid_argument("unexpected argument");
        }
        Json request;
        if (!input_path.empty()) {
            std::ifstream f(input_path);
            if (!f)
                throw std::runtime_error("cannot open input");
            f >> request;
        } else
            std::cin >> request;
        Case c(request.contains("case") ? request.at("case") : request,
               request.value("legacy_contacts", false));
        Deadline deadline;
        deadline.seconds = request.value("seconds", 60.0);
        auto mode = request.value("mode", std::string("solve"));
        auto filter_engine = request.value("filter_engine", std::string("indexed"));
        if (filter_engine != "interned" && filter_engine != "legacy" && filter_engine != "indexed")
            throw std::invalid_argument("unknown filter_engine");
        bool interned = filter_engine != "legacy", indexed = filter_engine == "indexed";
        GhrccOptions options;
        options.profile = request.value("ghrcc_profile", false);
        options.capture_call = request.value("capture_ghrcc_call", uint64_t(0));
        options.local_seconds = request.value("local_ghrcc_seconds", mode == "solve" ? 0.5 : 0.0);
        options.local_checks = request.value("local_ghrcc_checks", int64_t(-1));
        options.proved_wait_fallback = request.value("proved_wait_fallback", true);
        options.test_cancel_call = request.value("test_cancel_ghrcc_call", uint64_t(0));
        if (!std::isfinite(options.local_seconds) || options.local_seconds < 0)
            throw std::invalid_argument("local_ghrcc_seconds must be finite and nonnegative");
        if (mode == "solve") {
            double requested_seconds = deadline.seconds;
            // Measured cleanup overrun up to 0.513s. Reserve 1s at a 60s budget.
            double reserve = options.local_seconds > 0
                                 ? std::min(1.0, .02 * std::max(0.0, requested_seconds))
                                 : 0;
            reserve = request.value("finalization_reserve_seconds", reserve);
            if (!std::isfinite(reserve) || reserve < 0)
                throw std::invalid_argument("invalid finalization reserve");
            deadline.seconds = std::max(0.0, requested_seconds - reserve);
            auto result =
                solve_day(c, request.value("limit", 50), deadline,
                          request.value("detailed_profile", false), interned, indexed, options);
            result.metrics["search_deadline_seconds"] = deadline.seconds;
            result.metrics["requested_seconds"] = requested_seconds;
            result.metrics["finalization_reserve_seconds"] = reserve;
            result.metrics["search_overrun_seconds"] = result.metrics["deadline_overrun"];
            result.metrics["deadline_overrun"] =
                std::max(0.0, deadline.elapsed() - requested_seconds);
            std::cout
                << (plan_only ? Json{{"actions", result.plan.actions}} : result_json(result)).dump()
                << '\n';
            return 0;
        }
        if (mode == "replay") {
            DailyPlan plan;
            plan.actions = request.at("plan").at("actions").get<std::vector<std::vector<int>>>();
            std::cout << replay_json(RuleReplay().runDailyPlan(c.input, plan)).dump()
                      << '\n';
            return 0;
        }
        if (mode == "profile") {
            std::cout << course_json(profile(c, request.at("patrol"),
                request.at("actions").get<std::vector<int>>())).dump() << '\n';
            return 0;
        }
        Travel travel(c, deadline);
        if (mode == "supply" || mode == "filter") {
            SupplyChecker checker(c, travel, deadline, request.value("detailed_profile", false),
                                  interned, indexed, options);
            Json answers = Json::array();
            for (const auto &query : request.at("queries")) {
                deadline.remaining_checks = query.value("check_budget", int64_t(-1));
                Formula f;
                if (query.contains("formula"))
                    f = canonical(query.at("formula").get<Formula>());
                else
                    for (const auto &p : query.at("patrol_actions")) {
                        auto r =
                            profile(c, p.at("patrol"), p.at("actions").get<std::vector<int>>());
                        f = checker.extend(f, r.requirements);
                    }
                if (mode == "filter") {
                    std::string reason;
                    bool timeout = false;
                    try {
                        deadline.check();
                        reason = checker.filter(f);
                    } catch (const Timeout &) {
                        timeout = true;
                    }
                    answers.push_back({{"reason", reason},
                                       {"timeout", timeout},
                                       {"status", reason.empty() ? "UNKNOWN" : "INFEASIBLE"},
                                       {"pair_cache_entries", checker.pair_cache_entries()}});
                    continue;
                }
                auto r = checker.check(f, query.value("local_seconds", -1.0),
                                       query.value("local_checks", int64_t(-2)));
                Json answer = {{"status", r.status == Status::Unknown    ? "UNKNOWN"
                                          : r.status == Status::Feasible ? "FEASIBLE"
                                                                         : "INFEASIBLE"},
                               {"filter_reason", r.filter_reason},
                               {"local_timeout", r.local_timeout},
                               {"formula", f},
                               {"chains", r.chains},
                               {"states", r.states},
                               {"transitions", r.transitions},
                               {"cache_entries", checker.cache.size()}};
                if (r.status == Status::Feasible)
                    answer["supply_actions"] = checker.actions(r);
                answers.push_back(answer);
            }
            std::cout << Json{{"answers", answers},
                              {"calls", checker.calls},
                              {"cache_hits", checker.hits},
                              {"profile", checker.profile_json()}}
                             .dump()
                      << '\n';
            return 0;
        }
        Json result;
        result["distances"] = travel.distance;
        result["pools"] = Json::array();
        for (int p : c.patrols) {
            auto pool = generate_pool(c, travel, p, request.value("limit", 50), deadline);
            Json row = {{"patrol", p},
                        {"generated", pool.generated},
                        {"movement", Json::array()},
                        {"courses", Json::array()}};
            for (auto &r : pool.movement)
                row["movement"].push_back(course_json(r, false));
            for (auto &r : pool.courses)
                row["courses"].push_back(course_json(r));
            result["pools"].push_back(row);
        }
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    } catch (const Timeout &) {
        std::cerr << "audit timeout\n";
        return 2;
    }
}
