#pragma once
#include "../../competition/solver/third_party/json.hpp"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// File projection only: no solver, replay engine, network or process control.
namespace result_viewer {
using Json = nlohmann::json;
struct Agent { int cell, fuel; };
struct Frame {
    std::vector<Agent> agents;
    std::vector<int> stock, refueled;
};
struct Spot { int cell; int64_t brand; int stock; };
struct Attempt {
    std::string label, status;
    int revision = -1;
    bool replay_valid = false;
    std::optional<double> seconds, margin;
    std::array<int, 3> objective{};
    std::vector<Frame> frames;
};
struct Day {
    int index = 0, steps = 0, preferred = -1;
    bool next_state_verified = false, unresolved = false;
    std::string solver_error;
    std::vector<int> kinds, traffic;
    Frame initial;
    std::vector<Attempt> attempts;
};
struct Document {
    int width = 0, height = 0, fuel_limit = 0;
    std::vector<int> terrain;
    std::vector<Spot> spots;
    std::vector<Day> days;
    std::string status;
};
inline void require(bool ok, const std::string& reason) {
    if (!ok) throw std::runtime_error(reason);
}
inline int number(const Json& j, int low, int high, const char* field) {
    require(j.is_number_integer(), std::string(field) + ": expected integer");
    require(j >= low && j <= high, std::string(field) + ": out of range");
    return j.get<int>();
}
inline Json read(const std::filesystem::path& path) {
    require(std::filesystem::file_size(path) <= 256ull * 1024 * 1024, "JSON exceeds 256 MiB");
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "Cannot open JSON file");
    return Json::parse(in);
}
inline Document map_document(const Json& m) {
    Document out;
    const auto& map = m.at("map");
    out.width = number(map.at("width"), 1, 32, "map.width");
    out.height = number(map.at("height"), 1, 32, "map.height");
    out.fuel_limit = number(m.at("fuelLimits"), 1, 1000000000, "fuelLimits");
    const auto& cells = map.at("cells");
    require(cells.is_array() && cells.size() == size_t(out.height), "map.cells height mismatch");
    for (const auto& row : cells) {
        require(row.is_array() && row.size() == size_t(out.width), "map.cells width mismatch");
        for (const auto& c : row) out.terrain.push_back(number(c, 0, 3, "terrain"));
    }
    const auto& spots = m.at("spots");
    require(spots.is_array() && spots.size() <= 32, "spots must be array of at most 32");
    for (const auto& s : spots) {
        require(s.at("brand").is_number_integer() && s.at("brand") >= 0 &&
                s.at("brand") <= INT64_MAX, "invalid brand ID");
        out.spots.push_back({number(s.at("pos"), 0, out.width * out.height - 1, "spot.pos"),
                             s.at("brand").get<int64_t>(), number(s.at("stocks"), 0, 1000000000, "stocks")});
    }
    return out;
}
inline Frame frame(const Json& j, const Document& doc, size_t agents) {
    Frame f;
    const auto& rows = j.at("agents");
    require(rows.is_array() && rows.size() == agents, "replay agent count mismatch");
    for (const auto& a : rows)
        f.agents.push_back({number(a.at("cell"), 0, doc.width * doc.height - 1, "agent.cell"),
                             number(a.at("fuel"), 0, doc.fuel_limit, "agent.fuel")});
    const auto& stock = j.at("spot_stock");
    require(stock.is_array() && stock.size() == doc.spots.size(), "replay stock count mismatch");
    for (size_t i = 0; i < stock.size(); ++i)
        f.stock.push_back(number(stock[i], 0, doc.spots[i].stock, "spot_stock"));
    return f;
}
inline Day day_input(const Json& j, const Json& map, const Document& doc) {
    Day d;
    const auto& steps = map.at("daySteps");
    require(steps.is_array() && !steps.empty(), "daySteps missing");
    d.index = number(j.at("day"), 0, int(steps.size()) - 1, "day");
    d.steps = number(steps[d.index], 1, 256, "daySteps");
    const auto& agents = j.at("agents");
    require(agents.is_array() && !agents.empty() && agents.size() <= 8, "invalid agents");
    for (const auto& a : agents) {
        d.kinds.push_back(number(a.at("kind"), 0, 1, "kind"));
        d.initial.agents.push_back({number(a.at("pos"), 0, doc.width * doc.height - 1, "agent.pos"),
                                    number(a.at("fuel"), 0, doc.fuel_limit, "agent.fuel")});
    }
    for (const auto& s : doc.spots) d.initial.stock.push_back(s.stock);
    d.traffic.assign(doc.terrain.size(), -1);
    require(j.at("traffics").is_array(), "traffics must be array");
    for (const auto& t : j.at("traffics"))
        d.traffic[number(t.at("pos"), 0, int(doc.terrain.size()) - 1, "traffic.pos")] =
            number(t.at("status"), 0, 2, "traffic.status");
    return d;
}
inline Attempt attempt(const Json& report, const Day& day, const Document& doc) {
    require(report.at("format") == "c_staged.local_report.v1", "unsupported report format");
    require(number(report.at("day"), 0, INT32_MAX, "report.day") == day.index, "report/day mismatch");
    Attempt a;
    a.label = report.at("used_fallback").get<bool>() ? "fallback" :
        (report.contains("solver_completed") ? "solver" : "local report");
    a.status = "unrecorded";
    const auto& obj = report.at("objective");
    require(obj.is_array() && obj.size() == 3, "objective must have three entries");
    for (int i = 0; i < 3; ++i) a.objective[i] = number(obj[i], 0, INT32_MAX, "objective");
    if (report.contains("elapsed_seconds")) a.seconds = report.at("elapsed_seconds").get<double>();
    const auto& r = report.at("replay");
    a.replay_valid = r.at("valid").get<bool>();
    // Invalid/truncated reports must not be presented as usable routes.
    require(a.replay_valid, "report replay.valid is false");
    const auto& trace = r.at("trace");
    require(trace.is_array() && trace.size() == size_t(day.steps), "replay trace length mismatch");
    a.frames.push_back(day.initial);
    for (size_t i = 0; i < trace.size(); ++i) {
        const auto& t = trace[i];
        require(number(t.at("step"), 1, day.steps, "trace.step") == int(i + 1), "nonsequential trace");
        auto f = frame(t.at("state"), doc, day.kinds.size());
        require(t.at("refueled_agent_ids").is_array(), "invalid refuel IDs");
        for (const auto& id : t.at("refueled_agent_ids"))
            f.refueled.push_back(number(id, 0, int(day.kinds.size()) - 1, "refueled_agent_ids"));
        a.frames.push_back(std::move(f));
    }
    // This is structural comparison, not a second rule/replay implementation.
    require(trace.back().at("state") == r.at("committed_state"), "trace/final state mismatch");
    return a;
}
inline void check_context(const Json& report, const Json& map, const Day& d) {
    const auto& history = report.at("next_history_draft");
    require(history.at("map_config") == map, "report/map mismatch");
    require(history.at("agent_kinds") == Json(d.kinds), "report/agent kinds mismatch");
}
inline Document parse(const Json& root, const std::optional<Json>& daily = {}) {
    require(root.is_object() && root.contains("format") && root["format"].is_string(),
            "Open journal.json or report.json; this file has no supported format field");
    const auto format = root.at("format").get<std::string>();
    if (format == "c_staged.local_report.v1") {
        require(daily.has_value(), "report.json needs day.json in the same folder; or open journal.json");
        const auto& map = root.at("next_history_draft").at("map_config");
        auto doc = map_document(map);
        auto d = day_input(*daily, map, doc);
        check_context(root, map, d);
        d.attempts.push_back(attempt(root, d, doc));
        d.preferred = 0;
        doc.days.push_back(std::move(d));
        doc.status = "local report";
        return doc;
    }
    require(format == "c_staged.match_journal.v1", "Open journal.json or report.json, not answer/summary JSON");
    auto doc = map_document(root.at("setting"));
    doc.status = root.at("status").get<std::string>();
    require(root.at("days").is_array(), "journal.days must be array");
    int previous = -1;
    for (const auto& row : root.at("days")) {
        auto d = day_input(row.at("state"), root.at("setting"), doc);
        require(d.index > previous, "journal days not ordered");
        previous = d.index;
        d.next_state_verified = row.value("next_state_verified", false);
        d.unresolved = row.contains("ambiguous_payload") && !row["ambiguous_payload"].is_null();
        d.solver_error = row.value("solver_error", std::string{});
        require(row.at("attempts").is_array(), "attempts must be array");
        int revision = -1;
        for (const auto& record : row.at("attempts")) {
            check_context(record.at("report"), root.at("setting"), d);
            auto a = attempt(record.at("report"), d, doc);
            a.label = record.at("label").get<std::string>();
            if (record.at("report").at("used_fallback").get<bool>()) a.label = "fallback";
            a.status = record.at("status").get<std::string>();
            a.revision = record.contains("revision") ? number(record.at("revision"), -1, INT32_MAX, "revision") : -1;
            require(a.status != "accepted" || a.revision >= 0, "accepted answer has no valid revision");
            if (record.contains("engine_seconds")) a.seconds = record.at("engine_seconds").get<double>();
            if (record.contains("conservative_margin")) a.margin = record.at("conservative_margin").get<double>();
            require(!a.seconds || (std::isfinite(*a.seconds) && *a.seconds >= 0), "invalid elapsed seconds");
            require(!a.margin || std::isfinite(*a.margin), "invalid deadline margin");
            if (a.status == "accepted" && a.revision > revision) {
                d.preferred = int(d.attempts.size());
                revision = a.revision;
            }
            d.attempts.push_back(std::move(a));
        }
        if (d.preferred < 0 && !d.attempts.empty()) d.preferred = int(d.attempts.size()) - 1;
        doc.days.push_back(std::move(d));
    }
    require(!doc.days.empty(), "No day data has been recorded yet");
    return doc;
}
inline Document load(const std::filesystem::path& path) {
    const auto j = read(path);
    if (j.value("format", std::string{}) == "c_staged.local_report.v1") {
        const auto day = path.parent_path() / "day.json";
        require(std::filesystem::exists(day), "report.json needs adjacent day.json; open journal.json instead");
        if (std::filesystem::exists(path.parent_path() / "map.json"))
            require(read(path.parent_path() / "map.json") == j.at("next_history_draft").at("map_config"),
                    "adjacent map.json differs from report");
        return parse(j, read(day));
    }
    return parse(j);
}
class Store {
public:
    std::optional<Document> document;
    std::filesystem::path path;
    std::string error;
    bool open(const std::filesystem::path& requested) {
        try {
            auto next = load(requested);
            document = std::move(next);
            path = requested;
            error.clear();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }
    bool reload() { return open(path); }
};
} // namespace result_viewer
