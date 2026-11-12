#!/usr/bin/env python3
"""Сводка прогона compare.py по сетке «язык × аллокатор» (#8).

Имена бинарников — LANG-ALLOC/LAYOUT (например, Rust-mi/al64); запуски всех
раскладок одной пары LANG-ALLOC объединяются. Печатает:
- такты (медиана всех запусков, млн) по сценариям;
- аллокаторы внутри языка: ALLOC / sys того же языка;
- языки при одном аллокаторе: LANG / C того же аллокатора;
- долю быстрого режима (режимы — как в compare.py) и пик памяти.
Отношения — медианы всех запусков с бутстреп-интервалом 95%; где у пары
несколько режимов, медиана всех запусков зависит от их долей — это видно в
таблице режимов.

usage: alloc_report.py bench-results/alloc-single/compare.json
"""
import json
import os
import random
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare import ratio_ci, split_modes  # noqa: E402

LANGS = ['C', 'Rust', 'Zig']
ALLOCS = ['sys', 'mi', 'je']


def main():
    raw = json.load(open(sys.argv[1]))
    rng = random.Random(1)
    cases = list(raw)
    pooled = {}
    for case in cases:
        for name, runs in raw[case].items():
            pooled.setdefault(case, {}).setdefault(name.split('/')[0], []).extend(runs)
    pairs = [f'{l}-{a}' for l in LANGS for a in ALLOCS]
    pairs = [p for p in pairs if p in pooled[cases[0]]]

    def cyc(case, p):
        return [r['cycles'] / 1e6 for r in pooled[case][p]]

    def ratio(case, a, b):
        pt, lo, hi = ratio_ci(cyc(case, a), cyc(case, b), rng)
        mark = '**' if lo > 1 or hi < 1 else ''
        return f'{mark}{pt:.3f}{mark} ({lo:.3f}–{hi:.3f})'

    print('## Такты, млн (медиана всех запусков, все раскладки)\n')
    print('| сценарий | ' + ' | '.join(pairs) + ' |')
    print('|---|' + '---:|' * len(pairs))
    for case in cases:
        print(f'| {case} | ' + ' | '.join(f'{st.median(cyc(case, p)):.1f}' for p in pairs) + ' |')

    print('\n## Аллокаторы внутри языка (ALLOC / sys того же языка)\n')
    cols = [(l, a) for l in LANGS for a in ALLOCS[1:] if f'{l}-{a}' in pairs]
    print('| сценарий | ' + ' | '.join(f'{l}: {a}/sys' for l, a in cols) + ' |')
    print('|---|' + '---|' * len(cols))
    for case in cases:
        print(f'| {case} | ' + ' | '.join(ratio(case, f'{l}-sys', f'{l}-{a}') for l, a in cols) + ' |')

    print('\n## Языки при одном аллокаторе (LANG / C того же аллокатора)\n')
    cols = [(a, l) for a in ALLOCS for l in LANGS[1:] if f'{l}-{a}' in pairs]
    print('| сценарий | ' + ' | '.join(f'{a}: {l}/C' for a, l in cols) + ' |')
    print('|---|' + '---|' * len(cols))
    for case in cases:
        print(f'| {case} | ' + ' | '.join(ratio(case, f'C-{a}', f'{l}-{a}') for a, l in cols) + ' |')

    print('\n## Доля быстрого режима (по IPC, как в compare.py)\n')
    print('| сценарий | ' + ' | '.join(pairs) + ' |')
    print('|---|' + '---:|' * len(pairs))
    for case in cases:
        cells = []
        for p in pairs:
            modes = split_modes(pooled[case][p])
            cells.append(f'{len(modes[0])}/{len(pooled[case][p])}' if len(modes) > 1 else '—')
        print(f'| {case} | ' + ' | '.join(cells) + ' |')

    print('\n## Пик памяти, МиБ (медиана)\n')
    print('| сценарий | ' + ' | '.join(pairs) + ' |')
    print('|---|' + '---:|' * len(pairs))
    for case in cases:
        print(f'| {case} | ' + ' | '.join(
            f'{st.median(r["peak"] for r in pooled[case][p]) / 2**20:.0f}' for p in pairs) + ' |')


if __name__ == '__main__':
    main()
