#!/usr/bin/env python3
"""Linux checks against the supplied runtime, plus an independent ledger oracle."""
import argparse
import os
from pathlib import Path
import random
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "pa3"
FLAGS = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic"]


def command(args, cwd, env=None, success=True):
    process = subprocess.Popen(args, cwd=str(cwd), env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, universal_newlines=True,
                               start_new_session=True)
    try:
        out, err = process.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.communicate()
        raise AssertionError("Timeout: " + " ".join(map(str, args)))
    if success:
        assert process.returncode == 0, (args, process.returncode, err, out[-2000:])
        assert err == "", (args, err)
    else:
        assert process.returncode != 0, args
    return out


def audit(output, balances, orders, directory):
    """Derive every balance/pending cell from the observable transfer events."""
    rows = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\d+)\s*\|", line)
        if match:
            cells = []
            for cell in line.split("|")[1:-1]:
                values = re.fullmatch(r"\s*(-?\d+)\s*(?:\((-?\d+)\))?\s*", cell)
                assert values, cell
                cells.append((int(values.group(1)), int(values.group(2) or 0)))
            rows[int(match.group(1))] = cells
    assert set(rows) == set(range(1, len(balances) + 1)), output
    length = len(rows[1])
    assert length and all(len(row) == length for row in rows.values())
    outgoing = [(int(t), int(src), int(amount), int(dst)) for t, src, amount, dst in
        re.findall(r"(\d+): process\s+(\d+) transferred \$\s*(\d+) to process\s+(\d+)", output)]
    incoming = [(int(t), int(dst), int(amount), int(src)) for t, dst, amount, src in
        re.findall(r"(\d+): process\s+(\d+) received \$\s*(\d+) from process\s+(\d+)", output)]
    outgoing.sort()
    incoming.sort()
    assert len(outgoing) == len(incoming) == len(orders)
    expected = {i: [[balance, 0] for _ in range(length)]
                for i, balance in enumerate(balances, 1)}
    for (src, dst, amount), sent, received in zip(orders, outgoing, incoming):
        ts, actual_src, actual_amount, actual_dst = sent
        tr, recv_dst, recv_amount, recv_src = received
        assert (actual_src, actual_dst, actual_amount) == (src, dst, amount)
        assert (recv_src, recv_dst, recv_amount) == (src, dst, amount)
        assert ts < tr < length
        for time in range(ts, length):
            expected[src][time][0] -= amount
        for time in range(ts, tr):
            expected[dst][time][1] += amount
        for time in range(tr, length):
            expected[dst][time][0] += amount
    for account in rows:
        assert rows[account] == [tuple(cell) for cell in expected[account]], account
    for time in range(length):
        assert sum(sum(row[time]) for row in rows.values()) == sum(balances)
    with (directory / "events.log").open() as log_file:
        logs = log_file.read()
    assert len(re.findall("has STARTED", logs)) == len(balances)
    assert len(re.findall("has DONE", logs)) == len(balances)
    assert len(re.findall("received all STARTED", logs)) == len(balances) + 1
    assert len(re.findall("received all DONE", logs)) == len(balances) + 1
    for line in logs.splitlines():
        assert line in output, line
    for account in range(len(balances) + 1):
        times = [int(t) for t in re.findall(
            r"(?m)^(\d+): process\s+" + str(account) + r"\b", logs)]
        assert times == sorted(times), (account, times)
    with (directory / "pipes.log").open() as pipe_file:
        pipes = pipe_file.read().splitlines()
    assert len(pipes) == len(balances) * (len(balances) + 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    flags = FLAGS + (["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                     if args.sanitize else [])
    with tempfile.TemporaryDirectory(prefix="pa3-tests-") as temp:
        work = Path(temp)
        shutil.copy2(str(SOURCE / "libruntime.so"), str(work))
        env = dict(os.environ, LD_LIBRARY_PATH=str(work), LD_PRELOAD=str(work / "libruntime.so"))
        sources = [str(path) for path in sorted(SOURCE.glob("*.c"))]
        command([args.cc] + flags + sources + ["-L" + str(work), "-lruntime", "-o", "official"], work)
        command([args.cc] + flags + ["-I" + str(SOURCE), str(ROOT / "tests/probe.c")] +
                [str(SOURCE / name) for name in ("ipc.c", "clock.c", "history.c")] +
                ["-o", "probe"], work)
        print(command(["./probe"], work).strip())
        command(["./probe", "overflow"], work, success=False)
        total = 0
        for count in (1, 2, 3, 10, 15):
            balances = [1 + (i * 17) % 99 for i in range(count)]
            orders = [(src, src % count + 1, 1) for src in range(1, count + 1)] if count > 1 else []
            for _ in range(args.repeats):
                out = command(["./official", "-p", str(count)] + list(map(str, balances)), work, env)
                audit(out, balances, orders, work)
                total += 1
        # Replacing bank_robbery mirrors the bot's interface, with seeded legal orders.
        base = [path for path in sources if not path.endswith("bank_robbery.c")]
        for seed, count, transfers in ((11, 2, 0), (23, 2, 35), (37, 3, 30),
                                        (41, 10, 30), (53, 15, 30)):
            rng = random.Random(seed)
            balances = [rng.randint(1, 99) for _ in range(count)]
            current = list(balances)
            orders = []
            for _ in range(transfers):
                src, dst = rng.sample(range(1, count + 1), 2)
                amount = rng.randint(0, current[src - 1])
                orders.append((src, dst, amount))
                current[src - 1] -= amount
                current[dst - 1] += amount
            scenario = work / "scenario.c"
            with scenario.open("w") as scenario_file:
                scenario_file.write('#include "banking.h"\nvoid bank_robbery(void *p, local_id n) {\n'
                                    '    (void)p; (void)n;\n' + ''.join(
                    '    transfer(p, {}, {}, {});\n'.format(src, dst, amount)
                    for src, dst, amount in orders) + '}\n')
            command([args.cc] + flags + ["-I" + str(SOURCE)] + base + [str(scenario),
                     "-L" + str(work), "-lruntime", "-o", "scenario"], work)
            for _ in range(args.repeats):
                out = command(["./scenario", "-p", str(count)] + list(map(str, balances)), work, env)
                audit(out, balances, orders, work)
                total += 1
        for arguments in ([], ["-p", "0"], ["-p", "2", "10"],
                          ["-p", "2", "10x", "20"], ["-p", "2", "0", "20"]):
            command(["./official"] + arguments, work, env, success=False)
        print("PASS: {} multiprocess runs, exact history oracle, logs, argument errors".format(total)
              + (", ASan + UBSan" if args.sanitize else ""))


if __name__ == "__main__":
    main()
