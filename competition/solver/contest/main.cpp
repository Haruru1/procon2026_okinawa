#include "adapter.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace contest_solver;
namespace wire = contest_solver::contest;

int main(int argc, char **argv) {
    try {
        std::vector<std::string> files;
        std::string history, report, answer, kinds;
        bool offline = false, synthetic = false, inspect = false, shape = false, fallback = false;
        int64_t checks = -1;
        double budget = wire::solver_seconds;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (++i >= argc)
                    throw std::invalid_argument("missing value for " + arg);
                return std::string(argv[i]);
            };
            if (arg == "--help") {
                std::cout << "Usage: contest_solver map.json day.json [--history history.json]\n"
                             "  [--offline] [--report report.json] [--validate-answer answer.json]\n"
                             "  [--fallback-on-invalid] [--inspect] [--allow-synthetic]\n"
                             "  [--budget-seconds 0..55]: shorten the caller budget for a live match.\n"
                             "  --check-format: validate PDF JSON shape only (no solver/answer).\n"
                             "  map.json --kinds kinds.json: validate/output type selection array.\n"
                             "Stdout: official bare answer array; diagnostic/history reports are local only.\n"
                             "Live mode caps budget by endsAt; --offline uses archived inputs with seconds=55.\n";
                return 0;
            } else if (arg == "--history") history = value();
            else if (arg == "--report") report = value();
            else if (arg == "--validate-answer") answer = value();
            else if (arg == "--kinds") kinds = value();
            else if (arg == "--offline") offline = true;
            else if (arg == "--allow-synthetic") synthetic = true;
            else if (arg == "--inspect") inspect = true;
            else if (arg == "--check-format") shape = true;
            else if (arg == "--fallback-on-invalid") fallback = true;
            else if (arg == "--budget-seconds") {
                auto raw = value();
                size_t end = 0;
                budget = std::stod(raw, &end);
                if (end != raw.size()) throw std::invalid_argument("invalid budget-seconds");
            }
            else if (arg == "--test-check-budget") {
                auto raw = value();
                size_t end = 0;
                checks = std::stoll(raw, &end);
                if (end != raw.size() || checks < 0)
                    throw std::invalid_argument("invalid test check budget");
            } else if (arg.rfind("--", 0) == 0)
                throw std::invalid_argument("unknown option: " + arg);
            else files.push_back(arg);
        }
        if (files.empty() || files.size() > 2)
            throw std::invalid_argument("expected map.json and day.json; use --help");
        if (checks >= 0 && (!offline || !synthetic))
            throw std::invalid_argument("test cancellation requires --offline --allow-synthetic");
        if (fallback && answer.empty())
            throw std::invalid_argument("--fallback-on-invalid requires --validate-answer");
        if (!report.empty()) {
            const auto dest = std::filesystem::absolute(report).lexically_normal();
            auto inputs = files;
            for (const auto &s : {history, answer, kinds})
                if (!s.empty()) inputs.push_back(s);
            for (const auto &src : inputs) {
                const auto source = std::filesystem::absolute(src).lexically_normal();
                if (dest == source || (std::filesystem::exists(dest) && std::filesystem::equivalent(dest, source)))
                    throw std::invalid_argument("report must not overwrite an input file");
            }
        }
        const auto map = wire::parse_map(wire::read_json(files[0]));
        if (!kinds.empty()) {
            if (files.size() != 1 || !history.empty() || !answer.empty() || inspect || shape)
                throw std::invalid_argument("--kinds requires only map.json and kinds.json");
            if (!synthetic) wire::validate_contest_map(map);
            std::cout << wire::validate_kinds(wire::read_json(kinds), map.initial_positions.size()).dump() << '\n';
            return 0;
        }
        if (files.size() != 2)
            throw std::invalid_argument("day.json is required");
        if (int(inspect) + int(shape) + int(!answer.empty()) > 1)
            throw std::invalid_argument("choose exactly one of inspect/check-format/validate-answer");
        const auto daily = wire::read_json(files[1]);
        if (shape) {
            wire::validate_day_shape(daily, map.initial_positions.size());
            std::cout << Json{{"format_only", true}, {"map_config", map.raw}, {"daily", daily},
                               {"physical_validation_performed", false}}.dump() << '\n';
            return 0;
        }
        std::optional<Json> past;
        if (!history.empty()) past = wire::read_json(history);
        const auto input = wire::convert(map, daily, past, synthetic);
        if (inspect) {
            std::cout << Json{{"normalized", input.normalized}, {"brand_dictionary", map.brands},
                               {"day", input.day_index}, {"requested_seconds", wire::solver_seconds}}.dump() << '\n';
            return 0;
        }
        auto output = answer.empty() ? wire::solve(input, offline, checks, budget)
                                     : wire::validate_answer(input, wire::read_json(answer), fallback);
        if (!report.empty()) {
            std::ofstream out(report, std::ios::binary);
            if (!out) throw std::runtime_error("cannot open local report output");
            out << output.report.dump(2) << '\n';
            out.close();
            if (!out) throw std::runtime_error("cannot write local report");
        }
        std::cerr << Json{{"objective", output.report["objective"]},
                           {"valid", true}, {"used_fallback", output.report["used_fallback"]},
                           {"offline", offline}, {"note", "No HTTP submission was performed."}}.dump() << '\n';
        std::cout << output.answer.dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << Json{{"error", e.what()}}.dump() << '\n';
        return 1;
    } catch (const Timeout &) {
        std::cerr << Json{{"error", "unexpected timeout outside solver fallback boundary"}}.dump() << '\n';
        return 2;
    }
}
