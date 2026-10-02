#include "replay/replay.hpp"

#include "replay/transition.hpp"

#include <set>
#include <sstream>

namespace replay {
namespace {

ValidationError validateInput(const DailyInput& input, const DailyPlan& plan) {
    auto invalid = [](std::string message) {
        return ValidationError{ErrorCode::InvalidInput, 0, -1, std::move(message)};
    };

    if (input.map.width <= 0 || input.map.height <= 0 ||
        static_cast<int>(input.map.cells.size()) != input.map.width * input.map.height) {
        return invalid("map dimensions and cell count disagree");
    }
    if (input.day_steps <= 0) {
        return invalid("day_steps must be positive");
    }
    if (input.initial_state.agents.size() != input.agents.size()) {
        return invalid("initial agent-state count differs from agent specs");
    }
    if (input.initial_state.spot_stock.size() != input.spots.size()) {
        return invalid("initial Spot stock count differs from Spot definitions");
    }
    if (plan.actions.size() != input.agents.size()) {
        return {ErrorCode::AgentCountMismatch, 0, -1,
                "plan agent count differs from agent specs"};
    }

    std::set<int> agent_ids;
    for (std::size_t i = 0; i < input.agents.size(); ++i) {
        if (!agent_ids.insert(input.agents[i].id).second) {
            return invalid("agent IDs must be unique");
        }
        if (!input.map.isCell(input.initial_state.agents[i].cell_id) ||
            input.map.cells[input.initial_state.agents[i].cell_id].terrain == Terrain::Lake) {
            return invalid("an agent starts outside a traversable cell");
        }
        if (input.agents[i].type == AgentType::Patrol &&
            (input.initial_state.agents[i].fuel < 0 ||
             input.initial_state.agents[i].fuel > input.agents[i].fuel_capacity)) {
            return invalid("patrol fuel is outside [0, fuel_capacity]");
        }
        if (plan.actions[i].empty()) {
            return {ErrorCode::EmptyActionList, 0, input.agents[i].id,
                    "every agent needs an action covering the first step"};
        }
    }

    std::set<int> spot_ids;
    std::set<int> spot_cells;
    for (std::size_t i = 0; i < input.spots.size(); ++i) {
        const SpotInfo& spot = input.spots[i];
        if (!spot_ids.insert(spot.id).second || !spot_cells.insert(spot.cell_id).second) {
            return invalid("Spot IDs and Spot cells must be unique");
        }
        if (!input.map.isCell(spot.cell_id) ||
            input.map.cells[spot.cell_id].terrain != Terrain::Plain) {
            return invalid("every Spot must be on a plain cell");
        }
        if (input.initial_state.spot_stock[i] < 0 ||
            input.initial_state.spot_stock[i] > spot.stock_max) {
            return invalid("Spot stock is outside [0, stock_max]");
        }
    }
    return {};
}

SimulationResult rejected(
    const WorldState& initial,
    const WorldState& attempted,
    std::vector<StepSnapshot> trace,
    ValidationError error) {
    SimulationResult result;
    result.valid = false;
    result.error = std::move(error);
    result.initial_state = initial;
    result.attempted_state = attempted;
    result.committed_state = initial;
    result.trace = std::move(trace);
    return result;
}

}  // namespace

SimulationResult RuleReplay::runDailyPlan(
    const DailyInput& input,
    const DailyPlan& plan) const {
    const ValidationError input_error = validateInput(input, plan);
    if (input_error.code != ErrorCode::None) {
        return rejected(input.initial_state, input.initial_state, {}, input_error);
    }

    detail::RuntimeState runtime;
    runtime.world = input.initial_state;
    runtime.next_action.assign(input.agents.size(), 0);
    runtime.active.resize(input.agents.size());

    for (std::size_t i = 0; i < input.agents.size(); ++i) {
        const detail::OperationResult reserved =
            detail::reserveNextAction(input, plan, runtime, i, 0);
        if (!reserved.ok) {
            return rejected(input.initial_state, runtime.world, {}, reserved.error);
        }
    }

    std::vector<StepSnapshot> trace;
    trace.reserve(static_cast<std::size_t>(input.day_steps));
    for (int step = 1; step <= input.day_steps; ++step) {
        std::vector<std::size_t> completing;
        StepSnapshot snapshot;
        const detail::OperationResult transition =
            detail::transitionOneStep(input, runtime, step, completing, snapshot);
        if (!transition.ok) {
            return rejected(input.initial_state, runtime.world, std::move(trace), transition.error);
        }
        trace.push_back(std::move(snapshot));

        if (step == input.day_steps) {
            break;
        }
        for (std::size_t i : completing) {
            const detail::OperationResult reserved =
                detail::reserveNextAction(input, plan, runtime, i, step);
            if (!reserved.ok) {
                return rejected(input.initial_state, runtime.world, std::move(trace), reserved.error);
            }
        }
    }

    for (std::size_t i = 0; i < input.agents.size(); ++i) {
        if (runtime.active[i].remaining != 0) {
            std::ostringstream message;
            message << "agent remains in an action with " << runtime.active[i].remaining
                    << " step(s) left";
            return rejected(
                input.initial_state,
                runtime.world,
                std::move(trace),
                {ErrorCode::IncompleteAtDayEnd,
                 input.day_steps,
                 input.agents[i].id,
                 message.str()});
        }
        if (runtime.next_action[i] != plan.actions[i].size()) {
            return rejected(
                input.initial_state,
                runtime.world,
                std::move(trace),
                {ErrorCode::UnusedActions,
                 input.day_steps,
                 input.agents[i].id,
                 "actions remain after daySteps was filled"});
        }
    }

    SimulationResult result;
    result.valid = true;
    result.initial_state = input.initial_state;
    result.attempted_state = runtime.world;
    result.committed_state = runtime.world;
    result.trace = std::move(trace);
    return result;
}

}  // namespace replay
