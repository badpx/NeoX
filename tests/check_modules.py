#!/usr/bin/env python3
"""Build supported frontend/Text combinations and consume relocated installations.
Run outside the source tree; Python is a developer test tool, not a dependency.
"""
import argparse
import itertools
import json
import pathlib
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--workdir', required=True)
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parents[1]
work = pathlib.Path(args.workdir).resolve()
work.mkdir(parents=True, exist_ok=True)

def run(command, log, expect=0):
    result = subprocess.run([str(c) for c in command], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log.write('$ ' + ' '.join(map(str, command)) + '\n' + result.stdout + '\n')
    log.flush()
    if (result.returncode == 0) != (expect == 0):
        raise RuntimeError(f'Unexpected result {result.returncode}: {command}\n{result.stdout}')
    return result.stdout

common = ['-DNEOX_BACKEND_GL=OFF', '-DNEOX_BACKEND_METAL=OFF', '-DNEOX_PRESENT=OFF',
          '-DNEOX_SINGLE_THREADED=ON', '-DNEOX_WARNINGS_AS_ERRORS=ON',
          '-DNEOX_SANITIZE=ON', '-DCMAKE_BUILD_TYPE=Debug']
for scene, capture, text in itertools.product(('OFF', 'ON'), repeat=3):
    name = f'scene-{scene}_capture-{capture}_text-{text}'
    build = work / name
    with (work / (name + '.log')).open('w') as log:
        run(['cmake', '-S', source, '-B', build, *common,
             f'-DNEOX_WITH_SCENE={scene}', f'-DNEOX_WITH_CAPTURE={capture}', f'-DNEOX_WITH_TEXT={text}',
             '-DNEOX_BUILD_TESTS=ON', '-DNEOX_BUILD_EXAMPLES=OFF'], log)
        run(['cmake', '--build', build, '--parallel', '4'], log)
        run(['ctest', '--test-dir', build, '--output-on-failure'], log)
        objects = run(['ar', '-t', build / 'libneox.a'], log)
        assert ('scene.c.o' in objects) == (scene == 'ON')
        assert ('capture.c.o' in objects) == (capture == 'ON')
        # A required disabled component must fail at package discovery, not link.
        disabled = [k for k, v in [('Scene', scene), ('Capture', capture), ('Text', text)] if v == 'OFF']
        if disabled:
            prefix = build / 'consumer-check/relocated'
            run(['cmake', '-S', source / 'tests/consumer', '-B', build / 'missing-component',
                 f'-DCMAKE_PREFIX_PATH={prefix}', f'-DNEOX_CONSUMER_COMPONENTS={disabled[0]}'], log, expect=1)
    print(name, 'PASS', flush=True)

# Libraries only and examples only are real independently selectable builds.
for examples in ('OFF', 'ON'):
    build = work / f'no-tests-examples-{examples}'
    with (work / f'no-tests-examples-{examples}.log').open('w') as log:
        run(['cmake', '-S', source, '-B', build, *common, '-DNEOX_WITH_SCENE=OFF',
             '-DNEOX_WITH_CAPTURE=OFF', '-DNEOX_WITH_TEXT=OFF', '-DNEOX_BUILD_TESTS=OFF',
             f'-DNEOX_BUILD_EXAMPLES={examples}'], log)
        run(['cmake', '--build', build, '--parallel', '4'], log)
        inventory = json.loads(run(['ctest', '--test-dir', build, '--show-only=json-v1'], log))
        assert not inventory['tests']
        assert (build / 'ex01_scene').exists() == (examples == 'ON')
    print(f'no-tests-examples-{examples}', 'PASS', flush=True)

# Embedded add_subdirectory use defaults to libraries only and propagates public
# generated features, optional module includes, and static math dependencies.
with (work / 'subdirectory.log').open('w') as log:
    build = work / 'subdirectory'
    run(['cmake', '-S', source / 'tests/consumer', '-B', build, f'-DNEOX_SOURCE={source}', *common,
         '-DNEOX_WITH_SCENE=OFF', '-DNEOX_WITH_CAPTURE=OFF'], log)
    run(['cmake', '--build', build, '--parallel', '4'], log)
    run(['ctest', '--test-dir', build, '--output-on-failure'], log)
    cache = (build / 'CMakeCache.txt').read_text()
    assert 'NEOX_BUILD_TESTS:BOOL=OFF' in cache and 'NEOX_BUILD_EXAMPLES:BOOL=OFF' in cache
print('subdirectory', 'PASS', flush=True)
