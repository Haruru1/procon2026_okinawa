"""Supervisor safety tests; no real competition endpoint is ever contacted."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch, MagicMock

from competition.solver.contest.match_client import (
    Api, Engine, EngineError, Lock, MatchError, MatchRunner, Reply,
    TransportError, WindowClosed, acknowledged, atomic_json, certain_plan, confirm_next, endpoint)
from competition.solver.tests.test_official_format import documents, history


def bundle(setting, daily, answer=None, new_brand=False):
    kinds = [a['kind'] for a in daily['agents']]
    final = [dict(cell=a['pos'], fuel=a['fuel']) for a in daily['agents']]
    h = history(setting, daily)
    h.update(confirmed=False, completed_days=daily['day']+1)
    if new_brand:
        h['collected_brands'][0] = [0]
    return dict(answer=answer or [[-16]]*3, engine_seconds=.01,
                report=dict(objective=[1,1,1] if new_brand else [0,0,0],
                            next_history_draft=h, replay=dict(valid=True,committed_state=dict(agents=final))))


def receipt(b, revision=1, status='accepted'):
    return dict(**copy.deepcopy(b), revision=revision, status=status)


class ScriptApi:
    def __init__(self, daily, results):
        self.daily, self.results, self.posts = daily, list(results), []

    def request(self, method, path, body=None, deadline=None):
        if method == 'GET':
            return Reply(200, self.daily, time.monotonic(), time.time(), .01, 'test')
        self.posts.append(copy.deepcopy(body))
        result = self.results.pop(0)
        if isinstance(result, Exception):
            raise result
        return Reply(200, dict(revision=result), time.monotonic(), time.time(), .01, 'test')


class MatchClientTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='contest_client_')
        self.root = Path(self.temp.name)
        self.m, self.d = documents()
        self.d['endsAt'] = int(time.time())+40

    def tearDown(self):
        self.temp.cleanup()

    def runner(self):
        r = MatchRunner('http://127.0.0.1:18080', 'test-token-only', self.root, kinds=[a['kind'] for a in self.d['agents']])
        r.emit = lambda *_args, **_kwargs: None
        r.setting(self.m)
        record = dict(state=copy.deepcopy(self.d), attempts=[], ambiguous_payload=None,
                      solver_finished=False, next_state_verified=False, acquired_at=time.time())
        r.state['days'] = [record]
        return r, record

    def test_endpoints_do_not_accidentally_reach_remote_or_redirect(self):
        self.assertEqual(endpoint('http://127.0.0.1:18080/'), 'http://127.0.0.1:18080')
        for u in ['http://10.0.0.1', 'http://user:secret@localhost', 'http://localhost/?token=x',
                  'http://localhost/api', 'file:///tmp/x']:
            with self.subTest(url=u), self.assertRaises(MatchError):
                endpoint(u)
        self.assertEqual(endpoint('http://10.0.0.1', True), 'http://10.0.0.1')

    def test_http_clock_bound_uses_monotonic_not_local_wall(self):
        r = Reply(200, {}, 100, 1000001., .1, 'http_date_upper_bound')
        self.assertEqual(r.deadline(1000030), 129)

    def test_atomic_journal_and_single_writer_lock(self):
        path = self.root/'journal.json'
        atomic_json(path, dict(value=1)); atomic_json(path, dict(value=2))
        self.assertEqual(json.loads(path.read_text()), dict(value=2))
        self.assertFalse(list(self.root.glob('*.tmp')))
        with Lock(self.root/'a.lock'):
            with self.assertRaises(MatchError):
                with Lock(self.root/'a.lock'):
                    pass
        with Lock(self.root/'a.lock'):
            pass

    def test_portable_engine_is_beside_executable_not_working_directory(self):
        from competition.solver.contest.match_client import default_engine, DEFAULT_ENGINE
        with patch('sys.frozen', True, create=True), patch('sys.executable', str(self.root/'copy'/'contest_client.exe')):
            self.assertEqual(default_engine(), self.root/'copy'/'contest_solver.exe')
        self.assertEqual(default_engine(), DEFAULT_ENGINE)

    def test_team_lock_is_shared_by_different_portable_copies(self):
        from competition.solver.contest.match_client import team_lock_dir
        with patch.dict(os.environ, {'LOCALAPPDATA': str(self.root/'local_appdata')}):
            with patch('sys.executable', str(self.root/'copy_a'/'contest_client.exe')):
                a = team_lock_dir()
            with patch('sys.executable', str(self.root/'copy_b'/'contest_client.exe')):
                b = team_lock_dir()
            self.assertEqual(a, b)
            with Lock(a/'test.lock'):
                with self.assertRaises(MatchError):
                    with Lock(b/'test.lock'):
                        pass

    def test_confirmation_is_replay_plus_receipt_not_state_inference(self):
        r, record = self.runner()
        b = bundle(self.m, self.d, new_brand=True)
        record['attempts'] = [receipt(b)]
        next_day = copy.deepcopy(self.d); next_day['day'] = 1
        h = confirm_next(self.m, record, next_day)
        self.assertTrue(h['confirmed'])
        self.assertEqual(h['collected_brands'][0], [0])
        self.assertFalse(b['report']['next_history_draft']['confirmed'])
        for key, value in [('pos', 5), ('fuel', 0), ('kind', 1)]:
            bad = copy.deepcopy(next_day); bad['agents'][0][key] = value
            with self.subTest(key=key), self.assertRaises(MatchError):
                confirm_next(self.m, record, bad)
        next_day['day'] = 2
        with self.assertRaises(MatchError): confirm_next(self.m, record, next_day)

    def test_unacknowledged_different_plan_is_ambiguous_even_same_terminal_state(self):
        _, record = self.runner()
        waiting = bundle(self.m, self.d)
        unknown = bundle(self.m, self.d, [[-8,-8],[-16],[-16]], new_brand=True)
        record['attempts'] = [receipt(waiting), receipt(unknown, status='unknown')]
        with self.assertRaises(MatchError): certain_plan(record)
        record['attempts'].append(receipt(unknown, revision=2))
        self.assertEqual(certain_plan(record)['revision'], 2)

    def test_unknown_same_payload_does_not_change_history(self):
        _, record = self.runner()
        b = bundle(self.m, self.d)
        record['attempts'] = [receipt(b, status='intent'), receipt(b, revision=9)]
        self.assertEqual(certain_plan(record)['revision'], 9)

    def test_latest_revision_not_response_order_selects_adopted_answer(self):
        _, record = self.runner()
        old = bundle(self.m, self.d)
        new = bundle(self.m, self.d, [[-8,-8],[-16],[-16]], new_brand=True)
        record['attempts'] = [receipt(new, revision=9), receipt(old, revision=8),
                              receipt(old, revision=-1, status='rejected')]
        self.assertEqual(certain_plan(record)['answer'], new['answer'])
        next_day = copy.deepcopy(self.d)
        next_day['day'] = 1
        self.assertEqual(confirm_next(self.m, record, next_day)['collected_brands'][0], [0])

    def test_rejected_negative_revision_keeps_last_proven_plan(self):
        r, record = self.runner()
        old = bundle(self.m, self.d)
        record['attempts'] = [receipt(old)]
        new = bundle(self.m, self.d, [[-8,-8],[-16],[-16]], True)
        r.api = ScriptApi(self.d, [-1])
        self.assertFalse(r.propose(record, new, time.monotonic()+30, 'test'))
        self.assertEqual(certain_plan(record)['answer'], old['answer'])
        self.assertEqual(record['attempts'][-1]['status'], 'rejected')

    def test_lost_response_retries_only_identical_and_freezes_other_payloads(self):
        r, record = self.runner()
        old = bundle(self.m, self.d)
        record['attempts'] = [receipt(old)]
        new = bundle(self.m, self.d, [[-8,-8],[-16],[-16]], True)
        r.api = ScriptApi(self.d, [TransportError('lost reply'), 3])
        self.assertTrue(r.propose(record, new, time.monotonic()+30, 'test'))
        self.assertEqual(r.api.posts, [new['answer'],new['answer']])
        self.assertEqual(certain_plan(record)['revision'], 3)
        with self.assertRaises(MatchError): r.propose(record, old, time.monotonic()+30, 'test')

    def test_unknown_is_durable_before_restart(self):
        r, record = self.runner()
        b = bundle(self.m, self.d)
        r.api = ScriptApi(self.d, [TransportError('x')]*3)
        self.assertFalse(r.propose(record,b,time.monotonic()+30,'test'))
        saved = json.loads(r.path.read_text())['days'][0]
        self.assertEqual([a['status'] for a in saved['attempts']], ['unknown']*3)
        with self.assertRaises(MatchError): certain_plan(saved)
        restored = MatchRunner('http://127.0.0.1:18080','test-token-only',self.root,supply_count=1)
        self.assertEqual(restored.state['days'][0]['attempts'], record['attempts'])

    def test_deadline_or_changed_day_does_not_send_stale_answer(self):
        r, record = self.runner()
        r.api = ScriptApi(self.d, [])
        self.assertFalse(r.propose(record,bundle(self.m,self.d),time.monotonic()+1,'test'))
        self.assertEqual(r.api.posts, [])
        changed = copy.deepcopy(self.d); changed['day'] = 1
        r.api = ScriptApi(changed, [])
        self.assertFalse(r.propose(record,bundle(self.m,self.d),time.monotonic()+30,'test'))
        self.assertEqual(r.api.posts, [])

    def test_not_sent_is_distinct_from_unknown(self):
        r, record = self.runner()
        r.api = ScriptApi(self.d, [WindowClosed('not enough time')])
        self.assertFalse(r.propose(record,bundle(self.m,self.d),time.monotonic()+30,'test'))
        self.assertEqual(record['attempts'][0]['status'], 'not_sent')
        self.assertIsNone(record['ambiguous_payload'])

    def test_case_and_token_isolation_and_start_schedule_transition(self):
        r, _ = self.runner()
        r.state['setting']['startsAt'] = 0
        r.setting(self.m)
        bad = copy.deepcopy(self.m); bad['startsAt'] += 1
        with self.assertRaises(MatchError): r.setting(bad)
        with self.assertRaises(MatchError): MatchRunner('http://127.0.0.1:18080','other-token',self.root,supply_count=1)

    def test_worker_failure_retains_accepted_fallback(self):
        r, record = self.runner()
        r.api = ScriptApi(self.d, [1])
        r.engine = MagicMock()
        r.engine.evaluate.side_effect = [bundle(self.m,self.d), EngineError('worker crashed')]
        r.day(Reply(200,self.d,time.monotonic(),time.time(),.01,'test'))
        self.assertEqual(certain_plan(record)['answer'], [[-16]]*3)
        self.assertTrue(record['solver_finished'])
        self.assertIn('worker crashed', record['solver_error'])

    def test_worker_watchdog_kills_and_reaps_child(self):
        engine = Engine('fake-engine.exe',self.root/'jobs')
        child = MagicMock()
        child.wait.side_effect = [subprocess.TimeoutExpired('fake',.01), 1]
        with patch('subprocess.Popen', return_value=child):
            with self.assertRaises(subprocess.TimeoutExpired):
                engine.evaluate(self.m,self.d,None,timeout=.01)
        child.kill.assert_called_once()
        self.assertEqual(child.wait.call_count, 2)

    def test_nonzero_worker_exit_is_not_an_answer(self):
        child = MagicMock(); child.wait.return_value = 7
        with patch.dict(os.environ, {'PROCON_TOKEN':'hidden', 'CUSTOM_TOKEN':'also-hidden'}), \
             patch('subprocess.Popen', return_value=child) as spawn:
            with self.assertRaises(EngineError):
                Engine('fake.exe',self.root/'jobs',('CUSTOM_TOKEN',)).evaluate(self.m,self.d,None)
            self.assertNotIn('PROCON_TOKEN',spawn.call_args.kwargs['env'])
            self.assertNotIn('CUSTOM_TOKEN',spawn.call_args.kwargs['env'])

    def test_completed_final_score_is_labeled_prediction_not_confirmed_history(self):
        r, record = self.runner()
        for day in range(4):
            current = copy.deepcopy(self.d); current['day'] = day
            b = bundle(self.m,current)
            row = copy.deepcopy(record)
            row.update(state=current, attempts=[receipt(b)], next_state_verified=day<3)
            row['attempts'][0].update(response_received=time.time(), conservative_margin=5.)
            if day == 0: r.state['days'] = []
            r.state['days'].append(row)
        summary = r.finish()
        self.assertFalse(summary['server_score_available'])
        self.assertFalse(r.state['final_history_draft']['confirmed'])
        self.assertFalse(summary['final_state_independently_verified'])

    def test_startup_seals_zero_start_time_before_first_day(self):
        r, _ = self.runner()
        r.state['days'] = []
        r.state['setting']['startsAt'] = 0
        r.state['type_accepted'] = True
        initial = copy.deepcopy(self.m); initial['startsAt'] = 0
        replies = [Reply(200,initial,time.monotonic(),time.time(),.01,'test'),
                   Reply(200,self.d,time.monotonic(),time.time(),.01,'test'),
                   Reply(200,self.m,time.monotonic(),time.time(),.01,'test')]
        r.get = MagicMock(side_effect=replies)
        self.assertEqual(r.startup().body, self.d)
        self.assertEqual(r.state['setting']['startsAt'], self.m['startsAt'])

    def test_outage_after_rollover_stops_without_fabricating_history(self):
        r, record = self.runner()
        r.save()
        expired = Reply(200,self.d,time.monotonic(),self.d['endsAt']+40,.01,'test')
        r.startup = MagicMock(return_value=expired)
        r.day = MagicMock()
        r.get = MagicMock(return_value=None)
        with patch('time.sleep'), self.assertRaisesRegex(MatchError,'connection unavailable'):
            r.run()
        self.assertIsNone(r.state['history'])


if __name__ == '__main__':
    unittest.main()
