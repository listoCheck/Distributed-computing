"""Compile and exercise the real runtime, IPC framing and lifecycle."""
import os
import pathlib
import re
import signal
import shutil
import subprocess
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ENV = dict(os.environ, LD_LIBRARY_PATH=str(ROOT),
           LD_PRELOAD=str(ROOT / "libruntime.so"))
FLAGS = ["clang", "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-I", str(ROOT)]


def run(command, cwd, timeout=60, env=None, success=True):
    process = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise AssertionError(f"Timeout: {command}")
    if success:
        assert process.returncode == 0 and not err, (command, process.returncode, err, out)
    else:
        assert process.returncode != 0, command
    return out


def verify(binary, balances, directory, transfers):
    n = len(balances)
    out = run([binary, "-p", n, *balances], directory, env=ENV)
    logs = (directory / "events.log").read_text()
    console_events = [line for line in out.splitlines() if re.match(r"^[0-9]+: process ", line)]
    assert sorted(console_events) == sorted(logs.splitlines()), "stdout and event log differ"
    assert len(re.findall(r"has STARTED", logs)) == n
    assert len(re.findall(r"has DONE", logs)) == n
    assert len(re.findall(r"received all STARTED", logs)) == n + 1
    assert len(re.findall(r"received all DONE", logs)) == n + 1
    pipes = (directory / "pipes.log").read_text().splitlines()
    assert len(pipes) == n * (n + 1)
    pairs = {(int(a), int(b)) for a, b in re.findall(r"(\d+) -> (\d+):", "\n".join(pipes))}
    assert pairs == {(a, b) for a in range(n + 1) for b in range(n + 1) if a != b}
    rows = {}
    for line in out.splitlines():
        match = re.match(r"\s*(\d+)\s*\|", line)
        if match:
            rows[int(match[1])] = [int(v.strip()) for v in line.split("|")[1:-1]]
    assert set(rows) == set(range(1, n + 1)), out
    changes = [[] for _ in balances]
    pattern = r"(\d+): process\s+(\d+) (transferred|received) \$\s*(\d+) (?:to|from) process\s+(\d+)"
    outgoing, incoming = [], []
    for t, process, verb, amount, peer in re.findall(pattern, logs):
        t, process, amount, peer = map(int, (t, process, amount, peer))
        changes[process - 1].append((t, -amount if verb == "transferred" else amount))
        if verb == "transferred":
            outgoing.append((process, peer, amount))
        else:
            incoming.append((peer, process, amount))
    assert sorted(outgoing) == sorted(incoming) and len(outgoing) == transfers
    for index, initial in enumerate(balances):
        for t, value in enumerate(rows[index + 1]):
            assert value == initial + sum(delta for when, delta in changes[index] if when <= t), (
                index + 1, t, value, changes[index])
    assert sum(row[-1] for row in rows.values()) == sum(balances)
    assert len({len(row) for row in rows.values()}) == 1


def main():
    with tempfile.TemporaryDirectory(prefix="pa2-tests-") as temporary:
        work = pathlib.Path(temporary)
        for name in ("ipc", "history", "stop"):
            binary = work / name
            sources = [ROOT / "tests" / f"{name}_test.c", ROOT / "ipc.c"]
            if name != "ipc":
                sources += [ROOT / "bank_robbery.c"]
            libraries = [] if name == "ipc" else ["-L", ROOT, "-lruntime"]
            run(FLAGS + sources + libraries + ["-o", binary], work)
            run([binary], work, env=None if name == "ipc" else ENV, timeout=15)
            print(f"PASS: {name}", flush=True)
        # Exactly the bot's compilation flags, followed by strict build above.
        original = work / "bank-original"
        run(["clang", "-g", "-Wall", "-pedantic", "-std=c99",
             *sorted(ROOT.glob("*.c")), "-L", ROOT, "-lruntime", "-o", original], work)
        cases = 0
        for n in range(1, 11):
            for initial in (1, 99):
                verify(original, [initial] * n, work, 2 if n >= 2 else 0)
                cases += 1
        for empty in (False, True):
            binary = work / ("empty" if empty else "ring")
            run(FLAGS + (["-DEMPTY_SCENARIO"] if empty else []) +
                [ROOT / "main.c", ROOT / "ipc.c", ROOT / "tests/scenario.c",
                 "-L", ROOT, "-lruntime", "-o", binary], work)
            for n in (1, 2, 3, 5, 10):
                verify(binary, [10 + i for i in range(n)], work,
                       0 if empty or n == 1 else n * 3)
                cases += 1
        for args in ([], ["-p", "0"], ["-p", "11"], ["-p", "2", "10"],
                     ["-p", "1x", "10"], ["-p", "1", "0"], ["-p", "1", "100"],
                     ["-p", "1", "10x"], ["-p", "1", "10", "20"]):
            run([original, *args], work, env=ENV, success=False)
        print(f"PASS: {cases} bank scenarios, 9 invalid arguments, every history cell", flush=True)
        archive = ROOT / "pa2.tar.gz"
        if archive.exists():
            with tarfile.open(archive) as package:
                members = package.getmembers()
                assert all(member.isfile() and member.name.startswith("pa2/") and
                           len(pathlib.PurePosixPath(member.name).parts) == 2
                           for member in members)
                for member in members:
                    assert package.extractfile(member).read() == (
                        ROOT / pathlib.PurePosixPath(member.name).name).read_bytes(), member.name
                package.extractall(work)
            extracted = work / "pa2"
            shutil.copy2(ROOT / "libruntime.so", extracted / "libruntime.so")
            run(["clang", "-g", "-Wall", "-pedantic", "-std=c99",
                 *sorted(extracted.glob("*.c")), "-L", extracted, "-lruntime",
                 "-o", extracted / "pa2"], extracted)
            verify(extracted / "pa2", [10, 20, 30], extracted, 2)
            print("PASS: packaged files match, extracted archive compiles and runs", flush=True)


if __name__ == "__main__":
    main()
