"""Q6 reflected occupancy, tested independently of the frozen Python event model."""
import itertools
from functools import reduce
import random
import unittest

from competition.solver.tests.support import cpp
from competition.solver.tests.support import ReplayBridge


def line_case(terrains=('plain', 'plain', 'plain'), steps=6, roots=(0,)):
    return dict(map=dict(width=len(terrains), height=1,
                         cells=[dict(terrain=t, road='crowded') for t in terrains]),
                day_steps=steps, spots=[dict(id=0, cell=0, series=9, stock=2)],
                agents=[dict(id=0, type='patrol', cell=0, capacity=3, fuel=3)] +
                [dict(id=i+1, type='supply', cell=root) for i, root in enumerate(roots)])


def duration(cell):
    return {'plain': 2, 'mountain': 3, 'road':
            {'clear': 1, 'crowded': 2, 'jammed': 4}[cell.get('road', 'clear')]}[cell['terrain']]


def routes(case, start):
    """Exhaust all physical command sequences on a horizontal line, not event paths."""
    cells = case['map']['cells']
    T = case['day_steps']
    def walk(pos, t, actions, occupied):
        if t == T:
            yield actions, occupied
            return
        yield from walk(pos, t+1, actions+[-1], occupied+[(t+1, pos)])
        tau = duration(cells[pos])
        if t + tau <= T:
            for command, dest in ((5, pos-1), (2, pos+1)):
                if 0 <= dest < len(cells):
                    trace = [(t+j, pos) for j in range(1, tau)] + [(t+tau, dest)]
                    yield from walk(dest, t+tau, actions+[command], occupied+trace)
    yield from walk(start, 0, [], [])


def satisfied(formula, events):
    return all(any(tuple(e) in events for e in clause) for clause in formula)


