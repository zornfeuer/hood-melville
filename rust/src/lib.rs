//! Функциональная (персистентная) очередь реального времени Hood–Melville
//! на шести стеках: `push`, `pop`, `front` за гарантированное O(1).

#![deny(unsafe_code)]

mod node;
#[cfg(feature = "pool")]
pub mod pool;
mod queue;
pub mod refcount;
mod rotation;
mod stack;
mod storage;

pub use queue::{Iter, Queue, SyncQueue};
#[cfg(feature = "pool")]
pub use queue::{PooledQueue, SyncPooledQueue};
