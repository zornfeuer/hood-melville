//! Функциональная очередь Hood–Melville — аналог `fqueue` из `c/src/fqueue.c`.

use std::fmt;
use std::iter::FusedIterator;
use std::mem;

use crate::rotation::{Len, Rotation};

/// Шагов ротации на операцию. Одного достаточно — доказательство в
/// `docs/proof.md`: ротация завершается раньше, чем живой
/// фронт опустеет, а после неё `rear_len <= front_len`.
const ROTATION_STEPS: usize = 1;
use crate::stack::Stack;

/// Персистентная очередь Hood–Melville.
///
/// Операции не изменяют исходную очередь, а возвращают новую версию; версии
/// разделяют общие узлы. Клонирование — O(1).
///
/// # Сложность
///
/// - Worst-case O(1) — построение новой версии ([`push`](Self::push),
///   [`pop`](Self::pop)) и доступ ([`front`](Self::front), [`len`](Self::len),
///   [`is_empty`](Self::is_empty)): не более 3 новых узлов на `push` и 2 на
///   `pop`. Для обобщённого `T` к этому добавляется стоимость не более двух
///   `T::clone` на операцию.
/// - **Не** worst-case O(1) — уничтожение версии (`Drop`): освобождается
///   каскадом всё, чем версия владела монопольно, что может быть Θ(n) узлов.
///   Поэтому O(1) не гарантируется и для выражений, неявно удаляющих версию:
///   в `q = q.push(x)` сам `push` — O(1), а удаление старого `q` при
///   присваивании может стоить Θ(n). Суммарно освобождение не дороже
///   создания узлов, то есть амортизированно O(1) на операцию.
///
/// Доказательство — `docs/proof.md`.
///
/// ```
/// use hood_melville::Queue;
///
/// let q1 = Queue::new().push(1).push(2);
/// let q2 = q1.pop().unwrap();
///
/// assert_eq!(q1.front(), Some(&1)); // старая версия не изменилась
/// assert_eq!(q2.front(), Some(&2));
/// assert!(q2.pop().unwrap().pop().is_none());
/// ```
pub struct Queue<T> {
    // u32, а не usize: структура меньше (72 байта вместо 88). Предел —
    // u32::MAX элементов, переполнение проверяется в `push_mut`.
    len: Len,
    front_len: Len,
    rear_len: Len,
    /// Голова — первый элемент очереди; отсюда `pop`.
    front: Stack<T>,
    /// Голова — последний добавленный элемент; сюда `push`.
    rear: Stack<T>,
    rot: Rotation<T>,
}

impl<T> Queue<T> {
    /// Пустая очередь — аналог `fqueue_init`.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            len: 0,
            front_len: 0,
            rear_len: 0,
            front: Stack::new(),
            rear: Stack::new(),
            rot: Rotation::Idle,
        }
    }

    /// Число элементов — аналог `fqueue_size`.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.len as usize
    }

    /// Аналог `fqueue_is_empty`.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Первый элемент или `None` для пустой очереди — аналог `fqueue_front`.
    #[must_use]
    pub fn front(&self) -> Option<&T> {
        // Ротация всегда успевает подменить фронт до его исчерпания.
        debug_assert_eq!(self.front.is_empty(), self.is_empty());
        self.front.head()
    }
}

impl<T: Clone> Queue<T> {
    /// Новая очередь с `value` в конце — аналог `fqueue_push`.
    #[must_use]
    pub fn push(&self, value: T) -> Self {
        let mut q = self.clone();
        q.push_mut(value);
        q
    }

    /// Новая очередь без первого элемента или `None` для пустой — аналог
    /// `fqueue_pop`.
    #[must_use]
    pub fn pop(&self) -> Option<Self> {
        if self.is_empty() {
            return None;
        }
        let mut q = self.clone();
        q.pop_mut();
        Some(q)
    }

    /// Итератор по элементам от первого к последнему.
    ///
    /// Не заимствует очередь: работает над её O(1)-копией через `pop`, поэтому
    /// каждый шаг — O(1) в худшем случае, а элементы выдаются клонами.
    #[must_use]
    pub fn iter(&self) -> Iter<T> {
        Iter(self.clone())
    }