class RuleComplianceTests(unittest.TestCase):
    def test_day_start_spot_wait_and_immediate_departure_no_step_zero(self):
        case = line_case(steps=4, roots=())
        for actions in ([-4], [-1, 2, -1], [2, -2]):
            with self.subTest(actions=actions):
                p = cpp(case, mode='profile', patrol=0, actions=actions)
                actual = cpp(case, mode='replay', plan={'actions': [actions]})
                replayed_again = ReplayBridge().evaluate(case, [{'actions': [actions]}], False)[0]['replay']
                self.assertEqual(actual['trace'], replayed_again['trace'])
                self.assertEqual(actual['committed_state'], replayed_again['committed_state'])
                self.assertTrue(actual['valid'])
                self.assertEqual(p['spot_events'], [[0, 1]])
                self.assertEqual(p['spot_mask'], 1)
                final = actual['committed_state']['agents'][0]
                self.assertEqual(final['cell'], p['end_cell'])
                self.assertEqual(final['fuel'], p['fuel'])
                self.assertEqual(final['udon'], 1)
                self.assertEqual(final['series_today'], [9])
                self.assertEqual(actual['trace'][0]['acquisitions'], [{'agent_id': 0, 'spot_id': 0}])
        audit = cpp(case, mode='audit')
        moved = [r for r in audit['pools'][0]['movement'] if r['actions']]
        self.assertTrue(moved)
        self.assertTrue(all(r['spot_mask'] & 1 for r in moved))

    def test_pending_move_contact_is_not_infeasible(self):
        case = line_case(steps=2)
        case['agents'][0]['fuel'] = 0
        query = {'patrol_actions': [{'patrol': 0, 'actions': [2]}]}
        old = cpp(case, mode='supply', legacy_contacts=True, queries=[query])['answers'][0]
        self.assertEqual(old['status'], 'INFEASIBLE')
        current = cpp(case, mode='supply', queries=[query])['answers'][0]
        self.assertEqual(current['formula'], [[[1, 0]]])
        self.assertEqual(current['status'], 'FEASIBLE')
        plan = {'actions': [[2]] + current['supply_actions']}
        replay = cpp(case, mode='replay', plan=plan)
        self.assertTrue(replay['valid'])
        self.assertEqual(replay['trace'][0]['refueled_agent_ids'], [0])
        # Both moving in parallel: source contact at t=1 and target at t=2.
        formula = [[[1, 0]], [[2, 1]]]
        answer = cpp(case, mode='supply', queries=[{'formula': formula}])['answers'][0]
        self.assertEqual(answer['status'], 'FEASIBLE')
        self.assertEqual(answer['supply_actions'], [[2]])
        result = cpp(case, mode='replay', plan={'actions': [[2], [2]]})
        self.assertEqual(result['committed_state']['agents'][0]['fuel'], 3)

    def test_arrival_readiness_cannot_be_forgotten(self):
        case = line_case(('plain',)*4, steps=6, roots=(1,))
        case['map'].update(width=2, height=2)
        # Cells 0/1/3 form a triangle. Every pair is feasible from root 1,
        # but visiting 0 at t=3 delays arrival back at 1 until t=4, leaving
        # too little time to reach 3 at t=5. A pair-only DAG accepts falsely.
        query = {'formula': [[[3, 0]], [[4, 1]], [[5, 3]]]}
        answer = cpp(case, mode='supply', queries=[query])['answers'][0]
        self.assertEqual(answer['filter_reason'], '', 'this must reach the DP')
        self.assertEqual(answer['status'], 'INFEASIBLE')

    def test_physical_route_exhaustion_matches_filter_dp_and_witness(self):
        rng = random.Random(20260923)
        for terrain in (('plain',)*3, ('road','plain','mountain'), ('mountain','road','plain')):
            for roots in ((0,), (0,2)):
                case = line_case(terrain, roots=roots)
                case['spots'] = []
                for cell in case['map']['cells']:
                    if cell['terrain'] == 'road':
                        cell['road'] = 'clear' if terrain[0] == 'road' else 'jammed'
                event_universe = list(itertools.product(range(1,7), range(3)))
                formulas = [[], [[]], [[[1,0]],[[2,1]],[[3,2]]]]
                for _ in range(200):
                    formulas.append([rng.sample(event_universe, rng.randint(1,4))
                                     for _ in range(rng.randint(1,6))])
                queries = [dict(formula=f) for f in formulas]
                answers = cpp(case, mode='supply', queries=queries, seconds=120)['answers']
                physical = [[set(trace) for _, trace in routes(case, root)] for root in roots]
                witnesses = []
                for i, (formula, answer) in enumerate(zip(formulas, answers)):
                    full = (1 << len(formula)) - 1
                    coverage = []
                    for r in physical:
                        coverage.append({sum(1 << n for n, g in enumerate(formula)
                                             if any(tuple(e) in trace for e in g)) for trace in r})
                    possible = any(reduce(int.__or__, masks, 0) == full
                                   for masks in itertools.product(*coverage))
                    self.assertEqual(answer['status'], 'FEASIBLE' if possible else 'INFEASIBLE',
                                     (terrain, roots, i, formula, answer))
                    if possible:
                        plan = {'actions': [[-6]] + answer['supply_actions']}
                        witnesses.append((formula, plan))
                replays = ReplayBridge().evaluate(case, [plan for _, plan in witnesses], False)
                for (formula, plan), evaluated in zip(witnesses, replays):
                    replay = evaluated['replay']
                    self.assertTrue(replay['valid'], (terrain, roots, formula, plan, replay))
                    occupied = {(f['step'], a['cell']) for f in replay['trace']
                                for a in f['state']['agents'][1:]}
                    self.assertTrue(satisfied(formula, occupied))

    def test_cnf_all_contact_subsets_against_stepwise_fuel_replay(self):
        # All command sequences, every H subset, including mid-move resets,
        # OR alternatives, multiple reset windows and debit-before-reset.
        case = line_case(steps=6)
        for actions, occupied in routes(case, 0):
            for fuel in (0,1,3):
                case['agents'][0]['fuel'] = fuel
                profile = cpp(case, mode='profile', patrol=0, actions=actions)
                self.assertEqual(profile['positions'][1:], [c for _, c in occupied])
                last = 0
                burns = [0]
                for _, cell in occupied:
                    burns.append(int(cell != last))  # This fixture contains only plain cells.
                    last = cell
                self.assertEqual(profile['burns'], burns)
                for mask in range(64):
                    H = {event for i, event in enumerate(occupied) if mask & (1 << i)}
                    left = fuel
                    valid = True
                    for step, cell in occupied:
                        left -= burns[step]
                        if left < 0:
                            valid = False
                            break
                        if (step, cell) in H:
                            left = 3
                    self.assertEqual(satisfied(profile['requirements'], H), valid,
                                     (actions, fuel, H, profile['requirements']))

    def test_multirefill_terminal_cnf_and_final_debit(self):
        case = line_case(('road',)*4, steps=4)
        case['spots'] = []
        for cell in case['map']['cells']:
            cell['road'] = 'clear'
        actions = [2, 2, 2, -1]
        p = cpp(case, mode='profile', patrol=0, actions=actions)
        self.assertEqual(p['requirements'], [[[1,1]], [[2,2]]])
        self.assertFalse(satisfied(p['requirements'], {(1,1)}))
        self.assertFalse(satisfied(p['requirements'], {(2,2)}))
        self.assertTrue(satisfied(p['requirements'], {(1,1),(2,2)}))
        # A reset at t=3 is too late to pay t=3's debit (terminal feasibility).
        self.assertFalse(satisfied(p['requirements'], {(1,1),(3,3)}))
        result = cpp(case, mode='supply', queries=[{
            'patrol_actions': [{'patrol': 0, 'actions': actions}]}])['answers'][0]
        self.assertEqual(result['status'], 'FEASIBLE')
        replay = cpp(case, mode='replay', plan={'actions':[actions]+result['supply_actions']})
        self.assertTrue(replay['valid'])

    def test_pdf_q6_e_profile_supply_and_complete_replay(self):
        case = line_case(('road','road','plain','plain'), steps=6, roots=(2,))
        case['map']['cells'][0]['road'] = 'clear'
        case['spots'] = [dict(id=0, cell=2, series=77, stock=4)]
        starts = [0,0,0,0,2,3,3,2]
        case['agents'] = [dict(id=i, type='patrol' if i < 7 else 'supply',
                               cell=c, fuel=3, capacity=3) for i,c in enumerate(starts)]
        actions = [[2,2,2,-1],[-1,2,2,2],[-2,2,2,-1],[-3,2,-2],
                   [5,5,-2],[5,5,-2],[-2,5,5],[5,5,-2]]
        E = cpp(case, mode='profile', patrol=4, actions=actions[4])
        self.assertEqual(E['spot_events'], [[0,1]])
        self.assertEqual(E['positions'], [2,2,1,1,0,0,0])
        # Force the entire published supply occupancy, including pending moves.
        formula = [[[t,c]] for t,c in enumerate([2,1,1,0,0,0], 1)]
        result = cpp(case, mode='supply', queries=[{'formula':formula}])['answers'][0]
        self.assertEqual(result['status'], 'FEASIBLE')
        actions[7] = result['supply_actions'][0]
        replay = cpp(case, mode='replay', plan={'actions': actions})
        self.assertTrue(replay['valid'])
        self.assertEqual(replay['committed_state']['agents'][4]['fuel'], 3)
        self.assertEqual([f['state']['agents'][4]['fuel'] for f in replay['trace']], [3]*6)
        self.assertEqual(replay['committed_state']['agents'][4]['cell'], 0)
        self.assertEqual([(f['step'], a['agent_id']) for f in replay['trace']
                          for a in f['acquisitions']], [(1,4),(2,5),(3,0),(4,1)])
        replayed_again = ReplayBridge().evaluate(case, [{'actions': actions}], False)[0]['replay']
        self.assertEqual(replay['trace'], replayed_again['trace'])

    def test_reflected_timeout_is_unknown_and_retry_is_clean(self):
        case = line_case()
        formula = [[[1,0]], [[2,1]], [[4,2]]]
        result = cpp(case, mode='supply', queries=[
            {'formula': formula, 'local_checks': 0}, {'formula': formula}])['answers']
        self.assertEqual(result[0]['status'], 'UNKNOWN')
        self.assertEqual(result[0]['cache_entries'], 0)
        self.assertEqual(result[1]['status'], 'FEASIBLE')


if __name__ == '__main__':
    unittest.main()
