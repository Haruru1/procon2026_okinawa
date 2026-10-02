"""Official PDF adapter gates. Python is test-only; the CLI is standalone C++."""
from __future__ import annotations

import copy
import json
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from competition.solver.tests.support import cpp, suite
from competition.solver.tests.support import ReplayBridge

HERE = Path(__file__).resolve().parents[1]
EXE = HERE / "build" / "contest_solver.exe"
FIXTURES = HERE / "contest" / "fixtures"


def documents(size=8, steps=16, kinds=(0, 0, 1), spots=3):
    cells = [[0] * size for _ in range(size)]
    cells[-1][-1], cells[-1][-2], cells[-1][-3] = 3, 2, 1
    positions = list(range(len(kinds)))
    config = dict(startsAt=1778227200, daySeconds=[60] * 4, daySteps=[steps] * 4,
                  map=dict(height=size, width=size, cells=cells),
                  spots=[dict(brand=i, pos=size + i, stocks=1) for i in range(spots)],
                  agents=positions, fuelLimits=steps, players=1,
                  busyThreshold=2, jammedThreshold=4)
    daily = dict(endsAt=1778227260, day=0,
                 agents=[dict(kind=k, pos=p, fuel=steps) for k, p in zip(kinds, positions)],
                 others=[], traffics=[dict(pos=size*size-3, status=0)])
    return config, daily


def history(config, daily, acquired=None):
    n = len(config['agents'])
    return dict(format='c_staged.local_history.v1', map_config=copy.deepcopy(config),
                completed_days=daily['day'], confirmed=True,
                agent_kinds=[a['kind'] for a in daily['agents']],
                collected_brands=acquired if acquired is not None else [[] for _ in range(n)])


def from_normalized(case):
    terrain = {'plain': 0, 'road': 1, 'mountain': 2, 'lake': 3}
    statuses = {'clear': 0, 'crowded': 1, 'jammed': 2}
    w, h = case['map']['width'], case['map']['height']
    cells = case['map']['cells']
    config = dict(startsAt=1778227200, daySeconds=[60]*4, daySteps=[case['day_steps']]*4,
                  map=dict(width=w, height=h, cells=[[terrain[cells[y*w+x]['terrain']]
                                                    for x in range(w)] for y in range(h)]),
                  spots=[dict(brand=s['series'], pos=s['cell'], stocks=s['stock']) for s in case['spots']],
                  agents=[a['cell'] for a in case['agents']],
                  fuelLimits=max(a['capacity'] for a in case['agents']), players=1,
                  busyThreshold=2, jammedThreshold=4)
    daily = dict(endsAt=1778227260, day=1,
                 agents=[dict(kind=int(a['type']=='supply'), pos=a['cell'], fuel=a['fuel']) for a in case['agents']],
                 others=[], traffics=[dict(pos=i, status=statuses[c.get('road', 'clear')])
                                       for i, c in enumerate(cells) if c['terrain']=='road'])
    past = history(config, daily, [a.get('collected_series', []) for a in case['agents']])
    return config, daily, past


