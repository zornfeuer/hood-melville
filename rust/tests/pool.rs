//! Очередь с пулом узлов (`NodePool`, `docs/pool.md`): та же семантика, что
//! у `Queue`/`SyncQueue`, и сам пул — статистика, `trim`, кэши потоков,
//! контракт `Allocator` для блоков любой раскладки.
//!
//! Пулы общие для процесса, а тесты идут параллельно: каждый тест держит
//! `SERIAL`, чтобы счёт живых узлов был его собственным.

use std::alloc::{Allocator, Global, Layout};
use std::collections::VecDeque;
use std::rc::Rc;
use std::sync::{Arc, Barrier, Mutex, MutexGuard};
use std::thread;

use hood_melville::pool::{self, NodePool, PoolStats};
use hood_melville::refcount::{Atomic, Local, RefCount};
use hood_melville::{PooledQueue, Queue, SyncPooledQueue};

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// Размер плиты и узлов в ней из 16-байтовых узлов: первый — заголовок.
const SLAB: usize = 16384;
const PER_SLAB: usize = SLAB / 16 - 1;

fn xorshift(x: &mut u64) -> u64 {
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    *x
}

fn contents<R: RefCount, A: Allocator + Clone>(q: &Queue<u32, R, A>) -> Vec<u32> {
    q.iter().collect()
}

/// Случайная история с ветвлением: персистентные и на месте операции над
/// случайными живыми версиями, против моделей `VecDeque`.
fn random_history<R: RefCount, A: Allocator + Clone + Default>(seed: u64) {
    let mut x = seed;
    let mut versions: Vec<(Queue<u32, R, A>, VecDeque<u32>)> =
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
        random_history::<Local, NodePool>(seed);
        random_history::<Atomic, NodePool>(seed);
    }
    assert_eq!(pool::stats().live, 0);
}

/// Те же операции над `Queue` и `PooledQueue` со значениями `Rc`: копий
/// значения в каждой точке столько же (ротация клонирует значения общих
/// узлов), и все освобождаются.
fn rc_copies<A: Allocator + Clone + Default>() -> Vec<usize> {
    let tracker = Rc::new(());
    let mut counts = Vec::new();
    let q: Queue<Rc<()>, Local, A> = (0..1000).map(|_| Rc::clone(&tracker)).collect();
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
    let heap = rc_copies::<Global>();
    let pooled = rc_copies::<NodePool>();
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
    assert_eq!(s.slab_bytes, s.slabs * SLAB);
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
    assert_eq!(pool::trim(), peak * SLAB);
}

#[test]
fn nodes_freed_by_another_thread_return_to_their_owner() {
    let _g = serial();
    pool::trim();
    let before = pool::stats();
    let n = if cfg!(miri) { 2_000 } else { 10_000 };
    let q: SyncPooledQueue<u32> = (0..n).collect();
    let slabs = pool::stats().slabs;
    thread::spawn(move || drop(q)).join().unwrap();
    assert_eq!(pool::stats().live, 0, "в стеке освобождений этого потока");
    let r: SyncPooledQueue<u32> = (0..n).collect();
    let s = pool::stats();
    assert!(s.collects > before.collects);
    assert_eq!(s.slabs, slabs, "узлы вернулись владельцу, новых плит нет");
    drop(r);
    assert_eq!(pool::trim(), slabs * SLAB);
}

#[test]
fn the_cache_of_an_exited_thread_is_adopted() {
    let _g = serial();
    pool::trim();
    let before = pool::stats();
    let n = if cfg!(miri) { 2_000 } else { 10_000 };
    let fill = move || {
        let q: PooledQueue<u32> = (0..n).collect();
        assert_eq!(q.len(), n as usize);
    };
    // Число плит не проверяется: потоки других тестов, завершаясь, тоже
    // оставляют сирот, и второй поток может взять не кэш первого.
    thread::spawn(fill).join().unwrap();
    thread::spawn(fill).join().unwrap();
    let s = pool::stats();
    assert!(s.adoptions > before.adoptions);
    assert_eq!(s.live, 0);
    assert_eq!(pool::trim(), s.slabs * SLAB);
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
    assert_eq!(pool::trim(), s.slabs * SLAB);
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

/// `NodePool` как аллокатор общего назначения: блоки любой раскладки —
/// из пула (по классу) или у `Global`; запись во весь выданный блок, возврат
/// с любым допустимым размером.
#[test]
fn allocator_contract_for_any_layout() {
    let _g = serial();
    let layouts = [
        (0, 1),
        (1, 1),
        (8, 8),
        (12, 4),
        (16, 8),
        (24, 16),
        (40, 8),
        (256, 8),
        (256, 16),
        (257, 8),
        (64, 32),
        (4096, 8),
    ];
    let mut blocks = Vec::new();
    for (size, align) in layouts {
        let layout = Layout::from_size_align(size, align).unwrap();
        for _ in 0..3 {
            let block = NodePool.allocate(layout).unwrap();
            assert!(block.len() >= size);
            assert!(block.cast::<u8>().as_ptr().addr().is_multiple_of(align));
            // SAFETY: блок только что выделен, длина `block.len()`.
            unsafe { block.cast::<u8>().write_bytes(0xa5, block.len()) };
            blocks.push((block, layout));
        }
    }
    let pooled_live = pool::stats().live;
    assert_eq!(
        pooled_live,
        3 * 8,
        "в пуле — блоки до 256 байт с выравниванием до 16"
    );
    for (block, layout) in blocks {
        // Любой размер между запрошенным и выданным.
        let layout = Layout::from_size_align(block.len(), layout.align()).unwrap();
        // SAFETY: блок выделен `NodePool` и раскладка ему подходит.
        unsafe { NodePool.deallocate(block.cast(), layout) };
    }
    assert_eq!(pool::stats().live, 0);
}

/// Коллекции std поверх пула: рост `Vec` (перенос между классами и к
/// `Global`) и `Box`.
#[test]
fn std_collections_on_node_pool() {
    let _g = serial();
    let mut v: Vec<u64, NodePool> = Vec::new_in(NodePool);
    for i in 0..100 {
        v.push(i);
    }
    assert_eq!(v.iter().sum::<u64>(), 4950);
    let b = Box::new_in([7_u8; 24], NodePool);
    assert_eq!(pool::stats().live, 1);
    drop(v);
    drop(b);
    assert_eq!(pool::stats().live, 0);
}
