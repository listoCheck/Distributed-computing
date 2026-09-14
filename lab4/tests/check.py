"""Linux integration checks against the supplied runtime, no Python packages."""
import collections
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / 'pa4' / 'pa4'
ENV = dict(os.environ, LD_LIBRARY_PATH=str(PROGRAM.parent))


def check(count, mutex):
    with tempfile.TemporaryDirectory(prefix='pa4-test-') as directory:
        command = [str(PROGRAM), '-p', str(count), '--trace']
        if mutex:
            command.append('--mutexl')
        result = subprocess.run(command, cwd=directory, env=ENV,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=120, universal_newlines=True)
        assert result.returncode == 0, (command, result.returncode, result.stderr)
        assert not result.stderr, result.stderr
        path = Path(directory)
        log = (path / 'events.log').read_text()
        assert log.count('has STARTED') == count
        assert log.count('has DONE') == count
        assert log.count('received all STARTED') == count + 1
        assert log.count('received all DONE') == count + 1
        assert len((path / 'pipes.log').read_text().splitlines()) == count * (count + 1)
        # Lifecycle messages are atomic writes, but can interrupt runtime's
        # character-by-character output. Remove them before examining work.
        work = result.stdout
        for line in log.splitlines(True):
            assert line in work, line
            work = work.replace(line, '', 1)
        expected = ''.join('process %d is doing %d iteration out of %d\n' % (p, i, p * 5)
                           for p in range(1, count + 1) for i in range(1, p * 5 + 1))
        assert collections.Counter(work) == collections.Counter(expected)
        trace = (path / 'mutex.trace').read_text().splitlines()
        if not mutex:
            assert not trace
        else:
            assert sorted(work.splitlines()) == sorted(expected.splitlines())
            owner = None
            requests, completed, clocks = {}, collections.Counter(), {}
            for line in trace:
                action, peer, clock, iteration = line.split()
                peer, clock, iteration = int(peer), int(clock), int(iteration)
                assert clock >= clocks.get(peer, 0)
                clocks[peer] = clock
                if action == 'REQUEST':
                    assert peer not in requests
                    requests[peer] = iteration
                elif action == 'ENTER':
                    assert owner is None, ('overlap', line, owner)
                    assert requests[peer] == iteration
                    owner = (peer, iteration)
                elif action == 'EXIT':
                    assert owner == (peer, iteration)
                    assert iteration == completed[peer] + 1
                    completed[peer] += 1
                    del requests[peer]
                    owner = None
                else:
                    raise AssertionError(line)
            assert owner is None and not requests
            assert completed == {p: p * 5 for p in range(1, count + 1)}
        print('OK: p=%d mutex=%s' % (count, mutex), flush=True)


for n in (1, 2, 5, 15):
    for enabled in (False, True):
        check(n, enabled)
print('All 8 integration cases passed.', flush=True)
