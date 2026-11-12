#!/usr/bin/env python3
"""Параллельная нагрузка fqueue_par (#6): масштабирование по числу потоков и
доле операций над общими версиями, писатель и читатели снимков.

Сценарии (описание — rust/examples/fqueue_par.rs):
- `PATH share <mode> <share> <P>` печатает «контрольная_сумма время_нс»;
- `PATH snapshot <R>` печатает «контрольная_сумма время_писателя_нс снимки
  элементы».
Запуски всех сочетаний в каждом раунде идут в случайном порядке, чтобы
медленное состояние системы не доставалось одному сочетанию. Контрольная сумма
share зависит только от доли, snapshot — ни от чего; она обязана совпасть у
всех реализаций, режимов, P и R.

Печатается (медианы по раундам):
- share, для каждого режима: пропускная способность (млн операций/с),
  ускорение относительно одного потока той же реализации и доля от
  пропускной способности без разделения (share 0) при том же P;
- snapshot: скорость писателя (млн операций/с), его замедление относительно
  R=0 и скорость чтения (млн элементов/с, все читатели вместе).

usage:
  parallel.py [--rounds N] [--modes pool,threads] [--shares 0,25,50,100]
              [--threads 1,2,4,8] [--readers 0,1,4] [--settle S] [--out DIR]
              NAME=PATH [...]
Пустой --shares или --readers отключает сценарий.
"""
import argparse
import json
import os
import random
import statistics as st
import subprocess
import time

TASKS, OPS = 256, 40000  # share: всего операций TASKS * OPS
SNAP_OPS = 2 * 1000000    # snapshot: операций писателя (SNAP_PAIRS пар)


def run_once(path, args, env=None):
    out = subprocess.run([path, *map(str, args)], capture_output=True, text=True, check=True,
                         env={**os.environ, **env} if env else None).stdout.split()
    return out[0], [int(x) for x in out[1:]]


def ints(s):
    return [int(x) for x in s.split(',') if x]


def table(title, cols, rows, fmt):
    print(f'\n{title}')
    print('| | ' + ' | '.join(f'{c}' for c in cols) + ' |')
    print('|---|' + '---:|' * len(cols))
    for name, vals in rows:
        print(f'| {name} | ' + ' | '.join(fmt(v) if v is not None else '—' for v in vals) + ' |')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rounds', type=int, default=5)
    ap.add_argument('--modes', default='pool,threads')
    ap.add_argument('--shares', type=ints, default=ints('0,1,5,10,25,50,100'))
    ap.add_argument('--threads', type=ints, default=ints('1,2,4,6,8,10'))
    ap.add_argument('--readers', type=ints, default=ints('0,1,2,4,6,9'))
    ap.add_argument('--settle', type=float, default=0,
                    help='пауза перед замерами, с (остывание после сборки)')
    ap.add_argument('--out', default=None)
    ap.add_argument('--env', action='append', default=[], metavar='NAME:VAR=VALUE',
                    help='переменная окружения для бинарника NAME (например, подстановка аллокатора)')
    ap.add_argument('bins', nargs='+')
    a = ap.parse_args()
    bins = [b.split('=', 1) for b in a.bins]
    envs = {}
    for e in a.env:
        name, kv = e.split(':', 1)
        var, val = kv.split('=', 1)
        envs.setdefault(name, {})[var] = val
    modes = a.modes.split(',')
    names = [n for n, _ in bins]
    combos = [(name, path, ('share', m, s, p)) for name, path in bins for m in modes
              for s in a.shares for p in a.threads]
    combos += [(name, path, ('snapshot', r)) for name, path in bins for r in a.readers]

    if a.settle:
        time.sleep(a.settle)
    results = {(name, args): [] for name, _, args in combos}
    sums = {}
    t_start = time.time()
    for rnd in range(a.rounds):
        order = combos[:]
        random.shuffle(order)
        for name, path, args in order:
            h, vals = run_once(path, args, envs.get(name))
            key = args[2] if args[0] == 'share' else 'snapshot'
            if sums.setdefault(key, h) != h:
                raise SystemExit(f'контрольная сумма {name} {args}: {h} != {sums[key]}')
            results[(name, args)].append(vals)
        print(f'раунд {rnd + 1}/{a.rounds}: {time.time() - t_start:.0f} с', flush=True)

    def med(name, args, i=0):
        return st.median(v[i] for v in results[(name, args)])

    for m in modes if a.shares else []:
        ns = {(n, s, p): med(n, ('share', m, s, p)) for n in names for s in a.shares
              for p in a.threads}
        print(f'\n## share, режим {m} (столбцы — число потоков P)')
        for s in a.shares:
            table(f'share {s}%: млн операций/с', a.threads,
                  [(n, [TASKS * OPS / ns[(n, s, p)] * 1e3 for p in a.threads]) for n in names],
                  lambda v: f'{v:.1f}')
        if 1 in a.threads:
            table('Ускорение относительно P=1', a.threads,
                  [(f'{n} {s}%', [ns[(n, s, 1)] / ns[(n, s, p)] for p in a.threads])
                   for n in names for s in a.shares],
                  lambda v: f'{v:.2f}')
        if 0 in a.shares:
            table('Доля от пропускной способности при share 0 (тот же P)', a.threads,
                  [(f'{n} {s}%', [ns[(n, 0, p)] / ns[(n, s, p)] for p in a.threads])
                   for n in names for s in a.shares if s],
                  lambda v: f'{v:.2f}')

    if a.readers:
        print('\n## snapshot: писатель и R читателей (столбцы — R)')
        cols = a.readers
        writer = {(n, r): med(n, ('snapshot', r)) for n in names for r in cols}
        table('Писатель, млн операций/с', [f'{r}' for r in cols],
              [(n, [SNAP_OPS / writer[(n, r)] * 1e3 for r in cols]) for n in names],
              lambda v: f'{v:.1f}')
        if 0 in cols:
            table('Замедление писателя относительно R=0', cols,
                  [(n, [writer[(n, r)] / writer[(n, 0)] for r in cols]) for n in names],
                  lambda v: f'{v:.2f}')
        table('Читатели вместе, млн элементов/с', cols,
              [(n, [st.median(v[2] / v[0] * 1e3 for v in results[(n, ('snapshot', r))]) if r else None
                    for r in cols]) for n in names],
              lambda v: f'{v:.0f}')

    if a.out:
        os.makedirs(a.out, exist_ok=True)
        path = os.path.join(a.out, time.strftime('%Y%m%d-%H%M%S') + '.json')
        with open(path, 'w') as f:
            json.dump({'bins': dict(bins), 'rounds': a.rounds,
                       'results': {f'{n} ' + ' '.join(map(str, args)): v
                                   for (n, args), v in results.items()}}, f, indent=1)
        print(f'\nсырые данные: {path}')


if __name__ == '__main__':
    main()
