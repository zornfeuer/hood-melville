//! Время одной операции при разных размерах очереди — аналог `[benchmark]`
//! из `c/tests/fqueue_test.cxx`, но для нескольких типов элементов и с учётом
//! фазы ротации.
//!
//! Запуск: `cargo bench` (или `cargo bench -- push/String` для выборки).
//! Детерминированная проверка того же — в `tests/alloc.rs`.
//!
//! Стоимость операции зависит от фазы ротации, в которой находится очередь,
//! но не от её размера. Поэтому размеры берутся не круглыми, а в окнах фаз,
//! пропорциональных 2^k: при построении одними push'ами ротация стартует на
//! размере `2^k − 1`, `Reversing` занимает `2^(k−1)` шагов, вся ротация —
//! `2^k − 1`, по `STEPS` шагов на push (формулы проверены юнит-тестом
//! `pure_pushes_rotation_phases` в `queue.rs`). В каждой фазе время должно быть
//! одинаковым для всех N.

use std::rc::Rc;
use std::time::Duration;

use criterion::{BatchSize, BenchmarkId, Criterion, criterion_group, criterion_main};
use hood_melville::Queue;

/// 2^k для k = 7, 10, 13, 17 — порядки 10², 10³, 10⁴, 10⁵ как в C-бенчмарке.
const SCALES: [usize; 4] = [1 << 7, 1 << 10, 1 << 13, 1 << 17];

/// Шагов ротации на операцию — `ROTATION_STEPS` в `src/queue.rs`.
const STEPS: usize = 1;

/// Размер очереди в данной фазе для масштаба `base = 2^k`.
type SizeAt = fn(usize) -> usize;

/// Конец фазы `Reversing` и конец ротации, начатой на размере `base − 1`.
const fn rev_end(base: usize) -> usize {
    base - 1 + base / 2 / STEPS
}
const fn rot_end(base: usize) -> usize {
    base - 1 + (base - 1) / STEPS
}

/// Середины окон фаз. При одном шаге ротация длится до самого следующего
/// старта, и без ротации очередь бывает только на размере `2·base − 2`.
const PHASES: [(&str, SizeAt); 3] = [
    ("reversing", |base| base.midpoint(rev_end(base))),
    ("appending", |base| rev_end(base).midpoint(rot_end(base))),
    ("idle", |base| {
        if STEPS == 1 {
            2 * base - 2
        } else {
            rot_end(base).midpoint(2 * base - 1)
        }
    }),
];

/// Размер, push в который запускает ротацию (переход к 2^k − 1).
fn trigger(base: usize) -> usize {
    base - 2
}

/// Крупное значение без аллокаций внутри.
#[derive(Clone)]
struct Big(#[allow(dead_code, reason = "нужен только размер")] [u64; 32]);

/// Тип элемента и способ получить `i`-е значение.
trait Element: Clone + 'static {
    const NAME: &'static str;
    fn make(i: usize) -> Self;
}

impl Element for i32 {
    const NAME: &'static str = "i32";
    fn make(i: usize) -> Self {
        i32::try_from(i).unwrap()
    }
}

impl Element for String {
    const NAME: &'static str = "String";
    fn make(i: usize) -> Self {
        format!("{i:032}")
    }
}

impl Element for Big {
    const NAME: &'static str = "[u64; 32]";
    fn make(i: usize) -> Self {
        Self([i as u64; 32])
    }
}

impl Element for Rc<str> {
    const NAME: &'static str = "Rc<str>";
    fn make(i: usize) -> Self {
        Rc::from(format!("{i:032}"))
    }
}

fn build<T: Element>(n: usize) -> Queue<T> {
    (0..n).map(T::make).collect()
}

/// Один push в очередь размера N. Значение готовится вне замера, новая
/// версия удаляется тоже вне замера (`iter_batched`). Фаза `trigger` —
/// push, запускающий ротацию: у наивной очереди на двух стеках он стоил бы O(n).
fn push<T: Element>(c: &mut Criterion) {
    let mut group = c.benchmark_group(format!("push/{}", T::NAME));
    let phases = [("trigger", trigger as SizeAt)].into_iter().chain(PHASES);
    for (phase, size) in phases {
        for base in SCALES {
            let n = size(base);
            let q = build::<T>(n);
            let value = T::make(n);
            group.bench_with_input(BenchmarkId::new(phase, n), &q, |b, q| {
                b.iter_batched(|| value.clone(), |v| q.push(v), BatchSize::SmallInput);
            });
        }
    }
    group.finish();
}

/// Один pop из очереди размера N.
fn pop<T: Element>(c: &mut Criterion) {
    let mut group = c.benchmark_group(format!("pop/{}", T::NAME));
    for (phase, size) in PHASES {
        for base in SCALES {
            let n = size(base);
            let q = build::<T>(n);
            group.bench_with_input(BenchmarkId::new(phase, n), &q, |b, q| {
                b.iter_with_large_drop(|| q.pop());
            });
        }
    }
    group.finish();
}

fn all<T: Element>(c: &mut Criterion) {
    push::<T>(c);
    pop::<T>(c);
}

fn config() -> Criterion {
    Criterion::default()
        .warm_up_time(Duration::from_millis(300))
        .measurement_time(Duration::from_secs(1))
}

criterion_group! {
    name = benches;
    config = config();
    targets = all::<i32>, all::<String>, all::<Big>, all::<Rc<str>>
}
criterion_main!(benches);
