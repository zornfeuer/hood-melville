//! Функциональная очередь Hood–Melville — аналог `fqueue` из `c/include/fqueue_template.h`.

use std::fmt;
use std::iter::FusedIterator;
use std::mem;

#[cfg(feature = "pool")]
use crate::pool::Pooled;
use crate::refcount::{Atomic, Local, RefCount};
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
pub struct Queue<T, R: RefCount = Local> {
    // u32, а не usize, и `ok` здесь, а не в `Rotation`: структура — 64
    // байта, одна строка кэша (с usize — 88, с `ok` в ротации — 72). Предел —
    // u32::MAX элементов, переполнение проверяется в `push_mut`.
    len: Len,
    front_len: Len,
    rear_len: Len,
    /// Счётчик `ok` текущей ротации (см. `Rotation`); вне ротации — 0.
    ok: Len,
    /// Голова — первый элемент очереди; отсюда `pop`.
    front: Stack<T, R>,
    /// Голова — последний добавленный элемент; сюда `push`.
    rear: Stack<T, R>,
    rot: Rotation<T, R>,
}

/// Потокобезопасная персистентная очередь: та же [`Queue`] с атомарным
/// счётчиком ссылок в узлах (как `Arc` против `Rc`).
///
/// `SyncQueue<T>` — `Send + Sync` при `T: Send + Sync`: версию можно передать
/// другому потоку или разделять между потоками, а потоки — параллельно
/// порождать из общих версий новые (`push`, `pop`, `clone`) и читать их.
/// Изменение на месте (`push_mut`, `pop_mut`) — как и любое `&mut`, только у
/// владельца этой версии; её узлы при этом могут разделяться с версиями других
/// потоков. Цена — атомарные операции над счётчиками: в одном потоке
/// `SyncQueue` медленнее `Queue`.
///
/// ```
/// use hood_melville::SyncQueue;
/// use std::thread;
///
/// let base: SyncQueue<i32> = (1..=3).collect();
/// thread::scope(|s| {
///     for i in 0..4 {
///         let base = &base;
///         s.spawn(move || {
///             let q = base.push(10 + i).pop().unwrap();
///             assert_eq!(q.iter().collect::<Vec<_>>(), [2, 3, 10 + i]);
///         });
///     }
/// });
/// assert_eq!(base.len(), 3);
/// ```
///
/// Однопоточную [`Queue`] передать в другой поток нельзя:
///
/// ```compile_fail
/// let q = hood_melville::Queue::new().push(1);
/// std::thread::spawn(move || q.len());
/// ```
pub type SyncQueue<T> = Queue<T, Atomic>;

/// [`Queue`] с узлами из пула (`docs/pool.md`): тот же API и семантика,
/// узлы берутся из пула (плиты по 65 536 байт, кэш у каждого потока) и
/// возвращаются туда при обнулении счётчика. Пул держит память на пике
/// живого множества до [`pool::trim`](crate::pool::trim).
#[cfg(feature = "pool")]
pub type PooledQueue<T> = Queue<T, Pooled<Local>>;

/// [`SyncQueue`] с узлами из пула.
#[cfg(feature = "pool")]
pub type SyncPooledQueue<T> = Queue<T, Pooled<Atomic>>;

#[cfg(feature = "pool")]
impl<T> PooledQueue<T> {
    /// Пустая очередь с пулом узлов.
    #[must_use]
    pub const fn new_pooled() -> Self {
        Self::empty()
    }
}

#[cfg(feature = "pool")]
impl<T> SyncPooledQueue<T> {
    /// Пустая потокобезопасная очередь с пулом узлов.
    #[must_use]
    pub const fn new_sync_pooled() -> Self {
        Self::empty()
    }
}

impl<T> Queue<T> {
    /// Пустая очередь — аналог `fqueue_init`.
    #[must_use]
    pub const fn new() -> Self {
        Self::empty()
    }
}

impl<T> SyncQueue<T> {
    /// Пустая потокобезопасная очередь.
    ///
    /// Отдельное имя, как `new_sync` в `rpds`: с одним `new` на оба варианта
    /// `Queue::new()` без аннотации типа перестал бы выводиться.
    #[must_use]
    pub const fn new_sync() -> Self {
        Self::empty()
    }
}

