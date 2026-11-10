//! Ветвящаяся история с тем же детерминированным выбором родителей, что и
//! сценарий `branching` в бенчмарке (`examples/fqueue_bench.rs`,
//! `c/c/bench/fqueue_bench.c`), против эталонной модели для каждой версии.

// Арифметика генератора намеренно повторяет C-драйвер буквально.
#![allow(
    clippy::verbose_bit_mask,
    clippy::manual_is_multiple_of,
    clippy::cast_possible_truncation
)]

use std::collections::VecDeque;
use std::rc::Rc;

use hood_melville::Queue;

const BRANCHES: u64 = 8;

/// Генератор бенчмарка: K активных веток, раз в 64 шага — ответвление от
/// случайной версии из всей истории. `next` возвращает (родитель, pop?), а
/// `commit` запоминает новую версию как вершину ветки.
struct Choices {
    x: u64,
    heads: [usize; BRANCHES as usize],
    slot: usize,
}

impl Choices {
    fn new() -> Self {
        Self {
            x: 0x9E37_79B9_7F4A_7C15,
            heads: [0; BRANCHES as usize],
            slot: 0,
        }
    }

    fn next(&mut self, len: usize) -> (usize, bool) {
        let x = &mut self.x;
        *x ^= *x << 13;
        *x ^= *x >> 7;
        *x ^= *x << 17;
        self.slot = ((*x >> 3) % BRANCHES) as usize;
        let idx = if *x & 63 == 0 {
            ((*x >> 12) % len as u64) as usize
        } else {
            self.heads[self.slot]
        };
        (idx, (*x >> 40) % 3 == 0)
    }

    fn commit(&mut self, new_index: usize) {
        self.heads[self.slot] = new_index;
    }
}

fn contents<T: Clone>(q: &Queue<T>) -> Vec<T> {
    q.iter().collect()
}

#[test]
fn branching_history_matches_model() {
    let ops = if cfg!(miri) { 300 } else { 5_000 };
    let mut choices = Choices::new();
    let mut versions = vec![(Queue::new(), VecDeque::new())];
    let mut max_len = 0;

    for k in 0..ops {
        let (idx, want_pop) = choices.next(versions.len());
        let (parent, model) = &versions[idx];
        let before = contents(parent);

        let (child, child_model) = if want_pop && !parent.is_empty() {
            let mut m = model.clone();
            m.pop_front();
            (parent.pop().expect("родитель не пуст"), m)
        } else {
            let mut m = model.clone();
            m.push_back(k);
            (parent.push(k), m)
        };

        // Родитель не изменился, потомок соответствует модели.
        assert_eq!(
            contents(parent),
            before,
            "шаг {k}: изменился родитель {idx}"
        );
        assert_eq!(child.len(), child_model.len(), "шаг {k}");
        assert_eq!(child.front(), child_model.front(), "шаг {k}");
        max_len = max_len.max(child.len());
        choices.commit(versions.len());
        versions.push((child, child_model));
    }

    // Очереди успевали вырасти (ротации шли на длинных стеках), а ответвления
    // от старых версий действительно были.
    assert!(
        max_len > if cfg!(miri) { 20 } else { 200 },
        "max_len = {max_len}"
    );
    for (i, (q, m)) in versions.iter().enumerate() {
        assert!(q.iter().eq(m.iter().copied()), "версия {i}");
    }
}

#[test]
fn branching_history_frees_every_element() {
    let ops = if cfg!(miri) { 300 } else { 5_000 };
    let tracker = Rc::new(());
    let mut choices = Choices::new();
    let mut versions = vec![Queue::new()];
    for _ in 0..ops {
        let (idx, want_pop) = choices.next(versions.len());
        let parent = &versions[idx];
        let child = if want_pop && !parent.is_empty() {
            parent.pop().expect("родитель не пуст")
        } else {
            parent.push(Rc::clone(&tracker))
        };
        choices.commit(versions.len());
        versions.push(child);
    }
    assert!(Rc::strong_count(&tracker) > 1);
    drop(versions);
    assert_eq!(Rc::strong_count(&tracker), 1);
}
