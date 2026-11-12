#!/usr/bin/env python3
"""Быстрый контроль производительности при изменениях (`just bench-quick`).

Не замена полным сравнениям (compare.py по 4 раскладкам, десятки минут), а
сигнал за полминуты: что изменилось относительно сохранённой базы.

- Главная метрика — инструкции: почти детерминированы (разброс ~0.1%),
  поэтому изменение кода видно сразу и без шума. Такты меряются рядом
  (раунды вперемешку, медиана, бутстреп-интервал 95% отношения к базе) —
  это проверка, что изменение инструкций сказалось на скорости, но одна
  раскладка кода и малые N: такты здесь — ориентир, решение — по полному
  прогону.
- База — результаты прошлого прогона с --save, для этой машины
  (bench-results/quick/baseline.json; каталог не в git). Сравнение — по
  имени бинарника и сценарию.
- Отметка изменения — только сверх шума: сдвиг больше порога (--instr-tol,
  по умолчанию 0.5%; --cycles-tol, по умолчанию 5%) и больше собственного
  разброса метрики (p10–p90 относительно медианы, наибольший из базы и
  текущего прогона); для тактов ещё и интервал отношения без 1. Разброс
  инструкций у вариантов на системном malloc — до ~1% (его медленные пути
  зависят от состояния кучи), у пула — сотые доли процента.

usage:
  quick.py [--rounds N] [--save] [--baseline FILE] [--case S:N ...]
           [--env NAME:VAR=VALUE ...] NAME=PATH [NAME=PATH ...]
Код выхода 1 с --fail-on-regression, если есть отмеченное ухудшение.
"""

import argparse
import json
import os
import random
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare import ratio_ci, run_once  # noqa: E402


def spread(xs):
    """Разброс p10–p90 относительно медианы, %."""
    xs = sorted(xs)
    k = len(xs) - 1
    return (xs[round(0.9 * k)] - xs[round(0.1 * k)]) / st.median(xs) * 100


DEFAULT_CASES = [
    'fill:300000',
    'mixed:300000',
    'persistent:200000',
    'branching:300000',
    'fill_mut:300000',
    'mixed_mut:300000',
    'fill:3000000',
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rounds', type=int, default=10)
    ap.add_argument('--case', action='append', default=None)
    ap.add_argument('--env', action='append', default=[], metavar='NAME:VAR=VALUE')
    ap.add_argument('--baseline', default='bench-results/quick/baseline.json')
    ap.add_argument('--save', action='store_true', help='сохранить результаты как новую базу')
    ap.add_argument('--instr-tol', type=float, default=0.5, help='порог инструкций, %%')
    ap.add_argument('--cycles-tol', type=float, default=5.0, help='порог тактов, %%')
    ap.add_argument('--fail-on-regression', action='store_true')
    ap.add_argument('bins', nargs='+')
    a = ap.parse_args()
    cases = a.case or DEFAULT_CASES
    bins = [b.split('=', 1) for b in a.bins]
    envs = {}
    for e in a.env:
        name, kv = e.split(':', 1)
        var, val = kv.split('=', 1)
        envs.setdefault(name, {})[var] = val

    rng = random.Random(1)
    runs = {c: {n: [] for n, _ in bins} for c in cases}
    for name, path in bins:  # прогрев
        run_once(path, *cases[0].split(':'), envs.get(name))
    for _ in range(a.rounds):
        order = [(c, n, p) for c in cases for n, p in bins]
        rng.shuffle(order)
        for case, name, path in order:
            scenario, n = case.split(':')
            m = run_once(path, scenario, n, envs.get(name))
            runs[case][name].append(
                {
                    'cycles': m['cycles'],
                    'instructions': m['instructions'],
                    'checksum': m['checksum'],
                }
            )
    for case in cases:
        sums = {r['checksum'] for rs in runs[case].values() for r in rs}
        if len(sums) != 1:
            raise SystemExit(f'{case}: контрольные суммы различаются: {sums}')

    base = {}
    if os.path.exists(a.baseline):
        with open(a.baseline) as f:
            base = json.load(f)
    print(f'база: {a.baseline}' + ('' if base else ' (нет — только текущие значения)'))
    print(
        '\n| сценарий | бинарник | такты, млн | Δ тактов (95%) | инструкции, млн | Δ инструкций |'
    )
    print('|---|---|---:|---|---:|---:|')
    regressions = []
    for case in cases:
        for name, _ in bins:
            cur = runs[case][name]
            cyc = [r['cycles'] / 1e6 for r in cur]
            ins = st.median(r['instructions'] for r in cur) / 1e6
            old = base.get(case, {}).get(name)
            dc = di = ''
            if old:
                ocyc = [r['cycles'] / 1e6 for r in old]
                oins = st.median(r['instructions'] for r in old) / 1e6
                pt, lo, hi = ratio_ci(ocyc, cyc, rng)
                c_noise = max(spread(ocyc), spread(cyc))
                i_noise = max(
                    spread([r['instructions'] for r in old]),
                    spread([r['instructions'] for r in cur]),
                )
                c_mark = (lo > 1 or hi < 1) and abs(pt - 1) * 100 > max(a.cycles_tol, c_noise)
                i_delta = (ins / oins - 1) * 100
                i_mark = abs(i_delta) > max(a.instr_tol, i_noise)
                dc = f'{(pt - 1) * 100:+.1f}% ({(lo - 1) * 100:+.1f}…{(hi - 1) * 100:+.1f})'
                di = f'{i_delta:+.2f}%'
                if c_mark:
                    dc = ('▲ ' if pt > 1 else '▼ ') + dc
                if i_mark:
                    di = ('▲ ' if i_delta > 0 else '▼ ') + di
                if (c_mark and pt > 1) or (i_mark and i_delta > 0):
                    regressions.append(f'{case} {name}')
            print(f'| {case} | {name} | {st.median(cyc):.1f} | {dc} | {ins:.1f} | {di} |')
    if base:
        print(
            '\n▲ — больше (хуже), ▼ — меньше (лучше): сдвиг больше порога (такты '
            f'{a.cycles_tol}%, инструкции {a.instr_tol}%) и больше разброса p10–p90; '
            'такты — ещё и при интервале без 1.'
        )
        print('Ухудшения: ' + (', '.join(regressions) if regressions else 'нет'))
    if a.save:
        merged = dict(base)
        for case in cases:
            merged.setdefault(case, {}).update(runs[case])
        os.makedirs(os.path.dirname(a.baseline), exist_ok=True)
        with open(a.baseline, 'w') as f:
            json.dump(merged, f)
        print(f'база сохранена: {a.baseline}')
    if a.fail_on_regression and regressions:
        sys.exit(1)


if __name__ == '__main__':
    main()
