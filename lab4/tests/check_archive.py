"""Compile and test the actual submission, without --trace or output cleanup."""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='pa4-archive-') as directory:
    with tarfile.open(str(root / 'pa4.tar.gz')) as archive:
        for member in archive.getmembers():
            parts = Path(member.name).parts
            assert len(parts) == 2 and parts[0] == 'pa4' and member.isfile()
            assert parts[1] not in ('.', '..')
            archive.extract(member, directory)
    work = Path(directory) / 'pa4'
    shutil.copyfile(str(root / 'pa4/libruntime.so'), str(work / 'libruntime.so'))
    command = [os.environ.get('CC', 'clang'), '-std=c99', '-Wall', '-pedantic']
    command += [str(p) for p in sorted(work.glob('*.c'))] + ['-L.', '-lruntime']
    build = subprocess.run(command, cwd=str(work), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert build.returncode == 0 and not build.stderr, build.stderr
    env = dict(os.environ, LD_LIBRARY_PATH=str(work), LD_PRELOAD=str(work / 'libruntime.so'))
    for key in ('PA45_SILENT', 'PA_RT_DEBUG', 'PA_STRACE_MODE'):
        env.pop(key, None)
    run = subprocess.run(['./a.out', '-p', '9', '--mutexl'], cwd=str(work), env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True, timeout=120)
    expected = ['process %d is doing %d iteration out of %d' % (p, i, p * 5)
                for p in range(1, 10) for i in range(1, p * 5 + 1)]
    assert run.returncode == 0
    assert sorted(run.stderr.splitlines()) == sorted(expected), run.stderr
    assert sorted(run.stdout.splitlines()) == sorted((work / 'events.log').read_text().splitlines())
    assert not (work / 'mutex.trace').exists()
    assert not (work / 'mutex.stats.csv').exists()
    print('PASS: extracted archive, PDF compiler flags, 9 workers, 225 exact iteration lines, no --trace')
