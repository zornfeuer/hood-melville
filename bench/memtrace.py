#!/usr/bin/env python3
"""След памяти процесса во времени (#14, пул узлов — docs/pool.md).

Запускает бинарник нагрузки и, пока он работает, опрашивает его память
(proc_pid_rusage) — снаружи, одинаково для C, Rust и Zig: резидентный
размер (RSS) и phys_footprint (учёт памяти процесса в macOS, по нему же
пик в compare.py). Нагрузка идёт с FQ_PHASES=1, и на каждой метке конца
фазы в stderr память снимается сразу: для `wave` — пик (grow) и дно (shrink,
живых узлов N/100) каждой волны. Печатает медианы по повторам (МиБ).

Обе метрики нужны: системный malloc (xzone) после освобождения помечает
страницы переиспользуемыми — они остаются резидентными, но выпадают из
footprint (система заберёт их при нехватке), причём и после повторного
использования; пул держит страницы занятыми в обеих метриках.

usage:
  memtrace.py [--repeat K] [--interval MS] [--env NAME:VAR=VALUE ...]
              --case wave:2000000 NAME=PATH [NAME=PATH ...]
"""

import argparse
import ctypes
import json
import os
import statistics as st
import subprocess
import threading
import time

_libc = ctypes.CDLL('/usr/lib/libSystem.dylib', use_errno=True)
RUSAGE_INFO_V4 = 4
RI_RESIDENT_SIZE, RI_PHYS_FOOTPRINT = 8, 9  # поля rusage_info_v4 (по 8 байт)


def footprint(pid):
    """(resident, phys_footprint), байт; None, если процесса уже нет."""
    ri = (ctypes.c_uint64 * 64)()
    if _libc.proc_pid_rusage(pid, RUSAGE_INFO_V4, ctypes.byref(ri)) != 0:
        return None
    return ri[RI_RESIDENT_SIZE], ri[RI_PHYS_FOOTPRINT]


def trace(path, scenario, n, env, interval):
    """Опрос следа раз в interval мс и снимок на каждой метке фазы.

    Нагрузка запускается с FQ_PHASES=1 и пишет в stderr «phase NAME …» в
    конце каждой фазы; читающий поток снимает след в тот же момент. Для `wave`
    это пик (grow) и дно (shrink) каждой волны.
    """
    t0 = time.perf_counter()
    proc = subprocess.Popen(
        [path, scenario, str(n)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, **(env or {}), 'FQ_PHASES': '1'},
    )
    marks = []

    def read_marks():
        for line in proc.stderr:
            parts = line.decode().split()
            if len(parts) >= 2 and parts[0] == 'phase':
                f = footprint(proc.pid)
                marks.append((parts[1], f))

    reader = threading.Thread(target=read_marks)
    reader.start()
    samples = []
    while proc.poll() is None:
        f = footprint(proc.pid)
        if f and f[0]:
            samples.append((time.perf_counter() - t0, f))
        time.sleep(interval / 1e3)
    reader.join()
    out = proc.stdout.read().decode().strip()
    if proc.returncode != 0:
        raise SystemExit(f'{path} {scenario} {n}: код {proc.returncode}')
    return samples, marks, out, (time.perf_counter() - t0) * 1e3


def summarize(samples, marks, ms):
    mib = 2**20
    s = {'ms': ms}
    for i, key in ((0, 'rss'), (1, 'fp')):
        s[f'{key}_peak'] = max(f[i] for _, f in samples) / mib if samples else 0.0
        grow = [f[i] / mib for name, f in marks if name == 'grow' and f]
        shrink = [f[i] / mib for name, f in marks if name == 'shrink' and f]
        if grow and shrink:
            s[f'{key}_grow_last'] = grow[-1]
            s[f'{key}_trough_last'] = shrink[-1]
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repeat', type=int, default=3)
    ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--case', action='append', required=True)
    ap.add_argument('--env', action='append', default=[], metavar='NAME:VAR=VALUE')
    ap.add_argument('--out', default=None, help='JSON со всеми следами')
    ap.add_argument('bins', nargs='+')
    a = ap.parse_args()
    bins = [b.split('=', 1) for b in a.bins]
    envs = {}
    for e in a.env:
        name, kv = e.split(':', 1)
        var, val = kv.split('=', 1)
        envs.setdefault(name, {})[var] = val
    raw = {}
    for case in a.case:
        scenario, n = case.split(':')
        print(f'\n## {case}\n')
        print(
            '| бинарник | RSS пик | RSS пик посл. волны | RSS дно посл. волны | '
            'footprint пик | footprint пик посл. | footprint дно посл. | мс |'
        )
        print('|---|---:|---:|---:|---:|---:|---:|---:|')
        sums = set()
        for name, path in bins:
            runs = []
            for _ in range(a.repeat):
                samples, marks, out, ms = trace(path, scenario, n, envs.get(name), a.interval)
                sums.add(out)
                raw.setdefault(case, {}).setdefault(name, []).append(
                    {'samples': samples, 'marks': marks}
                )
                runs.append(summarize(samples, marks, ms))
            med = {k: st.median(r[k] for r in runs) for k in runs[0]}
            keys = (
                'rss_peak',
                'rss_grow_last',
                'rss_trough_last',
                'fp_peak',
                'fp_grow_last',
                'fp_trough_last',
            )
            cells = [f"{med.get(k, float('nan')):.1f}" for k in keys]
            print(f"| {name} | " + ' | '.join(cells) + f" | {med['ms']:.0f} |")
        if len(sums) != 1:
            raise SystemExit(f'{case}: контрольные суммы различаются: {sums}')
    if a.out:
        with open(a.out, 'w') as f:
            json.dump(raw, f)


if __name__ == '__main__':
    main()
