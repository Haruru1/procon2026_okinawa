#include "replay/types.hpp"

#include <array>
#include <limits>

namespace replay {

bool Map::inBounds(int row, int col) const {
    return 0 <= row && row < height && 0 <= col && col < width;
}

bool Map::isCell(int cell_id) const {
    return 0 <= cell_id && cell_id < static_cast<int>(cells.size());
}

int Map::cellId(int row, int col) const {
    return inBounds(row, col) ? row * width + col : -1;
}

std::pair<int, int> Map::rowCol(int cell_id) const {
    if (!isCell(cell_id) || width <= 0) {
        return {-1, -1};
    }
    return {cell_id / width, cell_id % width};
}

HexCoord Map::toAxial(int cell_id) const {
    const auto [row, col] = rowCol(cell_id);
    return {col - (row + (row & 1)) / 2, row};
}

int Map::fromAxial(HexCoord coord) const {
    const int row = coord.r;
    const int col = coord.q + (coord.r + (coord.r & 1)) / 2;
    return cellId(row, col);
}

int Map::neighbor(int cell_id, int direction) const {
    static constexpr std::array<HexCoord, 6> directions{{
        {0, -1}, {1, -1}, {1, 0}, {0, 1}, {-1, 1}, {-1, 0},
    }};

    if (!isCell(cell_id) || direction < 0 || direction >= 6) {
        return -1;
    }
    const HexCoord source = toAxial(cell_id);
    return fromAxial({source.q + directions[direction].q,
                      source.r + directions[direction].r});
}

int stepCost(const Cell& source) {
    switch (source.terrain) {
    case Terrain::Plain:
        return 2;
    case Terrain::Mountain:
        return 3;
    case Terrain::Road:
        switch (source.road_level) {
        case RoadLevel::Clear:
            return 1;
        case RoadLevel::Crowded:
            return 2;
        case RoadLevel::Jammed:
            return 4;
        }
        break;
    case Terrain::Lake:
        break;
    }
    return std::numeric_limits<int>::max() / 4;
}

int fuelCost(const Cell& source) {
    switch (source.terrain) {
    case Terrain::Plain:
        return 1;
    case Terrain::Mountain:
        return 2;
    case Terrain::Road:
        return 2;
    case Terrain::Lake:
        break;
    }
    return std::numeric_limits<int>::max() / 4;
}

std::string toString(ErrorCode code) {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::InvalidInput: return "invalid_input";
    case ErrorCode::AgentCountMismatch: return "agent_count_mismatch";
    case ErrorCode::EmptyActionList: return "empty_action_list";
    case ErrorCode::ActionListEnded: return "action_list_ended";
    case ErrorCode::InvalidDirection: return "invalid_direction";
    case ErrorCode::InvalidWait: return "invalid_wait";
    case ErrorCode::OutOfBounds: return "out_of_bounds";
    case ErrorCode::LakeEntry: return "lake_entry";
    case ErrorCode::FuelShortage: return "fuel_shortage";
    case ErrorCode::IncompleteAtDayEnd: return "incomplete_at_day_end";
    case ErrorCode::UnusedActions: return "unused_actions";
    }
    return "unknown";
}

}  // namespace replay
