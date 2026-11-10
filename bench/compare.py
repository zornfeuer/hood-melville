#!/usr/bin/env python3
"""Сравнение бинарников fqueue_bench с учётом режимов аллокатора.

Методика (почему так — см. docs/benchmarks.md, раздел «Методика»):
- запуски чередуются: в каждом раунде все бинарники в случайном порядке, чтобы
  медленное состояние системы не доставалось целиком одной реализации;
- для каждого запуска снимаются такты, инструкции, page reclaims и пиковая
  память (`/usr/bin/time -l`) и время (perf_counter вокруг процесса);
- на больших N запуски одного бинарника распадаются на режимы с разным IPC
  (раскладка кучи libmalloc); режимы выделяются по наибольшему разрыву в
  отсортированных IPC и описываются отдельно — несовместимые распределения
  не усредняются;
- отношение реализаций — отношение медиан тактов внутри одного режима и по
  всем запускам, с бутстреп-интервалом 95%.

usage:
  compare.py [--rounds N] [--warmup K] [--seed S] [--out DIR]
             --case fill:1000000 [--case ...] NAME=PATH [NAME=PATH ...]
Первый бинарник — база для отношений.

Имя вида IMPL/LAYOUT (например, C/al64) означает вариант раскладки кода
реализации IMPL. Если у реализации несколько раскладок, печатается сводка по
ним: доля быстрого режима и медианы внутри режимов по всем раскладкам, а
отношения реализаций считаются по объединённым запускам. Склонность к
медленному режиму на mixed 10^7 определяется раскладкой конкретной сборки, а
не кодом очереди, поэтому выводы о реализациях делаются только по нескольким
раскладкам.
"""
import argparse
import json
import os
import random
import re
import statistics as st
import subprocess
import time

TIME_FIELDS = {
    'cycles': r'(\d+)\s+cycles elapsed',
    'instructions': r'(\d+)\s+instructions retired',
    'reclaims': r'(\d+)\s+page reclaims',
    'peak': r'(\d+)\s+peak memory footprint',
}
MODE_GAP = 1.12  # соседние IPC, различающиеся сильнее, — разные режимы


def run_once(path, scenario, n):
    t0 = time.perf_counter()
    proc = subprocess.run(['/usr/bin/time', '-l', path, scenario, str(n)],
                          capture_output=True, text=True, check=True)
    wall = time.perf_counter() - t0
    m = {k: int(re.search(rx, proc.stderr).group(1)) for k, rx in TIME_FIELDS.items()}
    m['wall_ms'] = wall * 1e3
    m['ipc'] = m['instructions'] / m['cycles']
    m['checksum'] = proc.stdout.strip()
    return m


def split_modes(runs):
    """Разбить запуски на режимы по разрывам в отсортированных IPC."""
    rs = sorted(runs, key=lambda r: r['ipc'])
    modes, cur = [], [rs[0]]
    for a, b in zip(rs, rs[1:]):
        if b['ipc'] / a['ipc'] > MODE_GAP:
            modes.append(cur)
            cur = []
        cur.append(b)
    modes.append(cur)
    return modes[::-1]  # быстрый режим первым


def q(xs, p):
    xs = sorted(xs)
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def describe(runs, key='cycles'):
    xs = [r[key] for r in runs]
    return {
        'n': len(xs), 'median': st.median(xs), 'p25': q(xs, .25), 'p75': q(xs, .75),
        'min': min(xs), 'max': max(xs),
        'cv': st.stdev(xs) / st.mean(xs) if len(xs) > 1 else 0.0,
    }


def wilson(k, n, z=1.96):
    """95% интервал Уилсона для доли k/n."""
    if n == 0:
        return 0.0, 0.0
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * (p * (1 - p) / n + z * z / (4 * n * n)) ** 0.5 / d
    return max(0.0, c - h), min(1.0, c + h)