class OfficialFormatTests(unittest.TestCase):
    def run_cli(self, config, daily=None, *, past=None, answer=None, kinds=None,
                flags=(), raw_map=None, success=True):
        with tempfile.TemporaryDirectory(prefix='contest_format_') as folder:
            root = Path(folder)
            def write(name, obj):
                path = root / name
                path.write_text(json.dumps(obj), encoding='utf-8')
                return str(path)
            map_path = write('map.json', config)
            if raw_map is not None:
                Path(map_path).write_text(raw_map, encoding='utf-8')
            args = [str(EXE), map_path]
            if daily is not None: args.append(write('day.json', daily))
            if past is not None: args += ['--history', write('history.json', past)]
            if answer is not None: args += ['--validate-answer', write('answer.json', answer)]
            if kinds is not None: args += ['--kinds', write('kinds.json', kinds)]
            report = root / 'report.json'
            args += list(flags) + ['--report', str(report)]
            proc = subprocess.run(args, text=True, capture_output=True, timeout=75)
            if success:
                self.assertEqual(proc.returncode, 0, proc.stderr)
                value = json.loads(proc.stdout)
                data = json.loads(report.read_text(encoding='utf-8')) if report.exists() else None
                return value, data
            self.assertNotEqual(proc.returncode, 0)
            self.assertEqual(proc.stdout, '', 'invalid inputs must not emit a submission')
            return json.loads(proc.stderr)['error']

    def test_pdf_examples_are_verbatim_shape_not_a_combined_valid_case(self):
        m = json.loads((FIXTURES/'pdf_map.json').read_text())
        d = json.loads((FIXTURES/'pdf_day.json').read_text())
        result, _ = self.run_cli(m, d, flags=['--check-format'])
        self.assertEqual(result['map_config'], m)
        self.assertEqual(result['daily'], d)
        self.assertFalse(result['physical_validation_performed'])
        error = self.run_cli(m, d, flags=['--inspect'], success=False)
        self.assertIn('contest.daySteps', error)
        error = self.run_cli(m, d, past=history(m, d),
                             flags=['--inspect', '--allow-synthetic'], success=False)
        self.assertIn('not a road cell', error)
        types, _ = self.run_cli(m, kinds=json.loads((FIXTURES/'pdf_kinds.json').read_text()),
                               flags=['--allow-synthetic'])
        self.assertEqual(types, [0, 1, 0, 1])

    def test_official_conversion_and_explicit_budget(self):
        m, d = documents()
        normalized, _ = self.run_cli(m, d, flags=['--inspect'])
        c = normalized['normalized']
        self.assertEqual(normalized['requested_seconds'], 55)
        self.assertEqual(c['map']['cells'][-3:], [dict(terrain='road', road='clear'),
                                                dict(terrain='mountain', road='clear'),
                                                dict(terrain='lake', road='clear')])
        plan, report = self.run_cli(m, d, flags=['--offline'])
        self.assertIsInstance(plan, list)
        self.assertEqual(len(plan), 3)
        self.assertEqual(report['requested_seconds'], 55)
        self.assertEqual(report['search_deadline_seconds'], 54)
        self.assertTrue(report['replay']['valid'])
        self.assertFalse(report['used_fallback'])
        reference = cpp(c, seconds=55)
        self.assertEqual(plan, reference['plan']['actions'])
        self.assertEqual(report['replay'], reference['replay'])
        self.assertEqual(report['objective'], reference['objective'])
        repeated, check = self.run_cli(m, d, answer=plan)
        self.assertEqual(repeated, plan)
        self.assertEqual(check['replay'], report['replay'])

    def test_small_27_official_adapter_differential(self):
        for _, original, exact, _ in suite():
            with self.subTest(case=original['id']):
                m, d, past = from_normalized(original)
                inspected, _ = self.run_cli(m, d, past=past, flags=['--inspect', '--allow-synthetic'])
                normalized = inspected['normalized']
                actual, report = self.run_cli(m, d, past=past, flags=['--offline', '--allow-synthetic'])
                expected = cpp(normalized, seconds=55)
                self.assertEqual(actual, expected['plan']['actions'])
                self.assertEqual(report['replay'], expected['replay'])
                self.assertEqual(report['objective'], expected['objective'])
                self.assertTrue(all(not a.get('visited_today') and not a.get('series_today')
                                    and not a.get('udon') for a in original['agents']))
                self.assertFalse(original.get('initial_balls_today'))
                self.assertEqual(report['objective'], list(exact))
                replayed = ReplayBridge().evaluate(normalized, [{'actions': actual}], False)[0]['replay']
                self.assertTrue(replayed['valid'])
                self.assertEqual(report['replay']['trace'], replayed['trace'])
                self.assertEqual(report['replay']['committed_state'], replayed['committed_state'])

    def test_history_requires_confirmation_match_day_kind_and_known_brands(self):
        m, d = documents()
        d['day'] = 1
        error = self.run_cli(m, d, flags=['--inspect'], success=False)
        self.assertIn('required for day > 0', error)
        h = history(m, d, [[0], [], []])
        reference, _ = self.run_cli(m, d, past=h, flags=['--inspect'])
        self.assertEqual(reference['normalized']['agents'][0]['collected_series'], [0])
        mutations = [lambda x: x.update(confirmed=False),
                     lambda x: x.update(completed_days=0),
                     lambda x: x['map_config'].update(startsAt=1),
                     lambda x: x['map_config']['map']['cells'][0].__setitem__(0, 2),
                     lambda x: x['agent_kinds'].__setitem__(0, 1),
                     lambda x: x['collected_brands'].__setitem__(0, [999]),
                     lambda x: x['collected_brands'].__setitem__(0, [0, 0]),
                     lambda x: x['collected_brands'].__setitem__(2, [1])]
        for mutation in mutations:
            with self.subTest(mutation=mutations.index(mutation)):
                broken = copy.deepcopy(h); mutation(broken)
                self.assertIn('history', self.run_cli(m, d, past=broken, flags=['--inspect'], success=False))

    def test_brand_ids_are_lossless_and_daily_reset_uses_history(self):
        m, d = documents()
        brands = [-100, 9007199254740001, -100]
        for s, brand in zip(m['spots'], brands): s['brand'] = brand
        d['day'] = 1
        d['agents'][0]['pos'] = m['spots'][0]['pos']
        h = history(m, d, [[-100], [], []])
        plan, report = self.run_cli(m, d, past=h, answer=[[-16]]*3)
        self.assertEqual(report['objective'], [0, 1, 1])
        self.assertEqual(report['brand_dictionary'], sorted(set(brands)))
        draft = report['next_history_draft']
        self.assertFalse(draft['confirmed'])
        self.assertEqual(draft['collected_brands'][0], [-100])
        next_day = copy.deepcopy(d); next_day['day'] = 2
        self.assertIn('confirmed', self.run_cli(m, next_day, past=draft, flags=['--inspect'], success=False))
        draft['confirmed'] = True
        _, next_report = self.run_cli(m, next_day, past=draft, answer=plan)
        self.assertEqual(next_report['objective'], [0, 1, 1])

    def test_empty_few_spots_and_multiple_supplies(self):
        for spots in (0, 1):
            for kinds in ((0, 0, 0), (0, 1, 1), (1, 1, 1)):
                with self.subTest(spots=spots, kinds=kinds):
                    m, d = documents(kinds=kinds, spots=spots)
                    _, report = self.run_cli(m, d, flags=['--offline', '--allow-synthetic'])
                    self.assertTrue(report['replay']['valid'])
                    self.assertLessEqual(report['objective'][0], spots)
                    self.assertFalse(report['used_fallback'])

    def test_later_day_stranded_spot_patrol_is_scored_by_solver(self):
        m, d = documents(kinds=(0, 0, 0), spots=3)
        d['day'] = 2
        d['agents'][0].update(pos=m['spots'][0]['pos'], fuel=0)
        past = history(m, d, [[0, 1, 2], [], []])
        plan, report = self.run_cli(m, d, past=past, flags=['--offline'])
        self.assertEqual(report['objective'], [0, 3, 3])
        self.assertFalse(report['used_fallback'])
        self.assertEqual(plan[0], [-16])
        self.assertEqual(report['replay']['committed_state']['spot_stock'], [0, 0, 0])
        repeated, audit = self.run_cli(m, d, past=past, answer=plan)
        self.assertEqual(repeated, plan)
        self.assertEqual(audit['replay'], report['replay'])
        self.assertEqual(audit['objective'], report['objective'])

    def test_q6_later_day_spot_wait_or_immediate_move_without_forced_wait(self):
        m, d = documents()
        d['agents'][0]['pos'] = m['spots'][0]['pos']
        self.assertIn('first-day position/fuel',
                      self.run_cli(m, d, flags=['--inspect'], success=False))
        d['day'] = 1
        past = history(m, d)
        inspected, _ = self.run_cli(m, d, past=past, flags=['--inspect'])
        c = inspected['normalized']
        for actions in ([-16], [2, -14]):
            with self.subTest(actions=actions):
                answer = [actions, [-16], [-16]]
                submitted, report = self.run_cli(m, d, past=past, answer=answer)
                self.assertEqual(submitted, answer, 'do not insert acquisition-only waits')
                p = cpp(c, mode='profile', patrol=0, actions=actions)
                replay = report['replay']
                self.assertEqual(p['spot_events'][0], [0, 1])
                self.assertEqual(replay['trace'][0]['acquisitions'],
                                 [{'agent_id': 0, 'spot_id': 0}])
                self.assertEqual(report['objective'], [len(p['spot_events'])]*3)
                final = replay['committed_state']['agents'][0]
                self.assertEqual(final['cell'], p['end_cell'])
                self.assertEqual(final['fuel'], p['fuel'])
    def test_maximum_shape_timeout_and_fallback(self):
        m, d = documents(size=32, steps=256, kinds=(0,0,0,0,0,0,1,1), spots=32)
        plan, report = self.run_cli(m, d, flags=['--offline', '--allow-synthetic', '--test-check-budget', '0'])
        self.assertEqual(plan, [[-256]]*8)
        self.assertEqual(report['objective'], [0,0,0])
        self.assertTrue(report['replay']['valid'])
        self.assertFalse(report['solver_completed'])
        self.assertEqual(report['requested_seconds'], 55)
        inspected, _ = self.run_cli(m, d, flags=['--inspect'])
        self.assertEqual(len(inspected['normalized']['map']['cells']), 1024)

    def test_live_deadline_caps_archived_and_short_budget(self):
        m, d = documents()
        plan, report = self.run_cli(m, d)
        self.assertEqual(plan, [[-16]]*3)
        self.assertEqual(report['search_deadline_seconds'], 0)
        self.assertFalse(report['submission_window_open'])
        d['endsAt'] = int(time.time()) + 100
        m['daySeconds'] = [1]*4
        _, report = self.run_cli(m, d)
        self.assertEqual(report['requested_seconds'], 55)
        self.assertEqual(report['search_deadline_seconds'], 0)
        self.assertLess(report['elapsed_seconds'], 1)

    def test_missing_fields_and_wrong_scalar_types(self):
        m, d = documents()
        for field in m:
            with self.subTest(missing_map=field):
                bad = copy.deepcopy(m); del bad[field]
                self.assertIn('required field', self.run_cli(bad, d, flags=['--inspect'], success=False))
        for field in d:
            with self.subTest(missing_day=field):
                bad = copy.deepcopy(d); del bad[field]
                self.assertIn('required field', self.run_cli(m, bad, flags=['--inspect'], success=False))
        for value in (True, 16.0, '16', None, -1, 2**64):
            with self.subTest(bad_fuel=value):
                bad = copy.deepcopy(d); bad['agents'][0]['fuel'] = value
                self.assertIn('fuel', self.run_cli(m, bad, flags=['--inspect'], success=False))

    def test_malformed_duplicate_trailing_and_nonstandard_fields(self):
        m, d = documents()
        for raw in ('{', json.dumps(m) + '{}', '{"startsAt":1,"startsAt":2}', 'null'):
            self.run_cli(m, d, raw_map=raw, flags=['--inspect'], success=False)
        for alias, target in [('traffic', 'traffics'), ('day_steps', 'day')]:
            bad = copy.deepcopy(d); bad[alias] = bad.pop(target)
            self.run_cli(m, bad, flags=['--inspect'], success=False)

    def test_traffic_requires_complete_exact_road_entries(self):
        m, d = documents()
        for entries in ([], [dict(pos=0, status=0)], [dict(pos=61, status=0)]*2,
                        [dict(pos=61, status=3)], [dict(pos=61, status=True)]):
            bad = copy.deepcopy(d); bad['traffics'] = entries
            self.assertIn('traffic', self.run_cli(m, bad, flags=['--inspect'], success=False))

    def test_answer_shapes_durations_movement_and_full_rejection(self):
        m, d = documents()
        invalid = [dict(actions=[[-16]]*3), [[-16]]*2, [[-16]]*4, [[],[-16],[-16]],
                   [[6,-15],[-16],[-16]], [[True,-15],[-16],[-16]],
                   [[0,-14],[-16],[-16]], [[-15],[-16],[-16]],
                   [[-17],[-16],[-16]], [[-16,-1],[-16],[-16]],
                   [[2.0,-14],[-16],[-16]], [[-2**31],[-16],[-16]]]
        for answer in invalid:
            with self.subTest(answer=answer):
                self.run_cli(m, d, answer=answer, success=False)
        fixed, report = self.run_cli(m, d, answer=[[0,-14],[-16],[-16]], flags=['--fallback-on-invalid'])
        self.assertEqual(fixed, [[-16]]*3)
        self.assertTrue(report['used_fallback'])
        self.assertTrue(report['replay']['valid'])

    def test_fuel_before_refuel_no_time_zero_refuel_and_final_contact(self):
        m, d = documents(steps=4, kinds=(0,1,1), spots=0)
        m['map']['cells'][0][0] = 1
        m['fuelLimits'] = 3
        d['agents'] = [dict(kind=0,pos=0,fuel=0), dict(kind=1,pos=1,fuel=0), dict(kind=1,pos=2,fuel=0)]
        d['traffics'].append(dict(pos=0,status=0))
        # Arriving onto a supply cannot pay the fuel debit of this very arrival.
        error = self.run_cli(m, d, answer=[[2,-3],[-4],[-4]], flags=['--allow-synthetic'], success=False)
        self.assertIn('fuel_shortage', error)
        # Even colocated at time zero there is no reflection/refuel until t=1.
        d['agents'][1]['pos'] = 0
        self.assertIn('fuel_shortage', self.run_cli(m, d, answer=[[2,-3],[-4],[-4]],
                                                  flags=['--allow-synthetic'], success=False))
        _, report = self.run_cli(m, d, answer=[[-1,2,-2],[-4],[-4]], flags=['--allow-synthetic'])
        self.assertEqual(report['replay']['committed_state']['agents'][0]['fuel'], 1)
        # First contact at the very last reflection still refills the patrol.
        m, d = documents(steps=2, kinds=(0,1,1), spots=0)
        m['fuelLimits'] = 3
        d['agents'][0]['fuel'] = 1
        _, report = self.run_cli(m, d, answer=[[2],[-2],[-2]], flags=['--allow-synthetic'])
        self.assertEqual(report['replay']['trace'][0]['refueled_agent_ids'], [])
        self.assertEqual(report['replay']['trace'][1]['refueled_agent_ids'], [0])
        self.assertEqual(report['replay']['committed_state']['agents'][0]['fuel'], 3)
        # A supply leaving that cell simultaneously is not a contact.
        _, report = self.run_cli(m, d, answer=[[2],[2],[-2]], flags=['--allow-synthetic'])
        self.assertEqual(report['replay']['committed_state']['agents'][0]['fuel'], 0)
        self.assertEqual(report['replay']['trace'][1]['refueled_agent_ids'], [])

    def test_pdf_phase_example_acquisitions_fuel_contacts_residence(self):
        m, d = documents(size=4, steps=6, kinds=(0,0,0,0,0,0,0,1), spots=0)
        m['map']['cells'] = [[1,1,0,0]] + [[0]*4 for _ in range(3)]
        m['agents'] = [0,0,0,0,2,3,3,2]
        m['fuelLimits'] = 3
        m['spots'] = [dict(brand=77,pos=2,stocks=4)]
        d['agents'] = [dict(kind=int(i==7),pos=pos,fuel=3) for i,pos in enumerate(m['agents'])]
        d['traffics'] = [dict(pos=0,status=0),dict(pos=1,status=1)]
        answer = [[2,2,2,-1],[-1,2,2,2],[-2,2,2,-1],[-3,2,-2],
                  [5,5,-2],[5,5,-2],[-2,5,5],[5,5,-2]]
        actual, report = self.run_cli(m, d, answer=answer, flags=['--allow-synthetic'])
        self.assertEqual(actual, answer)
        frames = report['replay']['trace']
        self.assertEqual([f['state']['agents'][5]['fuel'] for f in frames], [3,2,2,1,1,1])
        self.assertEqual([f['state']['agents'][5]['cell'] for f in frames], [3,2,2,1,1,1])
        acquired = [(f['step'],a['agent_id']) for f in frames for a in f['acquisitions']]
        self.assertEqual(acquired, [(1,4),(2,5),(3,0),(4,1)])
        final = report['replay']['committed_state']
        self.assertEqual([a['cell'] for a in final['agents']], [3,3,2,1,0,1,1,0])
        self.assertEqual([a['fuel'] for a in final['agents'][:7]], [0,0,1,1,3,1,1])
        self.assertEqual(final['spot_stock'], [0])
        self.assertEqual(report['own_road_residence'][:2], [12,17])
        self.assertEqual(report['objective'], [1,1,4])
        self.assertIn(4, frames[-1]['refueled_agent_ids'])
        self.assertNotIn(5, [p for f in frames for p in f['refueled_agent_ids']])

    def test_pdf_answer_sample_in_compatible_synthetic_geometry(self):
        m, d = documents(size=8, steps=15, kinds=(0,1), spots=0)
        m['agents'] = [27,27]
        m['map']['cells'][3][3] = 2
        d['agents'] = [dict(kind=0,pos=27,fuel=15),dict(kind=1,pos=27,fuel=0)]
        answer = json.loads((FIXTURES/'pdf_answer.json').read_text())
        actual, report = self.run_cli(m, d, answer=answer, flags=['--allow-synthetic'])
        self.assertEqual(actual, answer)
        self.assertTrue(report['replay']['valid'])

    def test_kinds_validation(self):
        m, _ = documents()
        for bad in ([0,1], [0,1,2], [0,True,1], dict(agents=[0,1,0])):
            self.run_cli(m, kinds=bad, success=False)
        good, _ = self.run_cli(m, kinds=[0,1,0])
        self.assertEqual(good, [0,1,0])

    def test_six_directions_on_even_and_odd_rows(self):
        for source, targets in [(19, [11,12,20,28,27,18]), (27, [18,19,28,35,34,26])]:
            for direction, destination in enumerate(targets):
                with self.subTest(source=source, direction=direction):
                    m, d = documents(steps=2, spots=0)
                    d['agents'][0]['pos'] = source
                    _, r = self.run_cli(m, d, answer=[[direction],[-2],[-2]], flags=['--allow-synthetic'])
                    self.assertEqual(r['replay']['committed_state']['agents'][0]['cell'], destination)

    def test_daily_index_and_traffic_change_without_stale_conversion(self):
        m, d = documents()
        m['daySteps'] = [16,32,48,64]
        d['day'] = 2
        d['traffics'][0]['status'] = 2
        h = history(m, d)
        current, _ = self.run_cli(m, d, past=h, flags=['--inspect'])
        self.assertEqual(current['normalized']['day_steps'], 48)
        self.assertEqual(current['normalized']['map']['cells'][61]['road'], 'jammed')
        m['jammedThreshold'] = m['busyThreshold']
        h = history(m, d)
        # Do not invent a strict threshold ordering absent from the PDF.
        self.run_cli(m, d, past=h, flags=['--inspect'])
        d['day'] = 4
        self.assertIn('outside daySteps', self.run_cli(m, d, past=h, flags=['--inspect'], success=False))

    def test_opponents_validated_but_not_mixed_into_own_team(self):
        m, d = documents()
        base, _ = self.run_cli(m, d, answer=[[-16]]*3)
        m['players'] = 2
        d['others'] = [dict(id=900, agents=[dict(kind=1,pos=0,fuel=0)]*3)]
        actual, r = self.run_cli(m, d, answer=[[-16]]*3)
        self.assertEqual(actual, base)
        self.assertEqual(r['replay']['trace'][0]['refueled_agent_ids'], [])
        d['others'][0]['agents'][0]['pos'] = 63
        self.assertIn('traversable', self.run_cli(m, d, answer=actual, success=False))

    def test_original_inputs_are_not_overwritten_by_report(self):
        m, d = documents()
        with tempfile.TemporaryDirectory(prefix='contest_report_guard_') as folder:
            root = Path(folder)
            mp, dp = root/'map.json', root/'day.json'
            mp.write_text(json.dumps(m)); dp.write_text(json.dumps(d))
            proc = subprocess.run([str(EXE), str(mp), str(dp), '--offline', '--report', str(mp)],
                                  text=True, capture_output=True, timeout=5)
            self.assertNotEqual(proc.returncode, 0)
            self.assertEqual(proc.stdout, '')
            self.assertIn('overwrite', proc.stderr)
            self.assertEqual(json.loads(mp.read_text()), m)

    def test_caller_can_shorten_but_not_enlarge_budget(self):
        m, d = documents()
        plan, report = self.run_cli(m, d, flags=['--offline', '--budget-seconds', '0.5'])
        self.assertEqual(plan, [[-16]]*3)
        self.assertEqual(report['requested_seconds'], .5)
        self.assertEqual(report['policy_ceiling_seconds'], 55)
        self.assertEqual(report['search_deadline_seconds'], 0)
        for value in ['-1', '56', 'nan', 'inf', '4garbage']:
            with self.subTest(value=value):
                self.run_cli(m, d, flags=['--offline', '--budget-seconds', value], success=False)


if __name__ == '__main__':
    unittest.main()
