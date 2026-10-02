"""Build the contest solver and optional read-only viewer from this directory."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
CORE = Path('competition/solver')
REPLAY = Path('shared/replay')
BIN = CORE / 'build'


def run(args, env):
    print(' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, env=env, check=True)


def build_engine(env, tests):
    compiler = env.get('CXX', 'g++')
    objects = BIN / 'objects'
    (ROOT / objects).mkdir(parents=True, exist_ok=True)
    flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic',
             '-I', str(CORE), '-I', str(REPLAY / 'include')]
    sources = [CORE / name for name in ('core.cpp', 'candidates.cpp', 'supply.cpp', 'solver.cpp')]
    sources += [REPLAY / 'src' / name for name in ('types.cpp', 'transition.cpp', 'replay.cpp')]
    compiled = {}
    def compile_one(source):
        output = objects / ('_'.join(source.parts).replace('.', '_') + '.o')
        run([compiler, *flags, '-c', source, '-o', output], env)
        return output
    for source in sources:
        compiled[source] = compile_one(source)
    def link(name, units, common):
        extra = [compile_one(source) for source in units]
        run([compiler, *common, *extra, '-o', BIN / (name + '.exe')], env)
    link('contest_solver', [CORE / 'contest/adapter.cpp', CORE / 'contest/main.cpp'],
         list(compiled.values()))
    if tests:
        link('solver', [CORE / 'main.cpp'], list(compiled.values()))
        link('selection_regression', [CORE / 'tests/selection_regression.cpp'],
             [obj for source, obj in compiled.items()
              if source.name not in ('candidates.cpp', 'solver.cpp')])
        run([compiler, '-std=c++17', '-O2', 'viewer/result_viewer/test_model.cpp',
             '-o', BIN / 'viewer_model_test.exe'], env)
        run([BIN / 'selection_regression.exe'], env)
        with tempfile.TemporaryDirectory(prefix='viewer_model_test_') as folder:
            run([BIN / 'viewer_model_test.exe', folder], env)
        run([sys.executable, '-B', '-m', 'unittest', 'discover', '-s',
             CORE / 'tests', '-t', '.', '-v'], env)


def build_viewer(env):
    sdk = env.get('SIV3D_0_6_16')
    if not sdk or not (Path(sdk) / 'include/Siv3D.hpp').is_file():
        raise RuntimeError('Install OpenSiv3D 0.6.16 and set SIV3D_0_6_16')
    vswhere = Path(env.get('PROGRAMFILES(X86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    found = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.Component.MSBuild', '-find', r'MSBuild\**\Bin\MSBuild.exe'],
        env=env, text=True).splitlines()
    if not found:
        raise RuntimeError('Visual Studio 2022 MSBuild not found')
    run([found[0], 'viewer/result_viewer.vcxproj', '/p:Configuration=Release',
         '/p:Platform=x64', '/p:SolutionDir=' + str(ROOT) + os.sep,
         '/v:minimal', '/nologo'], env)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('target', choices=('engine', 'test', 'gui', 'all'), default='engine', nargs='?')
    args = parser.parse_args()
    env = {key.upper(): value for key, value in os.environ.items()}
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    if args.target in ('engine', 'test', 'all'):
        build_engine(env, args.target in ('test', 'all'))
    if args.target in ('gui', 'all'):
        build_viewer(env)


if __name__ == '__main__':
    main()