    // `push`/`pop` по месту. Наружу не видны: персистентность обеспечивают
    // публичные обёртки, а итератор и `Extend` работают со своей копией.

    fn push_mut(&mut self, value: T) {
        // Проверка до любых изменений: при панике (через `Extend` очередь
        // изменяется по месту) она остаётся согласованной.
        let len = self.len.checked_add(1).expect("очередь длиннее u32::MAX");
        self.rear.push(value);
        self.rear_len += 1;
        self.len = len;
        if matches!(self.rot, Rotation::Idle) {
            self.start_rotation_if_needed();
        }
        self.advance_rotation();
    }

    fn pop_mut(&mut self) {
        debug_assert!(!self.is_empty());
        self.front.advance();
        self.front_len -= 1;
        self.len -= 1;
        if matches!(self.rot, Rotation::Idle) {
            self.start_rotation_if_needed();
        } else if let Some(front) = self.rot.invalidate_mut() {
            self.install_front(front);
        }
        self.advance_rotation();
    }

    /// Аналог `start_rotation`: запускается, как только `|rear| > |front|`.
    /// Вызывается только при отсутствии текущей ротации.
    fn start_rotation_if_needed(&mut self) {
        if self.rear_len <= self.front_len {
            return;
        }
        self.rear_len = 0;
        let rear = mem::take(&mut self.rear);
        self.rot = Rotation::start(self.front.clone(), self.front_len, rear);
    }

    /// `ROTATION_STEPS` шагов ротации на операцию.
    fn advance_rotation(&mut self) {
        for _ in 0..ROTATION_STEPS {
            if let Some(front) = self.rot.step_mut() {
                self.install_front(front);
            }
        }
    }

    /// Установить новый фронт по завершении ротации — аналог `finish_rotation`.
    fn install_front(&mut self, front: Stack<T>) {
        self.front = front;
        self.front_len = self.len - self.rear_len;
    }
}

// Вручную: derive потребовал бы `T: Clone`/`T: Default`.
// `clone` — аналог `fqueue_clone`: O(1), все узлы общие.
impl<T> Clone for Queue<T> {
    fn clone(&self) -> Self {
        Self {
            len: self.len,
            front_len: self.front_len,
            rear_len: self.rear_len,
            front: self.front.clone(),
            rear: self.rear.clone(),
            rot: self.rot.clone(),
        }
    }
}

impl<T> Default for Queue<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Clone + fmt::Debug> fmt::Debug for Queue<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self).finish()
    }
}

/// Равенство по содержимому: внутреннее устройство (фаза ротации, раскладка
/// по стекам) у равных очередей может различаться.
impl<T: Clone + PartialEq> PartialEq for Queue<T> {
    fn eq(&self, other: &Self) -> bool {
        self.len == other.len && self.iter().eq(other.iter())
    }
}

impl<T: Clone + Eq> Eq for Queue<T> {}

impl<T: Clone> FromIterator<T> for Queue<T> {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        let mut q = Self::new();
        q.extend(iter);
        q
    }
}

/// Добавляет элементы в эту версию; другие версии, разделяющие с ней узлы,
/// не меняются.
impl<T: Clone> Extend<T> for Queue<T> {
    fn extend<I: IntoIterator<Item = T>>(&mut self, iter: I) {
        for value in iter {
            self.push_mut(value);
        }
    }
}

/// Итератор по элементам очереди — см. [`Queue::iter`].
pub struct Iter<T>(Queue<T>);

impl<T: Clone> Iterator for Iter<T> {
    type Item = T;

    fn next(&mut self) -> Option<T> {
        let value = self.0.front()?.clone();
        self.0.pop_mut();
        Some(value)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.0.len(), Some(self.0.len()))
    }
}

impl<T: Clone> ExactSizeIterator for Iter<T> {}

impl<T: Clone> FusedIterator for Iter<T> {}

impl<T> Clone for Iter<T> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<T: Clone> IntoIterator for Queue<T> {
    type Item = T;
    type IntoIter = Iter<T>;

    fn into_iter(self) -> Iter<T> {
        Iter(self)
    }
}

impl<T: Clone> IntoIterator for &Queue<T> {
    type Item = T;
    type IntoIter = Iter<T>;

