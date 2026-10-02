"""Portable small fixtures and replay runner. No external checkout is required."""
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parents[1]
EXE = HERE / 'build/solver.exe'


def cpp(case, **options):
    result = subprocess.run([str(EXE)], input=json.dumps(dict(case=case, **options)),
                            text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


def rows():
    return json.loads((HERE / 'tests/fixtures/small_exact.json').read_text(encoding='utf-8'))


def suite():
    return [('saved_exact', row['case'], row['objective'], row['plan']) for row in rows()]


class ReplayBridge:
    """Invoke the shared C++ replay; not a separate optimality proof or implementation."""
    def evaluate(self, case, plans, include_existing=False):
        if include_existing:
            raise ValueError('comparison implementations are not part of this distribution')
        return [dict(replay=cpp(case, mode='replay', plan=plan)) for plan in plans]
