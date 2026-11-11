//! Персистентный односвязный стек со структурным разделением хвостов.
//!
//! Тонкая обёртка над [`NodePtr`]: клонирование стека — retain головы,
//! удаление — release с итеративным освобождением цепочки.

use crate::node::NodePtr;
use crate::refcount::RefCount;

/// Неизменяемый стек. Разные версии разделяют общие хвосты.
pub(crate) struct Stack<T, R: RefCount>(Option<NodePtr<T, R>>);

impl<T, R: RefCount> Stack<T, R> {
    /// Пустой стек.
    pub(crate) const fn new() -> Self {
        Self(None)
    }

    pub(crate) const fn is_empty(&self) -> bool {
        self.0.is_none()
    }

    /// Новый стек с `value` на вершине поверх `next`.
    ///
    /// `next` передаётся по значению: если вызывающему он больше не нужен,
    /// ссылка просто переезжает в новый узел без изменения счётчика.
    #[must_use]
    #[inline]
    pub(crate) fn cons(value: T, next: Self) -> Self {
        Self(Some(NodePtr::new(value, next.0)))
    }

    /// Вершина стека.
    #[inline]
    pub(crate) fn head(&self) -> Option<&T> {
        self.0.as_ref().map(NodePtr::value)
    }

    /// Снимает вершину, не возвращая значение, — аналог `node_advance`.
    ///
    /// Уникальный узел отдаёт свой хвост без изменения счётчика; разделяемый
    /// остаётся другим версиям, а мы берём ещё одну ссылку на его хвост.
    #[inline]
    pub(crate) fn advance(&mut self) {
        if let Some(node) = self.0.take() {
            self.0 = match node.into_parts() {
                Ok((_, next)) => {
                    if let Some(n) = &next {
                        n.touch();
                    }
                    next
                }
                Err(shared) => shared.next().cloned(),
            };
        }
    }

    /// `self = cons(value, self)` — аналог `cons_prepend`.
    #[inline]
    pub(crate) fn push(&mut self, value: T) {
        let next = std::mem::take(self);
        *self = Self::cons(value, next);
    }

    /// Снимает вершину и возвращает её значение — аналог чтения `->value`
    /// с последующим `node_advance`.
    ///
    /// Если узлом больше никто не владеет, значение и хвост забираются из него
    /// без копирования; иначе значение клонируется, а узел остаётся другим
    /// версиям нетронутым.
    #[inline]
    pub(crate) fn pop(&mut self) -> Option<T>
    where
        T: Clone,
    {
        let node = self.0.take()?;
        let (value, next) = match node.into_parts() {
            Ok(parts) => {
                if let Some(n) = &parts.1 {
                    n.touch();
                }
                parts
            }
            Err(shared) => (shared.value().clone(), shared.next().cloned()),
        };
        self.0 = next;
        Some(value)
    }

    /// Итератор по элементам от вершины ко дну.
    #[cfg(test)]
    pub(crate) fn iter(&self) -> Iter<'_, T, R> {
        Iter(self.0.as_ref())
    }
}

/// Непустой стек: вершина есть всегда. Указатель на неё ненулевой, и
/// `Rotation` кодирует свой второй вариант нулём на его месте (niche) — без
/// отдельного тега.
pub(crate) struct NonEmptyStack<T, R: RefCount>(NodePtr<T, R>);

impl<T, R: RefCount> NonEmptyStack<T, R> {
    #[inline]
    pub(crate) fn new(stack: Stack<T, R>) -> Option<Self> {
        stack.0.map(Self)
    }

    /// Снимает вершину и возвращает её значение, если под ней есть ещё
    /// элементы; иначе `None`, стек не меняется (см. [`Self::into_last`]).
    #[inline]
    pub(crate) fn pop_unless_last(&mut self) -> Option<T>
    where
        T: Clone,
    {
        self.0.advance_unless_last()
    }

    /// Значение вершины стека из одного элемента; стек расходуется.
    #[inline]
    pub(crate) fn into_last(self) -> T
    where
        T: Clone,
    {
        debug_assert!(self.0.next().is_none());
        match self.0.into_parts() {
            Ok((value, _)) => value,
            Err(shared) => shared.value().clone(),
        }
    }

    #[cfg(test)]
    pub(crate) fn iter(&self) -> Iter<'_, T, R> {
        Iter(Some(&self.0))
    }
}

impl<T, R: RefCount> Clone for NonEmptyStack<T, R> {
    #[inline]
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

// Ручные реализации вместо derive: derive потребовал бы `T: Clone` / `T: Default`,
// хотя клонируется только указатель на узел.
impl<T, R: RefCount> Clone for Stack<T, R> {
    #[inline]
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<T, R: RefCount> Default for Stack<T, R> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
pub(crate) struct Iter<'a, T, R: RefCount>(Option<&'a NodePtr<T, R>>);

#[cfg(test)]
impl<'a, T, R: RefCount> Iterator for Iter<'a, T, R> {
    type Item = &'a T;

