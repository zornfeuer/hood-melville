//! Очередь с пулом узлов (фича `pool`, `docs/pool.md`): та же семантика, что
//! у `Queue`/`SyncQueue`, и сам пул — статистика, `trim`, кэши потоков.
//!
//! Пулы общие для процесса, а тесты идут параллельно: каждый тест держит
//! `SERIAL`, чтобы счёт живых узлов был его собственным.

#![cfg(feature = "pool")]

use std::collections::VecDeque;
use std::rc::Rc;
use std::sync::{Arc, Barrier, Mutex, MutexGuard};
use std::thread;

use hood_melville::pool::{self, PoolStats};
use hood_melville::{PooledQueue, Queue, SyncPooledQueue};

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Узлов в плите 65 536 байт из 16-байтовых узлов: первый — заголовок.
const PER_SLAB: usize = 65536 / 16 - 1;

fn xorshift(x: &mut u64) -> u64 {
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    *x
}

fn contents<R: hood_melville::refcount::RefCount>(q: &Queue<u32, R>) -> Vec<u32> {
    q.iter().collect()
}

/// Случайная история с ветвлением: персистентные и на месте операции над
/// случайными живыми версиями, против моделей `VecDeque`.
fn random_history<R: hood_melville::refcount::RefCount>(seed: u64) {
    let mut x = seed;
    let mut versions: Vec<(Queue<u32, R>, VecDeque<u32>)> =
        vec![(Queue::default(), VecDeque::new())];
    let steps = if cfg!(miri) { 200 } else { 20_000 };
    for step in 0..steps {
        let i = usize::try_from(xorshift(&mut x) % versions.len() as u64).unwrap();
        let (q, m) = &versions[i];
        let (mut q, mut m) = (q.clone(), m.clone());
        match xorshift(&mut x) % 6 {
            0 | 1 => {
                q = q.push(step);
                m.push_back(step);
            }
            2 => {
                q.push_mut(step);
                m.push_back(step);
            }
            3 => {
                if let Some(p) = q.pop() {
                    q = p;
                    m.pop_front();
                } else {
                    assert!(m.is_empty());
                }
            }
            _ => assert_eq!(q.pop_mut(), m.pop_front()),
        }
        assert_eq!(contents(&q), m.iter().copied().collect::<Vec<_>>());
        if versions.len() < 32 || xorshift(&mut x).is_multiple_of(2) {
            versions.push((q, m));
        } else {
            versions[i] = (q, m);
        }
    }
    for (q, m) in &versions {
        assert_eq!(contents(q), m.iter().copied().collect::<Vec<_>>());
    }
}

#[test]
fn pooled_queues_match_models() {
    let _g = serial();
    for seed in 1..=4 {
        random_history::<hood_melville::pool::Pooled<hood_melville::refcount::Local>>(seed);
        random_history::<hood_melville::pool::Pooled<hood_melville::refcount::Atomic>>(seed);
    }
    assert_eq!(pool::stats().live, 0);
}

/// Те же операции над `Queue` и `PooledQueue` со значениями `Rc`: копий
/// значения в каждой точке столько же (ротация клонирует значения общих
/// узлов), и все освобождаются.
fn rc_copies<R: hood_melville::refcount::RefCount>() -> Vec<usize> {
    let tracker = Rc::new(());
    let mut counts = Vec::new();
    let q: Queue<Rc<()>, R> = (0..1000).map(|_| Rc::clone(&tracker)).collect();
    let c = q.clone();
    let mut q = q;
    for _ in 0..500 {
        q.pop_mut();
    }
    counts.push(Rc::strong_count(&tracker));
    drop(c);
    counts.push(Rc::strong_count(&tracker));
    drop(q);
    counts.push(Rc::strong_count(&tracker));
    counts
}

#[test]
fn values_are_dropped_exactly_once() {
    let _g = serial();
    let heap = rc_copies::<hood_melville::refcount::Local>();
    let pooled = rc_copies::<hood_melville::pool::Pooled<hood_melville::refcount::Local>>();
    assert_eq!(heap, pooled);
    assert_eq!(pooled.last(), Some(&1));
    assert_eq!(pool::stats().live, 0);
}

#[test]
#[allow(clippy::many_single_char_names)]
fn stats_count_live_nodes_and_trim_returns_slabs() {
    let _g = serial();
    pool::trim();
    let before = pool::stats();
    assert_eq!(before.slabs, 0);

    let n = if cfg!(miri) { 500 } else { 20_000 };
    let q: PooledQueue<u32> = (0..n).collect();
    let s = pool::stats();
    assert!(s.slabs > 0);
    assert_eq!(s.slab_bytes, s.slabs * 65536);
    assert_eq!(s.capacity, s.slabs * PER_SLAB);
    assert_eq!(s.live + s.free, s.capacity);
    assert!(s.live >= n as usize && s.live <= 3 * n as usize);
    assert_eq!(pool::trim(), 0, "живые узлы есть");

    let c = q.clone();
    drop(q);
    assert_eq!(pool::stats().live, s.live, "клон держит те же узлы");
    drop(c);
    let s = pool::stats();
    assert_eq!(s.live, 0);
    assert_eq!(pool::trim(), s.slab_bytes);
    let after = pool::stats();
    assert_eq!(after.slabs, 0);
    assert_eq!(after.slab_frees, before.slab_frees + s.slabs as u64);

    let r: PooledQueue<u32> = (0..100).collect();
    assert_eq!(r.len(), 100);
    drop(r);
    assert_eq!(pool::stats().live, 0);
}

