import fcntl
import os
from pathlib import Path
import subprocess
import sys


def prepare_sdk(output, lock_dir):
    builder = os.environ.get('CHRONOLOG_SDK_BUILD_PYTHON', sys.executable)
    check = ('import importlib.util, pathlib, sys, sysconfig; '
             'assert sys.version_info >= (3, 12); '
             'assert all(importlib.util.find_spec(name) is not None for name in '
             '("build", "scikit_build_core", "nanobind")); '
             'assert (pathlib.Path(sysconfig.get_path("include")) / "Python.h").is_file()')
    try:
        available = subprocess.run([builder, '-c', check], capture_output=True, text=True, timeout=10)
    except OSError as error:
        print('SKIP SDK wheel toolchain unavailable: ' + str(error))
        raise SystemExit(77) from error
    if available.returncode:
        print('SKIP SDK wheel requires build, scikit-build-core, nanobind and Python 3.12 headers in ' + builder)
        raise SystemExit(77)
    output.mkdir(parents=True, exist_ok=True)
    lock_dir.mkdir(parents=True, exist_ok=True)
    project = Path(__file__).resolve().parents[3] / 'client/python'
    # The SDK backend uses one build/python cache, even when wheel outputs are separate.
    with (lock_dir / 'sdk-build.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        result = subprocess.run([builder, '-m', 'build', '--wheel', '--no-isolation',
                                 '--outdir', str(output), str(project)],
                                capture_output=True, text=True, timeout=900)
    if result.returncode:
        raise RuntimeError('FAIL SDK wheel build\n' + result.stdout + result.stderr)
    return next(output.glob('chronolog-*.whl'))
