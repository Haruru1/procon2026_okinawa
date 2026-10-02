#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace replay {

enum class Terrain {
    Plain,
    Road,
    Mountain,
    Lake,
};

enum class RoadLevel {
    Clear,
    Crowded,
    Jammed,
};

enum class AgentType {
    Patrol,
    Supply,
};

struct HexCoord {
    int q = 0;
    int r = 0;
};

struct Cell {
    Terrain terrain = Terrain::Plain;
    RoadLevel road_level = RoadLevel::Clear;
};

struct Map {
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;

    bool inBounds(int row, int col) const;
    bool isCell(int cell_id) const;
    int cellId(int row, int col) const;
    std::pair<int, int> rowCol(int cell_id) const;
    HexCoord toAxial(int cell_id) const;
    int fromAxial(HexCoord coord) const;
    int neighbor(int cell_id, int direction) const;
};

struct SpotInfo {
    int id = -1;
    int cell_id = -1;
    int series = 0;
    int stock_max = 1;
};

struct AgentSpec {
    int id = -1;
    AgentType type = AgentType::Patrol;
    int fuel_capacity = 0;
};

struct AgentState {
    int cell_id = -1;
    int fuel = 0;
    int udon = 0;
    std::set<int> visited_today;
    std::set<int> series_today;
    std::set<int> collected_series;
};

struct WorldState {
    std::vector<AgentState> agents;
    // Indexed by the position in DailyInput::spots, not by SpotInfo::id.
    std::vector<int> spot_stock;
};

struct DailyPlan {
    // Indexed like DailyInput::agents. 0..5 means move, and -w means wait w steps.
    std::vector<std::vector<int>> actions;
};

struct DailyInput {
    Map map;
    std::vector<SpotInfo> spots;
    std::vector<AgentSpec> agents;
    WorldState initial_state;
    int day_steps = 0;
};

struct Acquisition {
    int agent_id = -1;
    int spot_id = -1;
};

struct ActiveActionView {
    bool moving = false;
    int from_cell = -1;
    int to_cell = -1;
    int remaining = 0;
};

struct StepSnapshot {
    int step = 0;
    WorldState state;
    std::vector<ActiveActionView> active_actions;
    std::vector<Acquisition> acquisitions;
    std::vector<int> refueled_agent_ids;
};

enum class ErrorCode {
    None,
    InvalidInput,
    AgentCountMismatch,
    EmptyActionList,
    ActionListEnded,
    InvalidDirection,
    InvalidWait,
    OutOfBounds,
    LakeEntry,
    FuelShortage,
    IncompleteAtDayEnd,
    UnusedActions,
};

struct ValidationError {
    ErrorCode code = ErrorCode::None;
    int step = -1;
    int agent_id = -1;
    std::string message;
};

struct SimulationResult {
    bool valid = false;
    ValidationError error;
    WorldState initial_state;
    WorldState attempted_state;
    // Equals attempted_state on success and initial_state on full-answer rejection.
    WorldState committed_state;
    std::vector<StepSnapshot> trace;
};

int stepCost(const Cell& source);
int fuelCost(const Cell& source);
std::string toString(ErrorCode code);

}  // namespace replay