def ratio_ci(a, b, rng, iters=2000):
    """Медиана(b)/медиана(a) и бутстреп-интервал 95%."""
    point = st.median(b) / st.median(a)
    samples = sorted(st.median(rng.choices(b, k=len(b))) / st.median(rng.choices(a, k=len(a)))
                     for _ in range(iters))
    return point, samples[int(.025 * iters)], samples[int(.975 * iters)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rounds', type=int, default=20)
    ap.add_argument('--warmup', type=int, default=2)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--case', action='append', required=True)
    ap.add_argument('--out', default='bench-results')
    ap.add_argument('--settle', type=int, default=0,
                    help='ждать до N секунд, пока load average не опустится ниже 1.5')
    ap.add_argument('bins', nargs='+')
    args = ap.parse_args()
    bins = [b.split('=', 1) for b in args.bins]
    rng = random.Random(args.seed)
    os.makedirs(args.out, exist_ok=True)
    deadline = time.time() + args.settle
    while os.getloadavg()[0] > 1.5 and time.time() < deadline:
        time.sleep(5)
    load1 = os.getloadavg()[0]
    out(f'# Сравнение: {", ".join(n for n, _ in bins)}\n')
    out(f'Раундов: {args.rounds}, прогрев: {args.warmup}, seed: {args.seed}, '
        f'load average перед стартом: {load1:.2f}')
    if load1 > 1.5:
        out(f'\n**Внимание:** система нагружена (load average {load1:.2f}); фоновая '
            'нагрузка увеличивает разброс и меняет долю режимов.')
    raw = {}
    for case in args.case:
        scenario, n = case.split(':')
        for name, path in bins:
            for _ in range(args.warmup):
                run_once(path, scenario, n)
        runs = {name: [] for name, _ in bins}
        for rnd in range(args.rounds):
            order = bins[:]
            rng.shuffle(order)
            for pos, (name, path) in enumerate(order):
                m = run_once(path, scenario, n)
                m.update(round=rnd, position=pos)
                runs[name].append(m)
        sums = {r['checksum'] for rs in runs.values() for r in rs}
        if len(sums) != 1:
            raise SystemExit(f'{case}: контрольные суммы различаются: {sums}')
        raw[case] = runs
        report(case, runs, bins, rng)
        impls = {}
        for name, _ in bins:
            impls.setdefault(name.split('/')[0], []).append(name)
        if any(len(v) > 1 for v in impls.values()):
            report_impls(case, runs, impls, rng)
    with open(os.path.join(args.out, 'compare.json'), 'w') as f:
        json.dump(raw, f)
    with open(os.path.join(args.out, 'summary.md'), 'w') as f:
        f.write('\n'.join(LINES) + '\n')


LINES = []


def out(line=''):
    print(line, flush=True)
    LINES.append(line)


def fmt_m(c):
    return f"{c['median'] / 1e6:8.1f} [{c['p25'] / 1e6:.1f}–{c['p75'] / 1e6:.1f}]"


def report(case, runs, bins, rng):
    base = bins[0][0]
    out(f'\n### {case}\n')
    out('| бинарник | режим | запусков | такты, млн: медиана [IQR] | мин–макс | CV | '
          'IPC | reclaims | время, мс | инстр., млн | пик, МиБ |')
    out('|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
    modes_of = {}
    for name, _ in bins:
        rs = runs[name]
        modes = split_modes(rs)
        modes_of[name] = modes
        groups = [('все', rs)] + ([(f'режим {i + 1}', m) for i, m in enumerate(modes)]
                                  if len(modes) > 1 else [])
        for label, g in groups:
            c = describe(g)
            out(f"| {name} | {label} | {c['n']} | {fmt_m(c)} | "
                  f"{c['min'] / 1e6:.0f}–{c['max'] / 1e6:.0f} | {100 * c['cv']:.1f}% | "
                  f"{st.median(r['ipc'] for r in g):.2f} | "
                  f"{st.median(r['reclaims'] for r in g):.0f} | "
                  f"{st.median(r['wall_ms'] for r in g):.1f} | "
                  f"{st.median(r['instructions'] for r in g) / 1e6:.0f} | "
                  f"{st.median(r['peak'] for r in g) / 2**20:.0f} |")
    out()
    for name, _ in bins[1:]:
        a = [r['cycles'] for r in runs[base]]
        b = [r['cycles'] for r in runs[name]]
        p, lo, hi = ratio_ci(a, b, rng)
        line = f'- {name} / {base}, все запуски: **{p:.3f}** (95% CI {lo:.3f}–{hi:.3f})'
        fa, fb = modes_of[base][0], modes_of[name][0]
        if len(modes_of[base]) > 1 or len(modes_of[name]) > 1:
            p, lo, hi = ratio_ci([r['cycles'] for r in fa], [r['cycles'] for r in fb], rng)
            line += f'; быстрые режимы: **{p:.3f}** ({lo:.3f}–{hi:.3f})'
            def share(nm):
                k, n = len(modes_of[nm][0]), len(runs[nm])
                lo, hi = wilson(k, n)
                return f'{k}/{n} (95% CI {100 * lo:.0f}–{100 * hi:.0f}%)'
            line += f'; доля быстрого режима: {base} {share(base)}, {name} {share(name)}'
        out(line)


def report_impls(case, runs, impls, rng):
    """Сводка по реализациям через все их раскладки."""
    out(f'\n#### {case}: по реализациям (все раскладки вместе)\n')
    out('| реализация | раскладок | запусков | доля быстрого режима | такты быстрого режима, млн: '
        'медиана [IQR] | такты медленного режима, млн | медианы быстрого режима по раскладкам |')
    out('|---|---:|---:|---:|---:|---:|---|')
    pooled = {}
    for impl, names in impls.items():
        fast, slow, per_layout = [], [], []
        for nm in names:
            modes = split_modes(runs[nm])
            fast += modes[0]
            for m in modes[1:]:
                slow += m
            per_layout.append(st.median(r['cycles'] for r in modes[0]) / 1e6)
        allr = fast + slow
        pooled[impl] = (allr, fast)
        lo, hi = wilson(len(fast), len(allr))
        c = describe(fast)
        slow_s = f"{st.median(r['cycles'] for r in slow) / 1e6:.1f} ({len(slow)})" if slow else '—'
        out(f'| {impl} | {len(names)} | {len(allr)} | {len(fast)}/{len(allr)} '
            f'({100 * lo:.0f}–{100 * hi:.0f}%) | {fmt_m(c)} | {slow_s} | '
            f'{", ".join(f"{x:.0f}" for x in per_layout)} |')
    out()
    names = list(pooled)
    base = names[0]
    for impl in names[1:]:
        for label, idx in (('быстрые режимы', 1), ('все запуски', 0)):
            a = [r['cycles'] for r in pooled[base][idx]]
            b = [r['cycles'] for r in pooled[impl][idx]]
            p, lo, hi = ratio_ci(a, b, rng)
            out(f'- {impl} / {base}, {label}: **{p:.3f}** (95% CI {lo:.3f}–{hi:.3f})')


if __name__ == '__main__':
    main()