impl<T, R: RefCount> Queue<T, R> {
    const fn empty() -> Self {
        Self {
            len: 0,
            front_len: 0,
            rear_len: 0,
            ok: 0,
            front: Stack::new(),
            rear: Stack::new(),
            rot: Rotation::idle(),
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

impl<T: Clone, R: RefCount> Queue<T, R> {
    /// Новая очередь с `value` в конце — аналог `fqueue_push`.
    #[must_use]
    #[inline]
    pub fn push(&self, value: T) -> Self {
        let mut q = self.clone();
        q.push_mut(value);
        q
    }

    /// Новая очередь без первого элемента или `None` для пустой — аналог
    /// `fqueue_pop`.
    #[must_use]
    #[inline]
    pub fn pop(&self) -> Option<Self> {
        if self.is_empty() {
            return None;
        }
        let mut q = self.clone();
        q.pop_front_mut();
        Some(q)
    }

    /// Итератор по элементам от первого к последнему.
    ///
    /// Не заимствует очередь: работает над её O(1)-копией через `pop`, поэтому
    /// каждый шаг — O(1) в худшем случае, а элементы выдаются клонами.
    #[must_use]
    pub fn iter(&self) -> Iter<T, R> {
        Iter(self.clone())
    }

    /// Добавляет `value` в конец самой этой версии — аналог
    /// `fqueue_push_in_place`. То же, что `*self = self.push(value)`, но без
    /// копирования версии и освобождения старой; версии, с которыми `self`
    /// делит узлы, не меняются. Сложность — как у этого присваивания (см.
    /// [`Queue`]): сама операция O(1), освобождение узлов, которыми версия
    /// владела одна, может стоить больше.
    ///
    /// ```
    /// use hood_melville::Queue;
    ///
    /// let mut q = Queue::new();
    /// q.push_mut(1);
    /// let old = q.clone();
    /// q.push_mut(2);
    /// assert_eq!(q.len(), 2);
    /// assert_eq!(old.len(), 1);
    /// ```
    ///
    /// # Panics
    ///
    /// Если в очереди уже `u32::MAX` элементов; `self` тогда не меняется.
    /// Паника `T::clone` (значения из узлов, общих с другими версиями,
    /// клонируются при шаге ротации) оставляет `self` безопасным для
    /// использования и удаления, но с неопределённым содержимым.
    #[inline]
    pub fn push_mut(&mut self, value: T) {
        // Проверка до любых изменений: при панике очередь не меняется.
        let len = self.len.checked_add(1).expect("очередь длиннее u32::MAX");
        self.rear.push(value);
        self.rear_len += 1;
        self.len = len;
        if self.rot.is_idle() {
            self.start_rotation_if_needed();
        }
        self.advance_rotation();
    }

    /// Снимает первый элемент с самой этой версии и возвращает его или `None`
    /// для пустой — аналог `fqueue_pop_in_place`; см. [`push_mut`](Self::push_mut).
    /// Если узлом первого элемента владеет только `self`, значение забирается
    /// без клонирования.
    ///
    /// ```
    /// use hood_melville::Queue;
    ///
    /// let mut q: Queue<_> = (1..=3).collect();
    /// assert_eq!(q.pop_mut(), Some(1));
    /// assert_eq!(q.front(), Some(&2));
    /// ```
    ///
    /// # Panics
    ///
    /// Как у [`push_mut`](Self::push_mut): только при панике `T::clone`.
    #[inline]
    pub fn pop_mut(&mut self) -> Option<T> {
        self.pop_front_with(Stack::pop)
    }

    /// `pop` по месту без значения — для персистентного `pop`.
    #[inline]
    fn pop_front_mut(&mut self) {
        debug_assert!(!self.is_empty());
        self.pop_front_with(|front| {
            front.advance();
            Some(())
        });
    }

    /// `pop` по месту: `take` снимает вершину фронта (`None` — очередь пуста,
    /// ничего не меняется), затем счётчики и шаг ротации.
    ///
    /// Обобщённая, а не общая функция: каждый вызывающий получает свою копию.
    /// С одной общей функцией для `pop_front_mut` и `pop_mut` LLVM иначе
    /// встраивал персистентный `pop` — лишняя копия `Option<Queue>`, +9
    /// инструкций на операцию.
    #[inline]
    fn pop_front_with<V>(&mut self, take: impl FnOnce(&mut Stack<T, R>) -> Option<V>) -> Option<V> {
        let taken = take(&mut self.front)?;
        self.front_len -= 1;
        self.len -= 1;
        if self.rot.is_idle() {
            self.start_rotation_if_needed();
        } else if let Some(front) = self.rot.invalidate_mut(&mut self.ok) {
            self.install_front(front);
        }
        self.advance_rotation();
        Some(taken)
    }

    /// Аналог `start_rotation`: запускается, как только `|rear| > |front|`.
    /// Вызывается только при отсутствии текущей ротации.
    #[inline]
    fn start_rotation_if_needed(&mut self) {
        if self.rear_len <= self.front_len {
            return;
        }
        self.rear_len = 0;
        let rear = mem::take(&mut self.rear);
        // Лемма 1 в `docs/proof.md`: старт при `len = 2 * front_len + 1`,
        // отсюда граница `ok` (лемма 4).
        debug_assert_eq!(u64::from(self.len), 2 * u64::from(self.front_len) + 1);
        self.ok = self.front_len;
        self.rot = Rotation::start(self.front.clone(), rear);
    }

    /// `ROTATION_STEPS` шагов ротации на операцию.
    #[inline]
    fn advance_rotation(&mut self) {
        for _ in 0..ROTATION_STEPS {
            if let Some(front) = self.rot.step_mut(&mut self.ok) {
                self.install_front(front);
            }
        }
    }

    /// Установить новый фронт по завершении ротации — аналог `finish_rotation`.
    #[inline]
    fn install_front(&mut self, front: Stack<T, R>) {
        self.front = front;
        self.front_len = self.len - self.rear_len;
    }
}

// Вручную: derive потребовал бы `T: Clone`/`T: Default`.
// `clone` — аналог `fqueue_clone`: O(1), все узлы общие.
impl<T, R: RefCount> Clone for Queue<T, R> {
    #[inline]
    fn clone(&self) -> Self {
        Self {
            len: self.len,
            front_len: self.front_len,
            rear_len: self.rear_len,
            ok: self.ok,
            front: self.front.clone(),
            rear: self.rear.clone(),
            rot: self.rot.clone(),
        }
    }
}

impl<T, R: RefCount> Default for Queue<T, R> {
    fn default() -> Self {
        Self::empty()
    }
}

impl<T: Clone + fmt::Debug, R: RefCount> fmt::Debug for Queue<T, R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self).finish()
    }
}

