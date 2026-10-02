#include "replay/transition.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>

namespace replay::detail {
namespace {

OperationResult failure(ErrorCode code, int step, int agent_id, std::string message) {
    OperationResult result;
    result.ok = false;
    result.error = {code, step, agent_id, std::move(message)};
    return result;
}

int spotIndexAtCell(const DailyInput& input, int cell_id) {
    for (std::size_t i = 0; i < input.spots.size(); ++i) {
        if (input.spots[i].cell_id == cell_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::size_t> indicesInAgentIdOrder(const DailyInput& input) {
    std::vector<std::size_t> order(input.agents.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        return input.agents[lhs].id < input.agents[rhs].id;
    });
    return order;
}

std::vector<ActiveActionView> activeViews(const RuntimeState& runtime) {
    std::vector<ActiveActionView> views;
    views.reserve(runtime.active.size());
    for (const ActiveAction& action : runtime.active) {
        views.push_back({action.moving, action.from_cell, action.to_cell, action.remaining});
    }
    return views;
}

}  // namespace

OperationResult reserveNextAction(
    const DailyInput& input,
    const DailyPlan& plan,
    RuntimeState& runtime,
    std::size_t agent_index,
    int step) {
    const int agent_id = input.agents[agent_index].id;
    if (runtime.next_action[agent_index] >= plan.actions[agent_index].size()) {
        return failure(
            ErrorCode::ActionListEnded,
            step,
            agent_id,
            "action list ended before daySteps was filled");
    }

    const int encoded = plan.actions[agent_index][runtime.next_action[agent_index]++];
    ActiveAction next;
    next.from_cell = runtime.world.agents[agent_index].cell_id;

    if (encoded < 0) {
        if (encoded == std::numeric_limits<int>::min()) {
            return failure(ErrorCode::InvalidWait, step, agent_id, "wait duration overflows");
        }
        next.remaining = -encoded;
        if (next.remaining <= 0) {
            return failure(ErrorCode::InvalidWait, step, agent_id, "wait duration must be positive");
        }
    } else {
        if (encoded > 5) {
            return failure(ErrorCode::InvalidDirection, step, agent_id, "direction must be in [0, 5]");
        }
        const int destination = input.map.neighbor(next.from_cell, encoded);
        if (destination < 0) {
            return failure(ErrorCode::OutOfBounds, step, agent_id, "move leaves the map");
        }
        if (input.map.cells[destination].terrain == Terrain::Lake) {
            return failure(ErrorCode::LakeEntry, step, agent_id, "move enters a lake cell");
        }
        next.moving = true;
        next.to_cell = destination;
        next.remaining = stepCost(input.map.cells[next.from_cell]);
    }

    runtime.active[agent_index] = next;
    return {};
}

OperationResult transitionOneStep(
    const DailyInput& input,
    RuntimeState& runtime,
    int step,
    std::vector<std::size_t>& completing,
    StepSnapshot& snapshot) {
    completing.clear();
    for (std::size_t i = 0; i < runtime.active.size(); ++i) {
        if (runtime.active[i].remaining <= 0) {
            return failure(
                ErrorCode::InvalidInput,
                step,
                input.agents[i].id,
                "active action has no remaining duration");
        }
        --runtime.active[i].remaining;
        if (runtime.active[i].remaining == 0) {
            completing.push_back(i);
        }
    }

    // Phase 1: check every completing move, then consume fuel atomically.
    for (std::size_t i : completing) {
        if (!runtime.active[i].moving || input.agents[i].type != AgentType::Patrol) {
            continue;
        }
        const int required = fuelCost(input.map.cells[runtime.active[i].from_cell]);
        const int available = runtime.world.agents[i].fuel;
        if (available < required) {
            std::ostringstream message;
            message << "fuel shortage: required=" << required << ", available=" << available;
            return failure(ErrorCode::FuelShortage, step, input.agents[i].id, message.str());
        }
    }
    for (std::size_t i : completing) {
        if (runtime.active[i].moving && input.agents[i].type == AgentType::Patrol) {
            runtime.world.agents[i].fuel -=
                fuelCost(input.map.cells[runtime.active[i].from_cell]);
        }
    }

    // Phase 2: completed moves become visible simultaneously.
    for (std::size_t i : completing) {
        if (runtime.active[i].moving) {
            runtime.world.agents[i].cell_id = runtime.active[i].to_cell;
        }
    }

    // Phase 3: Spot acquisition is serialized by public agent ID.
    std::vector<Acquisition> acquisitions;
    for (std::size_t i : indicesInAgentIdOrder(input)) {
        if (input.agents[i].type != AgentType::Patrol) {
            continue;
        }
        AgentState& agent = runtime.world.agents[i];
        const int spot_index = spotIndexAtCell(input, agent.cell_id);
        if (spot_index < 0) {
            continue;
        }
        const SpotInfo& spot = input.spots[static_cast<std::size_t>(spot_index)];
        if (agent.visited_today.count(spot.id) != 0 ||
            runtime.world.spot_stock[static_cast<std::size_t>(spot_index)] <= 0) {
            continue;
        }
        --runtime.world.spot_stock[static_cast<std::size_t>(spot_index)];
        ++agent.udon;
        agent.visited_today.insert(spot.id);
        agent.series_today.insert(spot.series);
        agent.collected_series.insert(spot.series);
        acquisitions.push_back({input.agents[i].id, spot.id});
    }

    // Phase 4: every patrol sharing a reflected cell with any supply is full.
    std::set<int> supply_cells;
    for (std::size_t i = 0; i < input.agents.size(); ++i) {
        if (input.agents[i].type == AgentType::Supply) {
            supply_cells.insert(runtime.world.agents[i].cell_id);
        }
    }
    std::vector<int> refueled;
    for (std::size_t i : indicesInAgentIdOrder(input)) {
        if (input.agents[i].type != AgentType::Patrol ||
            supply_cells.count(runtime.world.agents[i].cell_id) == 0) {
            continue;
        }
        runtime.world.agents[i].fuel = input.agents[i].fuel_capacity;
        refueled.push_back(input.agents[i].id);
    }

    snapshot.step = step;
    snapshot.state = runtime.world;
    snapshot.active_actions = activeViews(runtime);
    snapshot.acquisitions = std::move(acquisitions);
    snapshot.refueled_agent_ids = std::move(refueled);
    return {};
}

}  // namespace replay::detail
