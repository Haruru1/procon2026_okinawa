"""Stable-plan snapshots and standalone client/engine connection checks."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from competition.solver.contest import match_client as client
from competition.solver.tests.support import HERE, cpp, rows
from competition.solver.tests.test_official_format import documents


class ReleaseTests(unittest.TestCase):
    def test_supply_selection_preserves_existing_implementation(self):
        data = json.loads((HERE / 'tests/fixtures/supply_selection.json').read_text(encoding='utf-8'))
        for row in data:
            with self.subTest(spots=len(row['setting']['spots']), count=row['supply_count']):
                kinds, details = client.choose_supply_layout(row['setting'], row['supply_count'])
                self.assertEqual(kinds, row['kinds'])
                self.assertEqual(json.loads(json.dumps(details)), row['details'])

    def test_small_exact_27_stable_complete_outputs(self):
        cases = rows()
        self.assertEqual(len(cases), 27)
        for row in cases:
            with self.subTest(case=row['case']['id']):
                result = cpp(row['case'], seconds=55)
                self.assertTrue(result['completed'])
                self.assertEqual(result['objective'], row['objective'])
                self.assertTrue(result['replay']['valid'])
                for key, value in row['stable_result'].items():
                    self.assertEqual(result[key], value, key)

    def test_client_help_and_explicit_engine(self):
        result = subprocess.run([sys.executable, '-B', str(HERE / 'contest/match_client.py'), '--help'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--engine', result.stdout)
        self.assertIn('--supply-count', result.stdout)
        executable = HERE / 'build/contest_solver.exe'
        self.assertEqual(client.DEFAULT_ENGINE, executable)
        with tempfile.TemporaryDirectory(prefix='contest_client_test_') as temp:
            setting, daily = documents()
            engine = client.Engine(executable, Path(temp) / 'jobs')
            output = engine.evaluate(setting, daily, None, timeout=10)
            self.assertTrue(output['report']['replay']['valid'])
            self.assertEqual(len(output['answer']), len(daily['agents']))
            with patch.object(client, 'MatchRunner') as runner, \
                 patch.dict('os.environ', {'PROCON_TOKEN': 'local-test-only'}), \
                 contextlib.redirect_stdout(io.StringIO()):
                runner.return_value.run.return_value = {}
                code = client.main(['--base-url', 'http://127.0.0.1:18080',
                                    '--run-dir', str(Path(temp) / 'match'), '--supply-count', '1',
                                    '--engine', str(executable)])
                self.assertEqual(code, 0)
                self.assertEqual(Path(runner.call_args.kwargs['engine'].executable), executable)


if __name__ == '__main__':
    unittest.main()