/// Равенство по содержимому: внутреннее устройство (фаза ротации, раскладка
/// по стекам) у равных очередей может различаться.
impl<T: Clone + PartialEq, R: RefCount> PartialEq for Queue<T, R> {
    fn eq(&self, other: &Self) -> bool {
        self.len == other.len && self.iter().eq(other.iter())
    }
}

impl<T: Clone + Eq, R: RefCount> Eq for Queue<T, R> {}

impl<T: Clone, R: RefCount> FromIterator<T> for Queue<T, R> {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        let mut q = Self::empty();
        q.extend(iter);
        q
    }
}

/// Добавляет элементы в эту версию; другие версии, разделяющие с ней узлы,
/// не меняются.
impl<T: Clone, R: RefCount> Extend<T> for Queue<T, R> {
    fn extend<I: IntoIterator<Item = T>>(&mut self, iter: I) {
        for value in iter {
            self.push_mut(value);
        }
    }
}

/// Итератор по элементам очереди — см. [`Queue::iter`].
pub struct Iter<T, R: RefCount = Local>(Queue<T, R>);

impl<T: Clone, R: RefCount> Iterator for Iter<T, R> {
    type Item = T;

    fn next(&mut self) -> Option<T> {
        self.0.pop_mut()
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (self.0.len(), Some(self.0.len()))
    }
}

impl<T: Clone, R: RefCount> ExactSizeIterator for Iter<T, R> {}

impl<T: Clone, R: RefCount> FusedIterator for Iter<T, R> {}

impl<T, R: RefCount> Clone for Iter<T, R> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<T: Clone, R: RefCount> IntoIterator for Queue<T, R> {
    type Item = T;
    type IntoIter = Iter<T, R>;

    fn into_iter(self) -> Iter<T, R> {
        Iter(self)
    }
}

impl<T: Clone, R: RefCount> IntoIterator for &Queue<T, R> {
    type Item = T;
    type IntoIter = Iter<T, R>;

