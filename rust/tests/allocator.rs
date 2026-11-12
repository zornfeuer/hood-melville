//! Очередь с пользовательским аллокатором (`Queue<T, R, A>`): аллокатор с
//! размером и состоянием, клоны которого лежат в узлах.

use std::alloc::{AllocError, Allocator, Global, Layout};
use std::cell::Cell;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicIsize, Ordering::Relaxed};
use std::thread;

use hood_melville::{Queue, SyncQueue};

/// Считает живые блоки; каждый клон — ещё одна ссылка на счётчик, так что
/// `Rc::strong_count` показывает, сколько клонов аллокатора живо.
#[derive(Clone)]
struct Counting(Rc<Cell<isize>>);

// SAFETY: делегирует `Global`; клоны делят счётчик и взаимозаменяемы.
unsafe impl Allocator for Counting {
    fn allocate(&self, layout: Layout) -> Result<NonNull<[u8]>, AllocError> {
        self.0.set(self.0.get() + 1);
        Global.allocate(layout)
    }

    unsafe fn deallocate(&self, ptr: NonNull<u8>, layout: Layout) {
        self.0.set(self.0.get() - 1);
        // SAFETY: блок выделен `Global` в `allocate`.
        unsafe { Global.deallocate(ptr, layout) };
    }
}

#[test]
fn stateful_allocator_clones_live_in_nodes_and_are_dropped_once() {
    let live = Rc::new(Cell::new(0));
    let alloc = Counting(Rc::clone(&live));
    {
        let mut q = Queue::new_in(alloc.clone());
        let mut versions = Vec::new();
        for i in 0..1000 {
            q.push_mut(i);
            if i % 7 == 0 {
                versions.push(q.clone());
            }
            if i % 3 == 0 {
                q = q.pop().unwrap();
            }
        }
        assert!(live.get() > 0);
        assert_eq!(q.iter().count(), q.len());
        // Клоны аллокатора — в узлах и в версиях.
        assert!(Rc::strong_count(&live) > 2);
        drop(versions);
        while q.pop_mut().is_some() {}
    }
    assert_eq!(live.get(), 0, "все узлы возвращены аллокатору");
    assert_eq!(
        Rc::strong_count(&live),
        2,
        "клоны из узлов удалены ровно раз"
    );
    drop(alloc);
}

#[test]
fn allocator_with_size_grows_only_queue_and_node() {
    assert_eq!(size_of::<Queue<i32>>(), 64);
    assert_eq!(
        size_of::<Queue<i32, hood_melville::refcount::Local, &Global>>(),
        72
    );
}

/// Потокобезопасный счётчик блоков для `SyncQueue`.
#[derive(Clone)]
struct SharedCounting(Arc<AtomicIsize>);

// SAFETY: как у `Counting`.
unsafe impl Allocator for SharedCounting {
    fn allocate(&self, layout: Layout) -> Result<NonNull<[u8]>, AllocError> {
        self.0.fetch_add(1, Relaxed);
        Global.allocate(layout)
    }

    unsafe fn deallocate(&self, ptr: NonNull<u8>, layout: Layout) {
        self.0.fetch_sub(1, Relaxed);
        // SAFETY: блок выделен `Global` в `allocate`.
        unsafe { Global.deallocate(ptr, layout) };
    }
}

#[test]
fn sync_queue_with_stateful_allocator_across_threads() {
    let live = Arc::new(AtomicIsize::new(0));
    let alloc = SharedCounting(Arc::clone(&live));
    let mut base = SyncQueue::new_sync_in(alloc);
    base.extend(0..100);
    let rounds = if cfg!(miri) { 10 } else { 1000 };
    thread::scope(|s| {
        for t in 0..4 {
            let base = &base;
            s.spawn(move || {
                for i in 0..rounds {
                    let q = base.push(t * rounds + i).pop().unwrap();
                    assert_eq!(q.len(), 100);
                }
            });
        }
    });
    drop(base);
    assert_eq!(live.load(Relaxed), 0);
    assert_eq!(Arc::strong_count(&live), 1);
}
