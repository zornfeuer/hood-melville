//! Перенос сценариев `[fqueue]` из `c/tests/fqueue_test.cxx`.

use std::collections::VecDeque;

use hood_melville::Queue;

/// Аналог `drain` из C-тестов: сама очередь при этом не меняется.
fn drain(q: &Queue<i32>) -> Vec<i32> {
    let mut out = Vec::new();
    let mut q = q.clone();
    while let Some(&x) = q.front() {
        out.push(x);
        q = q.pop().unwrap();
    }
    out
}

fn front(q: &Queue<i32>) -> i32 {
    *q.front().expect("очередь не пуста")
}

#[test]
fn create_produces_an_empty_queue() {
    let q: Queue<i32> = Queue::new();
    assert!(q.is_empty());
    assert_eq!(q.len(), 0);
    assert_eq!(q.front(), None);
    assert!(q.pop().is_none());
}

#[test]
fn push_returns_a_new_queue_without_modifying_the_original() {
    let q0 = Queue::new();
    let q1 = q0.push(42);

    assert!(q0.is_empty());
    assert_eq!(q0.len(), 0);
    assert_eq!(q0.front(), None);

    assert!(!q1.is_empty());
    assert_eq!(q1.len(), 1);
    assert_eq!(front(&q1), 42);
}

#[test]
fn pop_returns_a_new_queue_without_modifying_the_original() {
    let q1 = Queue::new().push(10);
    let q2 = q1.pop().unwrap();

    assert_eq!(q1.len(), 1);
    assert_eq!(front(&q1), 10);

    assert!(q2.is_empty());
    assert_eq!(q2.len(), 0);
    assert_eq!(q2.front(), None);
}

#[test]
fn pop_on_empty_queue_returns_none() {
    assert!(Queue::<i32>::new().pop().is_none());
}

#[test]
fn preserves_fifo_order_for_sequential_push_then_pop() {
    let q = Queue::new().push(1).push(2).push(3).push(4).push(5);
    assert_eq!(q.len(), 5);
    assert_eq!(drain(&q), [1, 2, 3, 4, 5]);
}

#[test]
fn len_and_is_empty_are_consistent() {
    let mut q = Queue::new();
    for i in 0..8 {
        assert_eq!(q.len(), i);
        assert_eq!(q.is_empty(), i == 0);
        q = q.push(i * 10);
    }
    assert_eq!(q.len(), 8);

    for remaining in (1..=8).rev() {
        assert_eq!(q.len(), remaining);
        assert!(!q.is_empty());
        q = q.pop().unwrap();
    }
    assert_eq!(q.len(), 0);
    assert!(q.is_empty());
}

#[test]
fn interleaved_push_and_pop() {
    let q = Queue::new().push(10).push(20);
    assert_eq!(front(&q), 10);
    let q = q.pop().unwrap().push(30);
    assert_eq!(front(&q), 20);
    let q = q.pop().unwrap();
    assert_eq!(front(&q), 30);
    let q = q.pop().unwrap();
    assert!(q.is_empty());
}

#[test]
fn two_queues_derived_from_the_same_base_are_independent() {
    let base = Queue::new().push(1).push(2);
    let branch_a = base.push(3);
    let branch_b = base.push(99);

    assert_eq!(base.len(), 2);
    assert_eq!(front(&base), 1);

    assert_eq!(drain(&branch_a), [1, 2, 3]);
    assert_eq!(drain(&branch_b), [1, 2, 99]);

    assert_eq!(base.len(), 2);
    assert_eq!(front(&base), 1);
}

#[test]
fn pop_derived_queues_are_independent() {
    let q = Queue::new().push(1).push(2).push(3);
    let after_pop = q.pop().unwrap();
    let after_pop2 = after_pop.pop().unwrap();

    assert_eq!(front(&q), 1);
    assert_eq!(front(&after_pop), 2);
    assert_eq!(front(&after_pop2), 3);
}

#[test]
fn push_heavy_sequence_forces_a_full_rotation_cycle() {
    let q = Queue::new().push(1).push(2).push(3);

    assert_eq!(front(&q), 1);
    let q = q.pop().unwrap().push(4).push(5);
    assert_eq!(front(&q), 2);
    let mut q = q.pop().unwrap().push(6);
    for expected in 3..=6 {
        assert_eq!(front(&q), expected);
        q = q.pop().unwrap();
    }
    assert!(q.is_empty());
}

#[test]
fn front_len_plus_one_pops_during_rotation() {
    // Третий push запускает ротацию при front_len = 1; два pop'а
    // исчерпывают старый фронт посреди ротации.
    let mut q = Queue::new().push(1).push(2).push(3);
    for expected in 1..=3 {
        assert_eq!(front(&q), expected);
        q = q.pop().unwrap();
    }
    assert!(q.is_empty());
}

#[test]
fn stress_200_operations_match_a_reference_queue() {
    let mut reference = VecDeque::new();
    let mut q = Queue::new();

    for i in 0..6 {
        q = q.push(i);
        reference.push_back(i);
    }

    // push, push, pop: очередь растёт и ротации запускаются повторно.
    for i in 6..106 {
        q = q.push(i).push(i * 100);
        reference.push_back(i);
        reference.push_back(i * 100);

        assert_eq!(q.front(), reference.front());
        assert_eq!(q.len(), reference.len());
        q = q.pop().unwrap();
        reference.pop_front();
    }

    while let Some(expected) = reference.pop_front() {
        assert_eq!(q.len(), reference.len() + 1);
        assert_eq!(front(&q), expected);
        q = q.pop().unwrap();
    }
    assert!(q.is_empty());
}
