//! Узел персистентного стека со встроенным счётчиком ссылок — аналог `node`
//! и `node_retain`/`node_release` из `c/src/fqueue.c`.
//!
//! Вместо `Rc<Node<T>>` (счётчики strong и weak по 8 байт снаружи узла —
//! 32 байта для `T = i32`) счётчик `u32` лежит в самом узле, рядом со
//! значением, и занимает выравнивающий промежуток: для `i32` узел — 16 байт,
//! как в C. Слабых ссылок нет, поэтому освобождение — один декремент.
//!
//! Единственный модуль крейта с `unsafe`. Инварианты:
//! - `NodePtr` всегда указывает на живой `NodeBox`, отданный `Box::leak`;
//! - `refs` равен числу живых `NodePtr` на этот узел (≥ 1, пока узел жив);
//! - узел освобождается (`Box::from_raw`) ровно один раз — тем, кто уменьшил
//!   `refs` с 1 до 0 или забрал узел как единственный владелец.
//!
//! Тесты всего крейта прогоняются под Miri (`just rust-miri`).

#![allow(unsafe_code)]
#![warn(clippy::undocumented_unsafe_blocks)]

use std::cell::Cell;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::ptr::NonNull;

struct NodeBox<T> {
    refs: Cell<u32>,
    value: T,
    next: Option<NodePtr<T>>,
}

/// Владеющая ссылка на узел: клонирование — retain, удаление — release.
pub(crate) struct NodePtr<T> {
    ptr: NonNull<NodeBox<T>>,
    /// Для drop check: `NodePtr` владеет `NodeBox<T>`, а значит и `T`.
    _owns: PhantomData<NodeBox<T>>,
}

impl<T> NodePtr<T> {
    /// Новый узел со счётчиком 1 — аналог `cons`.
    pub(crate) fn new(value: T, next: Option<Self>) -> Self {
        let node = Box::new(NodeBox {
            refs: Cell::new(1),
            value,
            next,
        });
        Self {
            ptr: NonNull::from(Box::leak(node)),
            _owns: PhantomData,
        }
    }

    fn inner(&self) -> &NodeBox<T> {
        // SAFETY: пока жив `self`, он держит одну из `refs` ссылок, и узел не
        // освобождён; изменяемых ссылок на `NodeBox` не бывает (счётчик — `Cell`).
        unsafe { self.ptr.as_ref() }
    }

    pub(crate) fn value(&self) -> &T {
        &self.inner().value
    }

    pub(crate) fn next(&self) -> Option<&Self> {
        self.inner().next.as_ref()
    }

    /// Разобрать узел на значение и хвост, если `self` — единственный
    /// владелец; иначе вернуть ссылку обратно.
    pub(crate) fn into_parts(self) -> Result<(T, Option<Self>), Self> {
        if self.inner().refs.get() != 1 {
            return Err(self);
        }
        let this = ManuallyDrop::new(self);
        // SAFETY: `refs == 1` — других `NodePtr` на узел нет, а наш ни
        // `Drop`, ни повторно уже не используется (`ManuallyDrop`). Указатель
        // получен из `Box::leak`, так что владение возвращается `Box` ровно раз.
        let node = unsafe { Box::from_raw(this.ptr.as_ptr()) };
        let NodeBox { value, next, .. } = *node;
        Ok((value, next))
    }

    /// Прочитать счётчик узла, чтобы его строка кэша начала загружаться
    /// заранее, вне критического пути следующего шага.
    ///
    /// В C то же самое делает `node_advance` неявно: `node_retain(n->next)`
    /// трогает следующий узел сразу. Без этого при неудачной раскладке кучи
    /// (переиспользованные, разбросанные блоки) промах приходится на
    /// критический путь — `mixed 10^7` замедлялся до 1.4× (см.
    /// `docs/benchmarks.md`).
    pub(crate) fn touch(&self) {
        std::hint::black_box(self.inner().refs.get());
    }

    #[cfg(test)]
    pub(crate) fn ptr_eq(a: &Self, b: &Self) -> bool {
        a.ptr == b.ptr
    }
}

impl<T> Clone for NodePtr<T> {
    fn clone(&self) -> Self {
        let refs = &self.inner().refs;
        // Переполнение означает 2^32 живых ссылок; как и `Rc`, не продолжаем.
        let Some(n) = refs.get().checked_add(1) else {
            std::process::abort();
        };
        refs.set(n);
        Self {
            ptr: self.ptr,
            _owns: PhantomData,
        }
    }
}

/// Release с итеративным освобождением цепочки — аналог `node_release`.
///
/// Рекурсивный drop (`NodeBox` → `next` → `NodeBox` → ...) переполнил бы стек
/// вызовов на длинных списках, поэтому хвост умершего узла освобождается в
/// том же цикле, пока узлы умирают; на первом живом цикл останавливается.
impl<T> Drop for NodePtr<T> {
    fn drop(&mut self) {
        let mut ptr = self.ptr;
        loop {
            // SAFETY: мы держим одну ссылку на узел `ptr` (сначала — `self`,
            // далее — `next`, забранный из освобождённого узла), он жив.
            let refs = unsafe { &ptr.as_ref().refs };
            let n = refs.get() - 1;
            if n != 0 {
                refs.set(n);
                return;
            }
            // SAFETY: мы были последним владельцем (`refs` дошёл до 0), ссылок
            // на узел больше нет, `refs` выше больше не используется.
            let node = unsafe { Box::from_raw(ptr.as_ptr()) };
            let NodeBox { value, next, .. } = *node;
            drop(value);
            let Some(next) = next else { return };
            // Ссылка `next` переходит к циклу: её release — следующая итерация.
            ptr = ManuallyDrop::new(next).ptr;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    #[test]
    fn node_layout_matches_c() {
        // C: `struct node { int value; int refcount; struct node *next; }`.
        assert_eq!(size_of::<NodeBox<i32>>(), 16);
        assert_eq!(size_of::<Option<NodePtr<i32>>>(), 8);
    }

    #[test]
    fn clone_and_drop_track_refs() {
        let a = NodePtr::new(1, None);
        let b = a.clone();
        assert_eq!(a.inner().refs.get(), 2);
        assert!(NodePtr::ptr_eq(&a, &b));
        drop(b);
        assert_eq!(a.inner().refs.get(), 1);
    }

    #[test]
    fn into_parts_only_for_unique_owner() {
        let tail = NodePtr::new(1, None);
        let a = NodePtr::new(2, Some(tail.clone()));
        let keep = a.clone();

        let Err(a) = a.into_parts() else {
            panic!("узел разделяемый");
        };
        drop(keep);
        let (value, next) = a.into_parts().ok().expect("теперь владелец один");
        assert_eq!(value, 2);
        assert!(NodePtr::ptr_eq(&next.unwrap(), &tail));
    }

    #[test]
    fn drop_releases_values_and_stops_at_shared_node() {
        let tracker = Rc::new(());
        let shared = NodePtr::new(Rc::clone(&tracker), None);
        let mut head = Some(shared.clone());
        for _ in 0..3 {
            head = Some(NodePtr::new(Rc::clone(&tracker), head));
        }
        assert_eq!(Rc::strong_count(&tracker), 5);

        drop(head);
        assert_eq!(Rc::strong_count(&tracker), 2);
        assert_eq!(shared.inner().refs.get(), 1);

        drop(shared);
        assert_eq!(Rc::strong_count(&tracker), 1);
    }
}
