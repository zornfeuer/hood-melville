//! Узел персистентного стека со встроенным счётчиком ссылок — аналог `node`
//! и `node_retain`/`node_release` из `c/include/fqueue_template.h`
//! (`*_impl_retain`/`*_impl_release`).
//!
//! Вместо `Rc<Node<T>>` (счётчики strong и weak по 8 байт снаружи узла —
//! 32 байта для `T = i32`) счётчик `u32` лежит в самом узле, рядом со
//! значением, и занимает выравнивающий промежуток: для `i32` узел — 16 байт,
//! как в C. Слабых ссылок нет, поэтому освобождение — один декремент.
//!
//! Счётчик — по политике `R` (`crate::refcount`): `Cell<u32>` или
//! `AtomicU32`; в обоих случаях 4 байта, узел для `i32` — 16.
//!
//! Модуль крейта с `unsafe` (ещё `storage` и `pool`). Инварианты:
//! - `NodePtr` всегда указывает на живой `NodeBox`, отданный `Box::leak`
//!   (с фичей `pool` — `R::Storage::place`: `Box` или пул);
//! - `refs` равен числу живых `NodePtr` на этот узел (≥ 1, пока узел жив);
//! - узел освобождается (`Box::from_raw`) ровно один раз — тем, кто уменьшил
//!   `refs` с 1 до 0 или забрал узел как единственный владелец;
//! - `value` и `next` после создания узла только читаются; с атомарным
//!   счётчиком их читают из нескольких потоков (отсюда `T: Sync`), а
//!   освобождение синхронизировано с этими чтениями порядками памяти счётчика.
//!
//! Тесты всего крейта прогоняются под Miri (`just rust-miri`).
//!
//! Функции, создающие и забирающие узлы, — в двух вариантах: без фичи `pool`
//! прежний текст с `Box`, с фичей — через `R::Storage`. Даже мелкая
//! перестройка MIR (общий макрос, временный кортеж) меняла решения LLVM о
//! встраивании в нагрузке, а код очереди без пула должен остаться побайтно
//! прежним (сверка машинного кода — `docs/pool.md`).

#![allow(unsafe_code)]
#![warn(clippy::undocumented_unsafe_blocks)]

use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::ptr::NonNull;

#[cfg(feature = "pool")]
use crate::refcount::NodeStorage;
use crate::refcount::{RefCount, ThreadSafe};

struct NodeBox<T, R: RefCount> {
    refs: R::Counter,
    value: T,
    next: Option<NodePtr<T, R>>,
}

/// Владеющая ссылка на узел: клонирование — retain, удаление — release.
pub(crate) struct NodePtr<T, R: RefCount> {
    ptr: NonNull<NodeBox<T, R>>,
    /// Для drop check: `NodePtr` владеет `NodeBox`, а значит и `T`.
    _owns: PhantomData<NodeBox<T, R>>,
}

// SAFETY: как у `Arc<T>`: ссылку можно передать другому потоку и разделять
// между потоками, если `T` можно и передавать (последний владелец забирает
// или удаляет значение в своём потоке), и читать из нескольких потоков.
// Счётчик атомарный, а освобождение синхронизировано с чтениями
// (`Release`/`Acquire`, см. `refcount`). С `Local` (`Cell`) — ни то ни другое:
// `NonNull` не `Send`/`Sync`, а `Rotation`/`Queue` наследуют это.
// Хранилище узлов (`Box`, пул) потокобезопасно в обоих случаях.
unsafe impl<T: Send + Sync, R: ThreadSafe> Send for NodePtr<T, R> {}
// SAFETY: см. выше.
unsafe impl<T: Send + Sync, R: ThreadSafe> Sync for NodePtr<T, R> {}

impl<T, R: RefCount> NodePtr<T, R> {
    /// Новый узел со счётчиком 1 — аналог `cons`.
    #[inline]
    #[cfg(not(feature = "pool"))]
    pub(crate) fn new(value: T, next: Option<Self>) -> Self {
        let node = Box::new(NodeBox {
            refs: R::one(),
            value,
            next,
        });
        Self {
            ptr: NonNull::from(Box::leak(node)),
            _owns: PhantomData,
        }
    }

    #[cfg(feature = "pool")]
    #[inline]
    pub(crate) fn new(value: T, next: Option<Self>) -> Self {
        Self {
            ptr: R::Storage::place(NodeBox {
                refs: R::one(),
                value,
                next,
            }),
            _owns: PhantomData,
        }
    }

    #[inline]
    fn inner(&self) -> &NodeBox<T, R> {
        // SAFETY: пока жив `self`, он держит одну из `refs` ссылок, и узел не
        // освобождён; изменяемых ссылок на `NodeBox` не бывает (счётчик меняется
        // через `Cell`/атомарные операции).
        unsafe { self.ptr.as_ref() }
    }

    #[inline]
    pub(crate) fn value(&self) -> &T {
        &self.inner().value
    }