    fn into_iter(self) -> Iter<T> {
        self.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    impl<T> Queue<T> {
        /// Структурные инварианты; O(n), только для тестов.
        fn check_invariants(&self) {
            assert_eq!(self.front.iter().count(), self.front_len as usize);
            assert_eq!(self.rear.iter().count(), self.rear_len as usize);
            assert_eq!(self.front.is_empty(), self.is_empty());
            match &self.rot {
                Rotation::Idle => {
                    assert_eq!(self.len, self.front_len + self.rear_len);
                    assert!(self.rear_len <= self.front_len);
                }
                Rotation::Reversing { front, rear, .. } => {
                    assert_eq!(rear.iter().count(), front.iter().count() + 1);
                }
                Rotation::Appending { ok, rfront, .. } => {
                    assert!((1..=rfront.iter().count()).contains(&(*ok as usize)));
                }
            }
        }
    }

    fn contents<T: Clone>(q: &Queue<T>) -> Vec<T> {
        q.iter().collect()
    }

    #[test]
    fn new_queue_is_empty() {
        let q: Queue<i32> = Queue::new();
        assert!(q.is_empty());
        assert_eq!(q.len(), 0);
        assert_eq!(q.front(), None);
        assert!(q.pop().is_none());
        assert_eq!(contents(&q), []);
    }

    #[test]
    fn push_and_pop_leave_original_untouched() {
        let q0 = Queue::new();
        let q1 = q0.push(42);
        let q2 = q1.pop().unwrap();

        assert!(q0.is_empty());
        assert_eq!((q1.len(), q1.front()), (1, Some(&42)));
        assert!(q2.is_empty());
        assert_eq!(q2.front(), None);
    }

    #[test]
    fn fifo_order() {
        let q: Queue<_> = (1..=5).collect();
        assert_eq!(q.len(), 5);
        assert_eq!(contents(&q), [1, 2, 3, 4, 5]);
    }

    #[test]
    fn pops_exhausting_old_front_mid_rotation() {
        // Третий push запускает ротацию при front = [1]; первый pop
        // опустошает старый фронт — новый должен быть уже готов.
        let mut q = Queue::new().push(1).push(2).push(3);
        for expected in 1..=3 {
            q.check_invariants();
            assert_eq!(q.front(), Some(&expected));
            q = q.pop().unwrap();
        }
        assert!(q.is_empty());
    }

    #[test]
    fn branches_from_same_base_are_independent() {
        let base = Queue::new().push(1).push(2);
        let a = base.push(3);
        let b = base.push(99);
        assert_eq!(contents(&a), [1, 2, 3]);
        assert_eq!(contents(&b), [1, 2, 99]);
        assert_eq!(contents(&base), [1, 2]);
    }

    /// Псевдослучайные операции над случайно выбранными старыми версиями
    /// (ветвление) против эталонной `VecDeque`. Инварианты проверяются на
    /// каждой версии, а в конце все версии сверяются заново.
    #[test]
    fn random_branching_matches_model() {
        let mut seed: u64 = 0x2545_F491_4F6C_DD1D;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };

        let mut versions = vec![(Queue::new(), VecDeque::new())];
        let ops = if cfg!(miri) { 300 } else { 5_000_u64 };
        for i in 0..ops {
            // Чаще берём свежие версии, чтобы очереди успевали вырасти.
            let back = usize::try_from(next() % 4).unwrap();
            let idx = versions.len().saturating_sub(1 + back);
            let (q, model) = &versions[idx];

            let derived = if next() % 3 == 0 {
                q.pop().map(|q| {
                    let mut m = model.clone();
                    m.pop_front();
                    (q, m)
                })
            } else {
                let mut m = model.clone();
                m.push_back(i);
                Some((q.push(i), m))
            };
            if let Some((q, m)) = derived {
                q.check_invariants();
                assert_eq!(q.len(), m.len());
                assert_eq!(q.front(), m.front());
                versions.push((q, m));
            }
        }

        assert!(
            versions
                .iter()
                .any(|(q, _)| q.len() > if cfg!(miri) { 10 } else { 100 })
        );
        for (q, m) in &versions {
            assert!(q.iter().eq(m.iter().copied()));
        }
    }

