//! Расписание ротации по операциям одинаково в C, Rust и Zig (#9): на одной
//! истории каждая персистентная операция создаёт одно и то же число узлов.
//! Последовательность `EXPECTED` — общая с `c/tests/fqueue_limits_test.cxx`
//! и `zig/src/tests.zig`; число новых узлов операции однозначно задаётся
//! тем, сколько шагов ротации и каких фаз она сделала.
//!
//! Узлы считаются глобальным аллокатором по размеру узла (16 байт для
//! `u32`), только в потоке теста — потоки тестового раннера не мешают.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

use hood_melville::Queue;

struct Counting;

thread_local! {
    static NODES: Cell<Option<usize>> = const { Cell::new(None) };
}

// SAFETY: всё делегируется `System`; счётчик — thread-local без выделений.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        if l.size() == 16 {
            let _ = NODES.try_with(|n| {
                if let Some(k) = n.get() {
                    n.set(Some(k + 1));
                }
            });
        }
        // SAFETY: контракт `GlobalAlloc::alloc` передан как есть.
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        // SAFETY: контракт `GlobalAlloc::dealloc` передан как есть.
        unsafe { System.dealloc(p, l) }
    }
}

#[global_allocator]
static A: Counting = Counting;

/// История: 30 push, затем 60 раз push + pop, затем pop до пустой (не
/// больше 31 элемента: в C тест идёт со сниженным пределом длины 40).
/// Каждый элемент — число узлов, созданных операцией.
fn schedule() -> Vec<usize> {
    let mut out = Vec::new();
    let mut q: Queue<u32> = Queue::new();
    let mut op = |q: &Queue<u32>, push: Option<u32>| -> Queue<u32> {
        NODES.with(|n| n.set(Some(0)));
        let next = match push {
            Some(x) => q.push(x),
            None => q.pop().expect("непустая"),
        };
        out.push(NODES.with(|n| n.replace(None)).unwrap());
        next
    };
    for i in 0..30 {
        q = op(&q, Some(i));
    }
    for i in 30..90 {
        q = op(&q, Some(i));
        q = op(&q, None);
    }
    while !q.is_empty() {
        q = op(&q, None);
    }
    out
}

#[rustfmt::skip]
const EXPECTED: [usize; 180] = [
    2, 1, 3, 2, 2, 1, 3, 3, 3, 2, 2, 2, 2, 1, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 1,
    3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 0, 1, 0,
    1, 0, 1, 0, 1, 0, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 1, 2, 1, 2, 1, 2, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
];

#[test]
fn rotation_schedule_matches_c_and_zig() {
    let got = schedule();
    if got[..] != EXPECTED[..] {
        panic!("расписание: {got:?}");
    }
}
