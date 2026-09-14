"""Linux integration checks against the supplied runtime, no Python packages."""
import collections
import argparse
import csv
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / 'pa4' / 'pa4'
ENV = dict(os.environ, LD_LIBRARY_PATH=str(PROGRAM.parent))
for variable in ('PA45_SILENT', 'PA_RT_DEBUG', 'PA_STRACE_MODE', 'LD_PRELOAD'):
    ENV.pop(variable, None)


def check(count, mutex, silent=False):
    with tempfile.TemporaryDirectory(prefix='pa4-test-') as directory:
        command = [str(PROGRAM), '-p', str(count), '--trace', '--stats']
        if mutex:
            command.append('--mutexl')
        environment = dict(ENV, LD_PRELOAD=str(PROGRAM.parent / 'libruntime.so'))
        if silent:
            environment['PA45_SILENT'] = '1'
        result = subprocess.run(command, cwd=directory, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=120, universal_newlines=True)
        assert result.returncode == 0, (command, result.returncode, result.stderr)
        path = Path(directory)
        log = (path / 'events.log').read_text()
        assert log.count('has STARTED') == count
        assert log.count('has DONE') == count
        assert log.count('received all STARTED') == count + 1
        assert log.count('received all DONE') == count + 1
        assert len((path / 'pipes.log').read_text().splitlines()) == count * (count + 1)
        # The supplied print() writes to fd 2. Its output must stay there:
        # redirecting it makes the verifier's captured work stream empty.
        assert sorted(result.stdout.splitlines()) == sorted(log.splitlines())
        work = result.stderr
        expected = ''.join('process %d is doing %d iteration out of %d\n' % (p, i, p * 5)
                           for p in range(1, count + 1) for i in range(1, p * 5 + 1))
        if silent:
            assert not work, work
        else:
            assert collections.Counter(work) == collections.Counter(expected), 'Wrong runtime output on stderr'
        trace = (path / 'mutex.trace').read_text().splitlines()
        waits = collections.defaultdict(list)
        request_times = {}
        if not mutex:
            assert not trace
        else:
            if not silent:
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
                    request_times[peer] = clock
                elif action == 'ENTER':
                    assert owner is None, ('overlap', line, owner)
                    assert requests[peer] == iteration
                    owner = (peer, iteration)
                    waits[peer].append(clock - request_times[peer])
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
        with (path / 'mutex.stats.csv').open() as stream:
            rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(stream)]
        assert len(rows) == count
        assert sorted(row['id'] for row in rows) == list(range(1, count + 1))
        total_entries = 5 * count * (count + 1) // 2
        for row in rows:
            peer = row['id']
            entries = peer * 5 if mutex else 0
            own_broadcasts = entries * (count - 1)
            other_requests = total_entries - entries if mutex else 0
            assert row['entries'] == entries
            assert row['request_sent'] == row['release_sent'] == row['reply_received'] == own_broadcasts
            assert row['request_received'] == row['release_received'] == row['reply_sent'] == other_requests
            assert row['wait_ticks'] == sum(waits[peer])
            assert row['max_wait_ticks'] == max(waits[peer], default=0)
        print('OK: p=%d mutex=%s silent=%s' % (count, mutex, silent), flush=True)


parser = argparse.ArgumentParser()
parser.add_argument('--counts', type=int, nargs='+', default=[1, 2, 5, 9, 15])
args = parser.parse_args()
for n in args.counts:
    for enabled in (False, True):
        check(n, enabled)
check(9, True, silent=True)
print('All %d integration cases passed, including message counts and wait statistics.' % (2 * len(args.counts) + 1), flush=True)
