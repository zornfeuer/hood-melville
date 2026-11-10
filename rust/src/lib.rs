//! Функциональная (персистентная) очередь реального времени Hood–Melville
//! на шести стеках: `push`, `pop`, `front` за гарантированное O(1).

#![deny(unsafe_code)]

mod node;
mod queue;
mod rotation;
mod stack;

pub use queue::{Iter, Queue};
