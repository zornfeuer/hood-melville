//! `SyncQueue`: та же семантика, что у `Queue`, плюс работа из нескольких
//! потоков. Под Miri (`just rust-miri`) многопоточные тесты проверяют и
//! отсутствие гонок данных.

use std::collections::VecDeque;
use std::sync::Arc;
use std::thread;

use hood_melville::{Queue, SyncQueue};

fn assert_send_sync<T: Send + Sync>() {}

#[test]
fn sync_queue_is_send_and_sync() {
    assert_send_sync::<SyncQueue<i32>>();
    assert_send_sync::<hood_melville::Iter<i32, hood_melville::refcount::Atomic>>();
}

fn xorshift(seed: &mut u64) -> u64 {
    *seed ^= *seed << 13;
    *seed ^= *seed >> 7;
    *seed ^= *seed << 17;
    *seed
}

/// Однопоточная семантика совпадает с `Queue` на одной и той же ветвящейся
/// истории.
#[test]
fn same_results_as_queue() {
    let mut seed = 0x2545_F491_4F6C_DD1D;
    let mut local = vec![Queue::new()];
    let mut sync = vec![SyncQueue::new_sync()];
    let ops = if cfg!(miri) { 300 } else { 5_000_u32 };
    for i in 0..ops {
        let r = xorshift(&mut seed);
        let idx = usize::try_from(r % local.len() as u64).unwrap();
        let (l, s) = if (r >> 40).is_multiple_of(4) {
            match (local[idx].pop(), sync[idx].pop()) {
                (Some(l), Some(s)) => (l, s),
                (None, None) => continue,
                _ => panic!("pop разошёлся"),
            }
        } else {
            (local[idx].push(i), sync[idx].push(i))
        };
        assert_eq!(l.front(), s.front());
        local.push(l);
        sync.push(s);
    }
    for (l, s) in local.iter().zip(&sync) {
        assert!(l.iter().eq(s.iter()));
    }
}

/// Потоки параллельно порождают версии из общих (`push`/`pop`/`clone`),
/// меняют на месте свои клоны и удаляют их; общие версии не меняются,
/// результаты совпадают с моделью.
#[test]
fn threads_derive_from_shared_versions() {
    let (threads, rounds): (u32, u32) = if cfg!(miri) { (3, 30) } else { (8, 3_000) };
    // Общие версии с разными состояниями ротации.
    let mut shared = vec![SyncQueue::new_sync()];
    let mut models = vec![VecDeque::new()];
    for i in 0..40_u32 {
        let (q, mut m) = (shared.last().unwrap(), models.last().unwrap().clone());
        let next = if i % 3 == 2 {
            m.pop_front();
            q.pop().unwrap()
        } else {
            m.push_back(i);
            q.push(i)
        };
        shared.push(next);
        models.push(m);
    }

    thread::scope(|s| {
        for t in 0..threads {
            let (shared, models) = (&shared, &models);
            s.spawn(move || {
                let mut seed = 0x9E37_79B9_7F4A_7C15 ^ (u64::from(t) + 1);
                for k in 0..rounds {
                    let idx = usize::try_from(xorshift(&mut seed) % shared.len() as u64).unwrap();
                    let mut q = shared[idx].clone();
                    let mut m = models[idx].clone();
                    for j in 0..4_u32 {
                        let value = 1_000_000 * (t + 1) + k * 4 + j;
                        if xorshift(&mut seed).is_multiple_of(3) {
                            assert_eq!(q.pop_mut(), m.pop_front());
                        } else {
                            q = q.push(value);
                            q.push_mut(value + 1);
                            m.push_back(value);
                            m.push_back(value + 1);
                        }
                    }
                    assert_eq!(q.len(), m.len());
                    assert!(q.iter().eq(m.iter().copied()));
                }
            });
        }
    });

    for (q, m) in shared.iter().zip(&models) {
        assert!(q.iter().eq(m.iter().copied()));
    }
}

/// Версии создаются в одних потоках, а удаляются в других; значения
/// (`Arc`) освобождаются ровно один раз.
#[test]
fn versions_dropped_in_other_threads() {
    let tracker = Arc::new(());
    let n = if cfg!(miri) { 50 } else { 10_000 };
    let base: SyncQueue<Arc<()>> = (0..n).map(|_| Arc::clone(&tracker)).collect();
    let versions: Vec<SyncQueue<Arc<()>>> = (0..8)
        .map(|i| {
            let mut q = base.clone();
            for _ in 0..i * n / 8 {
                q.pop_mut();
            }
            q
        })
        .collect();
    drop(base);
    thread::scope(|s| {
        for q in versions {
            s.spawn(move || drop(q));
        }
    });
    assert_eq!(Arc::strong_count(&tracker), 1);
}

/// Каждый поток обходит свой клон одной версии (читает значения и `next`
/// общих узлов) и удаляет его; узлы освобождает поток, отпустивший последнюю
/// ссылку, — после чтений в других потоках. Без `Release`/`Acquire` на
/// счётчиках Miri сообщит о гонке освобождения с этими чтениями.
#[test]
fn threads_read_shared_nodes_last_owner_frees() {
    let (n, threads, rounds) = if cfg!(miri) {
        (50, 3, 3)
    } else {
        (2_000_u64, 8, 20)
    };
    for _ in 0..rounds {
        let base: SyncQueue<u64> = (0..n).collect();
        let clones: Vec<_> = (0..threads).map(|_| base.clone()).collect();
        drop(base);
        thread::scope(|s| {
            for mut q in clones {
                s.spawn(move || {
                    let mut sum = 0;
                    while let Some(v) = q.pop_mut() {
                        sum += v;
                    }
                    assert_eq!(sum, n * (n - 1) / 2);
                });
            }
        });
    }
}
