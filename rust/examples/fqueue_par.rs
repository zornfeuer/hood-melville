//! Параллельная нагрузка на `SyncQueue` (#6). C-двойник — `c/bench/fqueue_par.c`,
//! Zig — `zig/bench/fqueue_par.zig`; сценарии и контрольные суммы совпадают.
//!
//! Использование:
//! - `fqueue_par share <pool|threads> <share> <P>` — независимые задачи,
//!   доля `share` (в процентах) их операций ветвится от общих версий;
//!   `pool` — пул `rayon` (`par_iter`, динамическое распределение задач),
//!   `threads` — `std::thread::scope`, задачи разбиты между P потоками
//!   заранее. Печатает контрольную сумму и время параллельной фазы, нс.
//! - `fqueue_par snapshot <R>` — писатель и R читателей снимков. Печатает
//!   контрольную сумму писателя, его время (нс), число прочитанных снимков и
//!   элементов.
//!
//! **share.** Перед параллельной фазой строятся `SHARED` общих версий: одна
//! линейная история (push, push, pop), сохраняется каждая 24-я версия, размеры
//! 0…504, узлы у версий в основном общие. Затем `TASKS` независимых задач по
//! `OPS` операций. У задачи своя версия; на каждом шаге родителем с
//! вероятностью `share`% становится случайная общая версия, иначе собственная.
//! Голова родителя читается, от него порождается потомок (pop или push
//! поровну), потомок заменяет собственную версию, прежняя освобождается.
//! При 0% задачи не касаются общих узлов, при 100% каждая операция ветвится
//! от общей версии: атомарные инкременты и декременты счётчиков общих узлов из
//! всех потоков сразу. Контрольная сумма не зависит от числа потоков и режима.
//!
//! **snapshot.** Писатель держит очередь из `SNAP_SIZE` элементов и меняет её
//! на месте (`push_mut`, затем `pop_mut`, `SNAP_PAIRS` раз); каждые `PERIOD`
//! пар публикует снимок — копию версии — в слот под мьютексом. Читатели, пока
//! писатель не закончил, берут последний снимок (копия под мьютексом) и
//! обходят его целиком. После публикации версия писателя делит все узлы со
//! снимком, и операции на месте идут по медленному пути, а обход читателей
//! трогает счётчики тех же узлов. Контрольная сумма — по значениям, извлечённым
//! писателем, от числа читателей не зависит.

use std::process::ExitCode;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use rayon::prelude::*;

type Q = hood_melville::SyncQueue<i32>;

const TASKS: u64 = 256;
const OPS: i32 = 40_000;
const SHARED: u64 = 64;

const SNAP_SIZE: i32 = 4096;
const SNAP_PAIRS: i32 = 1_000_000;
const PERIOD: i32 = 512;

fn xorshift(x: &mut u64) -> u64 {
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    *x
}

#[allow(clippy::cast_sign_loss, reason = "как `(uint64_t)x` в C")]
fn mix(h: u64, x: i32) -> u64 {
    h.wrapping_mul(31).wrapping_add(i64::from(x) as u64)
}

fn shared_versions() -> Vec<Q> {
    let mut versions = Vec::new();
    let mut q = Q::new_sync();
    for j in 0..i32::try_from(SHARED * 24).unwrap() {
        if j % 24 == 0 {
            versions.push(q.clone());
        }
        q = if j % 3 == 2 {
            q.pop().expect("история не пуста")
        } else {
            q.push(j)
        };
    }
    versions
}

#[allow(clippy::cast_possible_truncation, reason = "индекс < SHARED")]
fn run_task(shared: &[Q], share: u64, task: u64) -> u64 {
    let mut x = 0x9E37_79B9_7F4A_7C15 ^ (task + 1).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    let mut h = 0;
    let mut q = Q::new_sync();
    for k in 0..OPS {
        xorshift(&mut x);
        let parent = if x % 100 < share {
            &shared[((x >> 8) % SHARED) as usize]
        } else {
            &q
        };
        let child = match parent.front() {
            None => parent.push(k),
            Some(&value) => {
                h = mix(h, value);
                if (x >> 40) & 1 == 0 {
                    parent.pop().expect("родитель не пуст")
                } else {
                    parent.push(k)
                }
            }
        };
        q = child;
    }
    h
}