    fn next(&mut self) -> Option<Self::Item> {
        let node = self.0?;
        self.0 = node.next();
        Some(node.value())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::refcount::Local;

    type Stack<T> = super::Stack<T, Local>;
    use std::cell::Cell;

    fn from_slice<T: Clone>(items: &[T]) -> Stack<T> {
        // Последний элемент среза оказывается на вершине.
        items
            .iter()
            .cloned()
            .fold(Stack::new(), |s, x| Stack::cons(x, s))
    }

    fn tail<T>(s: &Stack<T>) -> Stack<T> {
        let mut t = s.clone();
        t.advance();
        t
    }

    fn to_vec<T: Clone>(s: &Stack<T>) -> Vec<T> {
        s.iter().cloned().collect()
    }

    fn shares_node<T>(a: &Stack<T>, b: &Stack<T>) -> bool {
        match (&a.0, &b.0) {
            (Some(x), Some(y)) => NodePtr::ptr_eq(x, y),
            _ => false,
        }
    }

    #[test]
    fn empty_stack() {
        let s: Stack<i32> = Stack::new();
        assert!(s.is_empty());
        assert_eq!(s.head(), None);
        let mut t = s.clone();
        t.advance();
        assert!(t.is_empty());
        assert_eq!(s.iter().next(), None);
    }

    #[test]
    fn cons_head_tail() {
        let s = from_slice(&[1, 2, 3]);
        assert_eq!(s.head(), Some(&3));
        assert_eq!(to_vec(&tail(&s)), [2, 1]);
        assert_eq!(to_vec(&s), [3, 2, 1]);
    }

    #[test]
    fn versions_share_tail() {
        let base = from_slice(&[1, 2]);
        let a = Stack::cons(10, base.clone());
        let b = Stack::cons(20, base.clone());

        assert_eq!(to_vec(&a), [10, 2, 1]);
        assert_eq!(to_vec(&b), [20, 2, 1]);
        assert_eq!(to_vec(&base), [2, 1]);
        assert!(shares_node(&tail(&a), &base));
        assert!(shares_node(&tail(&b), &base));
    }

    #[test]
    fn push_and_pop() {
        let mut s = Stack::new();
        s.push(1);
        s.push(2);
        assert_eq!(s.pop(), Some(2));
        assert_eq!(s.pop(), Some(1));
        assert_eq!(s.pop(), None);
        assert!(s.is_empty());
    }

    #[test]
    fn pop_does_not_affect_other_versions() {
        let original = from_slice(&[1, 2, 3]);
        let mut walker = original.clone();
        assert_eq!(walker.pop(), Some(3));
        assert_eq!(walker.pop(), Some(2));
        assert_eq!(to_vec(&walker), [1]);
        assert_eq!(to_vec(&original), [3, 2, 1]);
    }

    #[test]
    fn pop_moves_value_out_of_unique_node() {
        // Значение без Clone-копий: уникальный узел отдаёт его по значению.
        #[derive(Debug, PartialEq)]
        struct Counted<'a>(i32, &'a Cell<u32>);
        impl Clone for Counted<'_> {
            fn clone(&self) -> Self {
                self.1.set(self.1.get() + 1);
                Self(self.0, self.1)
            }
        }

        let clones = Cell::new(0);
        let mut s = Stack::cons(Counted(1, &clones), Stack::new());
        assert_eq!(s.pop().map(|c| c.0), Some(1));
        assert_eq!(clones.get(), 0);

        let mut s = Stack::cons(Counted(2, &clones), Stack::new());
        let keep = s.clone();
        assert_eq!(s.pop().map(|c| c.0), Some(2));
        assert_eq!(clones.get(), 1);
        assert_eq!(keep.head().map(|c| c.0), Some(2));
    }

    #[test]
    fn dropping_long_stack_does_not_overflow() {
        let mut s = Stack::new();
        for i in 0..if cfg!(miri) { 10_000 } else { 1_000_000 } {
            s.push(i);
        }
        drop(s);
    }

    #[test]
    fn drop_frees_every_node_exactly_once_and_keeps_shared_tail() {
        struct Tracked<'a>(&'a Cell<u32>);
        impl Drop for Tracked<'_> {
            fn drop(&mut self) {
                self.0.set(self.0.get() + 1);
            }
        }

        let dropped = Cell::new(0);
        let mut shared = Stack::new();
        for _ in 0..3 {
            shared.push(Tracked(&dropped));
        }
        let mut a = shared.clone();
        a.push(Tracked(&dropped));
        a.push(Tracked(&dropped));

        // Освобождаются только два собственных узла `a`, общий хвост жив.
        drop(a);
        assert_eq!(dropped.get(), 2);
        assert_eq!(shared.iter().count(), 3);

        drop(shared);
        assert_eq!(dropped.get(), 5);
    }
}