    fn into_iter(self) -> Iter<T, R> {
        self.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    impl<T, R: RefCount> Queue<T, R> {
        /// Структурные инварианты; O(n), только для тестов.
        fn check_invariants(&self) {
            assert_eq!(self.front.iter().count(), self.front_len as usize);
            assert_eq!(self.rear.iter().count(), self.rear_len as usize);
            assert_eq!(self.front.is_empty(), self.is_empty());
            match &self.rot {
                rot if rot.is_idle() => {
                    assert_eq!(self.len, self.front_len + self.rear_len);
                    assert!(self.rear_len <= self.front_len);
                    assert!(matches!(rot, Rotation::Appending { acc, .. } if acc.is_empty()));
                }
                Rotation::Reversing { front, rear, .. } => {
                    assert_eq!(rear.iter().count(), front.iter().count() + 1);
                }
                Rotation::Appending { rfront, .. } => {
                    assert!((1..=rfront.iter().count()).contains(&(self.ok as usize)));
                }
            }
        }
    }

    fn contents<T: Clone, R: RefCount>(q: &Queue<T, R>) -> Vec<T> {
        q.iter().collect()
    }

    #[test]
    fn queue_fits_a_cache_line() {
        assert_eq!(size_of::<Queue<i32>>(), 64);
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

    /// `push_mut`/`pop_mut` над случайными версиями истории, которые делят
    /// узлы с остальными: результат совпадает с моделью, прочие версии не
    /// меняются. Через версии, ставшие единственными владельцами своих
    /// узлов, проходит и путь без клонирования.
    #[test]
    fn in_place_operations_match_model_and_keep_other_versions() {
        let mut seed: u64 = 0x9E37_79B9_7F4A_7C15;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };

        let mut versions = vec![(Queue::new(), VecDeque::new())];
        let ops = if cfg!(miri) { 300 } else { 5_000_u64 };
        for i in 0..ops {
            let r = next();
            let idx = usize::try_from(r % versions.len() as u64).unwrap();
            match r >> 40 & 15 {
                // Новая версия-копия, общая со старой по всем узлам.
                0 | 1 => {
                    let copy = versions[idx].clone();
                    versions.push(copy);
                }
                // Удаление: часть узлов остаётся у единственного владельца.
                2 if versions.len() > 1 => {
                    versions.swap_remove(idx);
                }
                3..=8 => {
                    let (q, m) = &mut versions[idx];
                    assert_eq!(q.pop_mut(), m.pop_front());
                }
                _ => {
                    let (q, m) = &mut versions[idx];
                    q.push_mut(i);
                    m.push_back(i);
                }
            }
            if let Some((q, m)) = versions.get(idx) {
                q.check_invariants();
                assert_eq!(q.len(), m.len());
                assert_eq!(q.front(), m.front());
            }
        }

        let (min_versions, min_len) = if cfg!(miri) { (3, 5) } else { (10, 20) };
        assert!(versions.len() > min_versions);
        assert!(versions.iter().any(|(q, _)| q.len() > min_len));
        for (q, m) in &versions {
            q.check_invariants();
            assert!(q.iter().eq(m.iter().copied()));
        }
    }

    #[test]
    fn pop_mut_moves_value_out_of_unshared_node() {
        #[derive(Debug, PartialEq)]
        struct Counted(u32);
        static CLONES: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        impl Clone for Counted {
            fn clone(&self) -> Self {
                CLONES.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                Self(self.0)
            }
        }

        let clones = || CLONES.load(std::sync::atomic::Ordering::Relaxed);

        // Одноэлементная очередь: ротация завершается за один шаг и переносит
        // значение из уникального узла `rear`, ничего не клонируя.
        let mut q = Queue::new();
        q.push_mut(Counted(7));
        assert_eq!(clones(), 0);

        // Узел общий с копией — значение клонируется, копия не меняется.
        let copy = q.clone();
        assert_eq!(q.pop_mut(), Some(Counted(7)));
        assert_eq!(clones(), 1);
        assert_eq!(copy.front(), Some(&Counted(7)));

        // Узлом владеет только `copy` — значение забирается из узла.
        let mut copy = copy;
        assert_eq!(copy.pop_mut(), Some(Counted(7)));
        assert_eq!(clones(), 1);
        assert_eq!(copy.pop_mut(), None);
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
                ref rot if rot.is_idle() => "Idle",
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