    /// Все истории push/pop длиной до `DEPTH` (≈10⁵ состояний): инварианты
    /// и голова очереди проверяются в каждом достижимом состоянии.
    #[test]
    fn exhaustive_short_histories() {
        // Под Miri — в тысячи раз медленнее — хватает коротких историй.
        const DEPTH: u32 = if cfg!(miri) { 8 } else { 18 };

        fn go(q: &Queue<u32>, model: &mut VecDeque<u32>, depth: u32) -> u64 {
            q.check_invariants();
            assert_eq!(q.front(), model.front());
            if depth == 0 {
                return 1;
            }
            model.push_back(depth);
            let mut visited = 1 + go(&q.push(depth), model, depth - 1);
            model.pop_back();
            if let Some(popped) = q.pop() {
                let x = model.pop_front().unwrap();
                visited += go(&popped, model, depth - 1);
                model.push_front(x);
            }
            visited
        }

        let visited = go(&Queue::new(), &mut VecDeque::new(), DEPTH);
        assert!(cfg!(miri) || visited > 90_000);
    }

    /// При одних push'ах ротация стартует, когда размер становится 2^k − 1.
    /// На этом построен бенчмарк «push, запускающий ротацию».
    #[test]
    #[cfg_attr(miri, ignore = "проверка логики фаз, память не затрагивает")]
    fn pure_pushes_start_rotation_at_powers_of_two_minus_one() {
        let mut q = Queue::new();
        for i in 0..5_000 {
            let next = q.push(i);
            // Старт ротации забирает `rear` целиком; иначе push его удлиняет.
            let started = next.rear_len == 0;
            assert_eq!(
                started,
                (next.len() + 1).is_power_of_two(),
                "len = {}",
                next.len()
            );
            q = next;
        }
    }

    /// Фаза после каждого push при построении одними push'ами — по формуле
    /// из модели (`docs/proof.md`): ротация стартует на
    /// размере `2^k − 1` (`m = 2^(k−1) − 1`), `Reversing` занимает `m + 1`
    /// шагов, вся ротация — `2m + 1`, по `ROTATION_STEPS` шагов на push. На
    /// этих окнах построены фазы бенчмарка `benches/queue.rs`.
    #[test]
    #[cfg_attr(miri, ignore = "проверка логики фаз, память не затрагивает")]
    fn pure_pushes_rotation_phases() {
        let mut q = Queue::new();
        for n in 1..=1_usize << 13 {
            q = q.push(n);
            let start = (1_usize << (n + 1).ilog2()) - 1; // последний 2^k − 1 ≤ n
            let m = start / 2;
            let steps = ROTATION_STEPS * (n - start + 1);
            let expected = if steps > 2 * m {
                "Idle"
            } else if steps > m {
                "Appending"
            } else {
                "Reversing"
            };
            let actual = match q.rot {
                Rotation::Idle => "Idle",
                Rotation::Reversing { .. } => "Reversing",
                Rotation::Appending { .. } => "Appending",
            };
            assert_eq!(actual, expected, "n = {n}");
        }
    }

    #[test]
    fn long_queue_drops_without_overflow() {
        let n = if cfg!(miri) { 10_000 } else { 1_000_000 };
        let q: Queue<_> = (0..n).collect();
        assert_eq!(q.len(), n);
        drop(q);
    }

    #[test]
    fn traits() {
        let q: Queue<_> = (1..=3).collect();

        assert_eq!(format!("{q:?}"), "[1, 2, 3]");

        // Одинаковое содержимое при разном внутреннем устройстве.
        let other = Queue::new().push(0).push(1).push(2).pop().unwrap().push(3);
        assert_eq!(q, other);
        assert_ne!(q, q.push(4));

        let mut extended = q.clone();
        extended.extend([4, 5]);
        assert_eq!(contents(&extended), [1, 2, 3, 4, 5]);
        assert_eq!(contents(&q), [1, 2, 3]);

        let it = q.iter();
        assert_eq!(it.len(), 3);
        assert_eq!((&q).into_iter().sum::<i32>(), 6);
        assert_eq!(q.into_iter().collect::<Vec<_>>(), [1, 2, 3]);

        assert_eq!(Queue::<i32>::default(), Queue::new());
    }

    #[test]
    fn clone_does_not_require_clone_elements() {
        struct NotClone;
        let q: Queue<NotClone> = Queue::new();
        let _copy = q.clone();
    }
}