    #[inline]
    pub(crate) fn next(&self) -> Option<&Self> {
        self.inner().next.as_ref()
    }

    /// Разобрать узел на значение и хвост, если `self` — единственный
    /// владелец; иначе вернуть ссылку обратно.
    #[inline]
    #[cfg(not(feature = "pool"))]
    pub(crate) fn into_parts(self) -> Result<(T, Option<Self>), Self> {
        if !R::is_unique(&self.inner().refs) {
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

    #[cfg(feature = "pool")]
    #[inline]
    pub(crate) fn into_parts(self) -> Result<(T, Option<Self>), Self> {
        if !R::is_unique(&self.inner().refs) {
            return Err(self);
        }
        let this = ManuallyDrop::new(self);
        // SAFETY: `refs == 1` — других `NodePtr` на узел нет, а наш ни
        // `Drop`, ни повторно уже не используется (`ManuallyDrop`). Указатель
        // получен из `R::Storage::place`, узел забирается ровно раз.
        let NodeBox { value, next, .. } = unsafe { R::Storage::take(this.ptr) };
        Ok((value, next))
    }

    /// Заменить `self` ссылкой на следующий узел и вернуть значение узла —
    /// как `into_parts`, но на месте; `None`, если следующего узла нет
    /// (`self` тогда не меняется). Для непустого стека (`NonEmptyStack`),
    /// который не может временно остаться без вершины.
    ///
    /// Единственный владелец забирает значение из узла, а ссылка узла на
    /// хвост переходит к `self` без изменения счётчика; разделяемый узел
    /// остаётся другим версиям, значение клонируется.
    #[inline]
    #[cfg(not(feature = "pool"))]
    pub(crate) fn advance_unless_last(&mut self) -> Option<T>
    where
        T: Clone,
    {
        let next = self.inner().next.as_ref()?;
        if !R::is_unique(&self.inner().refs) {
            // Паника `clone` оставляет `self` нетронутым.
            let value = self.value().clone();
            *self = next.clone();
            return Some(value);
        }
        // SAFETY: `refs == 1` — других `NodePtr` на узел нет; указатель из
        // `Box::leak`, владение возвращается `Box` ровно раз. До записи
        // `self.ptr` ниже ничто не паникует и не читает `self`: висячий
        // указатель не виден ни `Drop`, ни вызывающему.
        let node = unsafe { Box::from_raw(self.ptr.as_ptr()) };
        let NodeBox { value, next, .. } = *node;
        // `next` проверен выше и с тех пор не менялся.
        let Some(next) = next else {
            std::process::abort()
        };
        self.ptr = ManuallyDrop::new(next).ptr;
        self.touch();
        Some(value)
    }

    #[cfg(feature = "pool")]
    #[inline]
    pub(crate) fn advance_unless_last(&mut self) -> Option<T>
    where
        T: Clone,
    {
        let next = self.inner().next.as_ref()?;
        if !R::is_unique(&self.inner().refs) {
            // Паника `clone` оставляет `self` нетронутым.
            let value = self.value().clone();
            *self = next.clone();
            return Some(value);
        }
        // SAFETY: `refs == 1` — других `NodePtr` на узел нет; указатель из
        // `R::Storage::place`, узел забирается ровно раз. До записи
        // `self.ptr` ниже ничто не паникует и не читает `self`: висячий
        // указатель не виден ни `Drop`, ни вызывающему.
        let NodeBox { value, next, .. } = unsafe { R::Storage::take(self.ptr) };
        // `next` проверен выше и с тех пор не менялся.
        let Some(next) = next else {
            std::process::abort()
        };
        self.ptr = ManuallyDrop::new(next).ptr;
        self.touch();
        Some(value)
    }

    /// Прочитать счётчик узла, чтобы его строка кэша начала загружаться
    /// заранее, вне критического пути следующего шага.
    ///
    /// В C то же самое делает `node_advance` неявно: `node_retain(n->next)`
    /// трогает следующий узел сразу. Без этого при неудачной раскладке кучи
    /// (переиспользованные, разбросанные блоки) промах приходится на
    /// критический путь — `mixed 10^7` замедлялся до 1.4× (см.
    /// `docs/benchmarks.md`).
    #[inline]
    pub(crate) fn touch(&self) {
        R::touch(&self.inner().refs);
    }

    #[cfg(test)]
    pub(crate) fn ptr_eq(a: &Self, b: &Self) -> bool {
        a.ptr == b.ptr
    }
}

impl<T, R: RefCount> Clone for NodePtr<T, R> {
    #[inline]
    fn clone(&self) -> Self {
        R::increment(&self.inner().refs);
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
impl<T, R: RefCount> Drop for NodePtr<T, R> {
    #[inline]
    #[cfg(not(feature = "pool"))]
    fn drop(&mut self) {
        let mut ptr = self.ptr;
        loop {
            // SAFETY: мы держим одну ссылку на узел `ptr` (сначала — `self`,
            // далее — `next`, забранный из освобождённого узла), он жив.
            let refs = unsafe { &ptr.as_ref().refs };
            if !R::decrement(refs) {
                return;
            }
            // SAFETY: мы были последним владельцем (`refs` дошёл до 0), ссылок
            // на узел больше нет, `refs` выше больше не используется; с
            // атомарным счётчиком `decrement` синхронизировал нас со всеми
            // обращениями прежних владельцев.
            let node = unsafe { Box::from_raw(ptr.as_ptr()) };
            let NodeBox { value, next, .. } = *node;
            drop(value);
            let Some(next) = next else { return };
            // Ссылка `next` переходит к циклу: её release — следующая итерация.
            ptr = ManuallyDrop::new(next).ptr;
        }
    }

    /// С пулом — короткая проверка счётчика, а освобождение цепочки —
    /// отдельной функцией ([`release_chain`]): в одной функции с циклом LLVM
    /// выносил в её начало адрес кэша потока (вызов `_tlv_get_addr`) и
    /// сохранение регистров, и их платило каждое отпускание ссылки, а не
    /// только освобождение узла (в C это `release`, где цикл начинается с
    /// проверки).
    #[cfg(feature = "pool")]
    #[inline]
    fn drop(&mut self) {
        // SAFETY: `self` держит одну ссылку на узел `ptr`, он жив.
        if R::decrement(unsafe { &self.ptr.as_ref().refs }) {
            // SAFETY: ссылка была последней.
            unsafe { release_chain(self.ptr) };
        }
    }
}

/// Освободить узел `ptr`, чья последняя ссылка уже отпущена, и дальше по
/// цепочке, пока узлы умирают.
///
/// # Safety
/// Счётчик `ptr` дошёл до 0 (с атомарным — `decrement` синхронизировал нас с
/// прежними владельцами); узел размещён `R::Storage::place`.
#[cfg(feature = "pool")]
#[inline(never)]
unsafe fn release_chain<T, R: RefCount>(mut ptr: NonNull<NodeBox<T, R>>) {
    loop {
        // SAFETY: см. контракт; далее — `next`, забранный из освобождённого
        // узла, чей счётчик тоже дошёл до 0.
        let NodeBox { value, next, .. } = unsafe { R::Storage::take(ptr) };
        drop(value);
        let Some(next) = next else { return };
        // Ссылка `next` переходит к циклу.
        ptr = ManuallyDrop::new(next).ptr;
        // SAFETY: держим ссылку `next` на живой узел.
        if !R::decrement(unsafe { &ptr.as_ref().refs }) {
            return;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::refcount::{Atomic, Local};
    use std::rc::Rc;
    use std::sync::Arc;
    use std::thread;

    type NodePtr<T> = super::NodePtr<T, Local>;

    fn refs<T, R: RefCount>(p: &super::NodePtr<T, R>) -> u32 {
        R::get(&p.inner().refs)
    }

    #[test]
    fn node_layout_matches_c() {
        // C: `struct node { int value; int refcount; struct node *next; }`.
        assert_eq!(size_of::<NodeBox<i32, Local>>(), 16);
        assert_eq!(size_of::<NodeBox<i32, Atomic>>(), 16);
        assert_eq!(size_of::<Option<NodePtr<i32>>>(), 8);
    }

    #[test]
    fn clone_and_drop_track_refs() {
        let a = NodePtr::new(1, None);
        let b = a.clone();
        assert_eq!(refs(&a), 2);
        assert!(NodePtr::ptr_eq(&a, &b));
        drop(b);
        assert_eq!(refs(&a), 1);
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
        assert_eq!(refs(&shared), 1);

        drop(shared);
        assert_eq!(Rc::strong_count(&tracker), 1);
    }

    /// Ссылки на общий хвост берут и отпускают несколько потоков; каждый
    /// строит свою цепочку поверх него и разбирает её. Под Miri проверяет и
    /// отсутствие гонок данных.
    #[test]
    fn atomic_refs_across_threads() {
        type P = super::NodePtr<Arc<u32>, Atomic>;
        let tracker = Arc::new(7);
        let shared: P = super::NodePtr::new(Arc::clone(&tracker), None);
        let (threads, rounds) = if cfg!(miri) { (3, 20) } else { (8, 2000) };
        thread::scope(|s| {
            for _ in 0..threads {
                s.spawn(|| {
                    for _ in 0..rounds {
                        let mut head = Some(shared.clone());
                        for _ in 0..3 {
                            head = Some(super::NodePtr::new(Arc::clone(&tracker), head));
                        }
                        let mut node = head.unwrap();
                        while let Some(value) = node.advance_unless_last() {
                            assert_eq!(*value, 7);
                        }
                        assert!(super::NodePtr::ptr_eq(&node, &shared));
                    }
                });
            }
        });
        assert_eq!(refs(&shared), 1);
        drop(shared);
        assert_eq!(Arc::strong_count(&tracker), 1);
    }
}
