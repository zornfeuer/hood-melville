//! Где живут узлы очереди: по `Box` на узел ([`Heap`]) или в пуле
//! ([`Pool`](crate::pool::Pool), фича `pool`, `docs/pool.md`).

#![allow(unsafe_code)]
#![warn(clippy::undocumented_unsafe_blocks)]

use std::ptr::NonNull;

use crate::refcount::sealed;

/// Где живут узлы: глобальный аллокатор ([`Heap`]) или пул ([`Pool`]).
pub trait NodeStorage: sealed::Sealed + 'static {
    /// Разместить узел.
    #[doc(hidden)]
    fn place<N>(node: N) -> NonNull<N>;

    /// Забрать узел обратно и освободить его память.
    ///
    /// # Safety
    /// `ptr` получен из [`place`](Self::place) того же хранилища и забирается
    /// ровно один раз.
    #[doc(hidden)]
    unsafe fn take<N>(ptr: NonNull<N>) -> N;
}

/// Узел — отдельный `Box` у глобального аллокатора.
pub enum Heap {}

impl sealed::Sealed for Heap {}

impl NodeStorage for Heap {
    #[inline]
    fn place<N>(node: N) -> NonNull<N> {
        NonNull::from(Box::leak(Box::new(node)))
    }

    #[inline]
    unsafe fn take<N>(ptr: NonNull<N>) -> N {
        // SAFETY: `ptr` из `Box::leak` в `place` и забирается один раз.
        *unsafe { Box::from_raw(ptr.as_ptr()) }
    }
}
