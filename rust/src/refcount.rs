//! Политика счётчика ссылок в узлах: однопоточная [`Local`] (`Cell<u32>`, как
//! у `Rc`) или потокобезопасная [`Atomic`] (`AtomicU32`, как у `Arc`).
//!
//! Параметр очереди: [`Queue<T>`](crate::Queue) — это `Queue<T, Local>`,
//! [`SyncQueue<T>`](crate::SyncQueue) — `Queue<T, Atomic>`. Алгоритм и код у них
//! общие, различаются только операции над счётчиком; однопоточный вариант
//! компилируется в тот же код, что и без параметра.

use std::cell::Cell;
use std::sync::atomic::{AtomicU32, Ordering, fence};

mod sealed {
    pub trait Sealed {}
}

/// Политика счётчика ссылок узлов; реализуют только [`Local`] и [`Atomic`].
pub trait RefCount: sealed::Sealed + 'static {
    #[doc(hidden)]
    type Counter;

    #[doc(hidden)]
    fn one() -> Self::Counter;

    /// Добавить ссылку; при переполнении — `abort`, как у `Rc`/`Arc`.
    #[doc(hidden)]
    fn increment(counter: &Self::Counter);

    /// Убрать ссылку; `true`, если она была последней. В этом случае все
    /// обращения других владельцев к узлу видны вызывающему (он освобождает
    /// узел).
    #[doc(hidden)]
    fn decrement(counter: &Self::Counter) -> bool;

    /// Вызывающий — единственный владелец (его ссылка — одна-единственная):
    /// можно забрать значение из узла и освободить его.
    #[doc(hidden)]
    fn is_unique(counter: &Self::Counter) -> bool;

    /// Прочитать счётчик ради загрузки строки кэша заранее; значение не важно.
    #[doc(hidden)]
    fn touch(counter: &Self::Counter);

    /// Текущее значение — только для тестов.
    #[doc(hidden)]
    fn get(counter: &Self::Counter) -> u32;
}

/// Однопоточный счётчик (`Cell<u32>`): версии нельзя передавать между потоками.
pub enum Local {}

/// Потокобезопасный счётчик (`AtomicU32`): версии — `Send + Sync`
/// при `T: Send + Sync`.
pub enum Atomic {}

impl sealed::Sealed for Local {}
impl sealed::Sealed for Atomic {}

impl RefCount for Local {
    type Counter = Cell<u32>;

    #[inline]
    fn one() -> Cell<u32> {
        Cell::new(1)
    }

    #[inline]
    fn increment(counter: &Cell<u32>) {
        // Переполнение означает 2^32 живых ссылок; как и `Rc`, не продолжаем.
        let Some(n) = counter.get().checked_add(1) else {
            std::process::abort();
        };
        counter.set(n);
    }

    #[inline]
    fn decrement(counter: &Cell<u32>) -> bool {
        let n = counter.get() - 1;
        if n != 0 {
            counter.set(n);
            return false;
        }
        true
    }

    #[inline]
    fn is_unique(counter: &Cell<u32>) -> bool {
        counter.get() == 1
    }

    #[inline]
    fn touch(counter: &Cell<u32>) {
        std::hint::black_box(counter.get());
    }

    fn get(counter: &Cell<u32>) -> u32 {
        counter.get()
    }
}

/// Предел ссылок атомарного счётчика, как `isize::MAX` у `Arc`: проверка идёт
/// после инкремента, и до обёртывания `u32` остаётся запас в 2^31 инкрементов —
/// столько потоков одновременно не успеют проскочить проверку.
const ATOMIC_MAX_REFS: u32 = u32::MAX / 2;

// Порядки памяти — как у `Arc` (подробное обоснование — в его исходниках):
// - инкремент `Relaxed`: новую ссылку делают из уже имеющейся, а её передача
//   другому потоку сама синхронизирует;
// - декремент `Release`, а последний владелец перед освобождением делает
//   `Acquire`-барьер: все обращения других владельцев к узлу (чтение `value`
//   и `next`) происходят до освобождения;
// - проверка уникальности `Acquire` по той же причине: после неё узел
//   забирают (значение и хвост) и освобождают.
impl RefCount for Atomic {
    type Counter = AtomicU32;

    #[inline]
    fn one() -> AtomicU32 {
        AtomicU32::new(1)
    }

    #[inline]
    fn increment(counter: &AtomicU32) {
        if counter.fetch_add(1, Ordering::Relaxed) > ATOMIC_MAX_REFS {
            std::process::abort();
        }
    }

    #[inline]
    fn decrement(counter: &AtomicU32) -> bool {
        if counter.fetch_sub(1, Ordering::Release) != 1 {
            return false;
        }
        fence(Ordering::Acquire);
        true
    }

    #[inline]
    fn is_unique(counter: &AtomicU32) -> bool {
        counter.load(Ordering::Acquire) == 1
    }

    #[inline]
    fn touch(counter: &AtomicU32) {
        std::hint::black_box(counter.load(Ordering::Relaxed));
    }

    fn get(counter: &AtomicU32) -> u32 {
        counter.load(Ordering::Relaxed)
    }
}