fn run_pool(shared: &[Q], share: u64, p: usize) -> (u64, Duration) {
    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(p)
        .build()
        .expect("пул потоков");
    let t0 = Instant::now();
    let h = pool.install(|| {
        (0..TASKS)
            .into_par_iter()
            .map(|t| run_task(shared, share, t))
            .reduce(|| 0, u64::wrapping_add)
    });
    (h, t0.elapsed())
}

fn run_threads(shared: &[Q], share: u64, p: usize) -> (u64, Duration) {
    let t0 = Instant::now();
    let h = std::thread::scope(|s| {
        let workers: Vec<_> = (0..p as u64)
            .map(|i| {
                s.spawn(move || {
                    (i..TASKS)
                        .step_by(p)
                        .map(|t| run_task(shared, share, t))
                        .fold(0, u64::wrapping_add)
                })
            })
            .collect();
        workers
            .into_iter()
            .map(|w| w.join().expect("поток нагрузки"))
            .fold(0, u64::wrapping_add)
    });
    (h, t0.elapsed())
}

fn run_snapshot(readers: usize) -> (u64, Duration, u64, u64) {
    let mut q = Q::new_sync();
    for i in 0..SNAP_SIZE {
        q.push_mut(i);
    }
    let slot = Mutex::new(q.clone());
    let done = AtomicBool::new(false);
    std::thread::scope(|s| {
        let workers: Vec<_> = (0..readers)
            .map(|_| {
                s.spawn(|| {
                    let (mut snapshots, mut elements, mut sum) = (0, 0, 0);
                    while !done.load(Ordering::Relaxed) {
                        let snapshot = slot.lock().expect("слот снимка").clone();
                        for v in snapshot {
                            sum = mix(sum, v);
                            elements += 1;
                        }
                        snapshots += 1;
                    }
                    std::hint::black_box(sum);
                    (snapshots, elements)
                })
            })
            .collect();
        let t0 = Instant::now();
        let mut h = 0;
        for k in 0..SNAP_PAIRS {
            q.push_mut(SNAP_SIZE + k);
            h = mix(h, q.pop_mut().expect("очередь не пуста"));
            if (k + 1) % PERIOD == 0 {
                // Копия снимается и прежний снимок освобождается вне мьютекса.
                let snapshot = q.clone();
                let old = std::mem::replace(&mut *slot.lock().expect("слот снимка"), snapshot);
                drop(old);
            }
        }
        let elapsed = t0.elapsed();
        done.store(true, Ordering::Relaxed);
        let (snapshots, elements) = workers
            .into_iter()
            .map(|w| w.join().expect("поток читателя"))
            .fold((0, 0), |(s, e), (ws, we)| (s + ws, e + we));
        (h, elapsed, snapshots, elements)
    })
}

fn usage() -> ExitCode {
    eprintln!("usage: fqueue_par share <pool|threads> <share 0..100> <threads>");
    eprintln!("       fqueue_par snapshot <readers>");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    match args.iter().map(String::as_str).collect::<Vec<_>>()[1..] {
        ["share", mode, share, p] => {
            let (Ok(share), Ok(p)) = (share.parse::<u64>(), p.parse::<usize>()) else {
                return usage();
            };
            if share > 100 || p == 0 {
                return usage();
            }
            let shared = shared_versions();
            let (h, elapsed) = match mode {
                "pool" => run_pool(&shared, share, p),
                "threads" => run_threads(&shared, share, p),
                _ => return usage(),
            };
            println!("{h} {}", elapsed.as_nanos());
        }
        ["snapshot", readers] => {
            let Ok(readers) = readers.parse() else {
                return usage();
            };
            let (h, elapsed, snapshots, elements) = run_snapshot(readers);
            println!("{h} {} {snapshots} {elements}", elapsed.as_nanos());
        }
        _ => return usage(),
    }
    ExitCode::SUCCESS
}