#[test]
fn memory_is_kept_at_the_peak_until_trim() {
    let _g = serial();
    pool::trim();
    let n = if cfg!(miri) { 2_000 } else { 100_000 };
    let mut q: PooledQueue<u32> = (0..n).collect();
    let peak = pool::stats().slabs;
    for _ in 0..n - n / 100 {
        q.pop_mut();
    }
    assert_eq!(pool::stats().slabs, peak, "плиты не возвращаются сами");
    for i in 0..n * 9 / 10 {
        q.push_mut(i);
    }
    assert_eq!(pool::stats().slabs, peak, "рост снова — из свободных узлов");
    drop(q);
    assert_eq!(pool::trim(), peak * 65536);
}

#[test]
fn long_cache_spills_batches_other_threads_take() {
    let _g = serial();
    pool::trim();
    let before = pool::stats();
    // Больше предела кэша потока (4096), чтобы освобождение переливалось.
    let n = if cfg!(miri) { 6_000 } else { 10_000 };
    drop((0..n).collect::<PooledQueue<u32>>());
    let s = pool::stats();
    assert!(s.spills > before.spills);
    assert_eq!(s.live, 0);
    let slabs = s.slabs;
    thread::spawn(move || {
        let r: PooledQueue<u32> = (0..n / 2).collect();
        assert_eq!(r.len(), (n / 2) as usize);
    })
    .join()
    .unwrap();
    let s = pool::stats();
    assert!(s.refills > before.refills);
    assert_eq!(s.slabs, slabs, "другой поток взял пачки, а не плиту");
    assert_eq!(s.live, 0, "кэш завершившегося потока вернулся в пул");
    assert_eq!(pool::trim(), slabs * 65536);
}

#[test]
fn exiting_threads_return_caches() {
    let _g = serial();
    pool::trim();
    let rounds = if cfg!(miri) { 3 } else { 100 };
    // Каждый поток держит очередь, пока она есть у всех четырёх: потоки
    // гарантированно пересекаются и берут по своей плите (иначе на малом
    // числе ядер поток успевал завершиться и отдать плиту следующему).
    let all_live = Arc::new(Barrier::new(4));
    let threads: Vec<_> = (0..4)
        .map(|_| {
            let all_live = Arc::clone(&all_live);
            thread::spawn(move || {
                let held: PooledQueue<u32> = (0..50).collect();
                all_live.wait();
                drop(held);
                for _ in 0..rounds {
                    let q: PooledQueue<u32> = (0..50).collect();
                    assert_eq!(q.len(), 50);
                }
            })
        })
        .collect();
    for t in threads {
        t.join().unwrap();
    }
    let s: PoolStats = pool::stats();
    assert!(s.slabs >= 4, "плита на поток");
    assert_eq!(s.live, 0);
    assert_eq!(pool::trim(), s.slabs * 65536);
}

#[test]
fn sync_pooled_versions_shared_and_dropped_across_threads() {
    let _g = serial();
    let n: u64 = if cfg!(miri) { 300 } else { 30_000 };
    let base: SyncPooledQueue<u64> = (0..n).collect();
    let expected = n * (n - 1) / 2;
    let threads: Vec<_> = (0..4)
        .map(|_| {
            let mut q = base.clone();
            thread::spawn(move || {
                let mut sum = 0;
                while let Some(v) = q.pop_mut() {
                    sum += v;
                }
                sum
            })
        })
        .collect();
    drop(base);
    for t in threads {
        assert_eq!(t.join().unwrap(), expected);
    }
    assert_eq!(pool::stats().live, 0);

    // Значения `Arc` освобождаются ровно раз, где бы ни умер узел.
    let tracker = Arc::new(());
    let base: SyncPooledQueue<Arc<()>> = (0..1000).map(|_| Arc::clone(&tracker)).collect();
    let versions: Vec<_> = (0..4).map(|_| base.clone()).collect();
    drop(base);
    let threads: Vec<_> = versions
        .into_iter()
        .map(|v| thread::spawn(move || drop(v)))
        .collect();
    for t in threads {
        t.join().unwrap();
    }
    assert_eq!(Arc::strong_count(&tracker), 1);
    assert_eq!(pool::stats().live, 0);
}
