//! Инкрементальная ротация Hood–Melville: пошаговое построение
//! `new_front = front ++ reverse(rear)`.
//!
//! Аналог `rotation` / `rotate_step` / `finish_rotation` из `c/include/fqueue_template.h`.
//! Вместо tagged union — enum с данными; шаг меняет состояние на месте
//! (`step_mut(&mut self)`), как `rotate_step` в C. Готовый фронт возвращается
//! из шага, а состояние сразу становится `Idle`.
//!
//! Раскладка — 32 байта без тега, чтобы `Queue` занимала 64 (одна строка
//! кэша): у `Reversing` тыл непуст до конца фазы, поэтому хранится как
//! [`NonEmptyStack`], и нулевой указатель на его месте означает `Appending`
//! (niche); `Idle` — это `Appending` с пустым `rfront` (в настоящей фазе
//! `Appending` он непуст: `1 <= ok <= |rfront|`). Счётчик `ok` лежит в
//! `Queue` и передаётся в методы: внутри enum он добавил бы 8 байт с
//! выравниванием.
//!
//! Переходы по значению (`step(self) -> Step`) читались красивее, но после
//! компиляции каждый шаг перемещал enum через память и вызывал
//! `drop_in_place` для заведомо пустых значений; API по значению оставлен
//! только тестам.

use std::alloc::Allocator;
use std::mem;

use crate::refcount::RefCount;
use crate::stack::{NonEmptyStack, Stack};

/// Тип длин и счётчика `ok` (см. поля `Queue`); в C — `fqueue_impl_len`.
pub(crate) type Len = u32;

/// Состояние ротации. Хранит четыре из шести стеков очереди.
///
/// Счётчик `ok` — сколько элементов старого фронта ещё живы в очереди и не
/// скопированы в `acc` — хранится снаружи (поле `Queue::ok`); вне ротации он
/// не используется.
pub(crate) enum Rotation<T, R: RefCount, A: Allocator> {
    /// Параллельно разворачиваются `rear` → `acc` и `front` → `rfront`.
    ///
    /// Инвариант: `|rear| == |front| + 1`, оба уменьшаются на каждом шаге.
    Reversing {
        /// Снимок фронта на старте (`old_front` в C).
        front: Stack<T, R, A>,
        /// `reverse(front)`, строится по ходу (`rfacc` в C).
        rfront: Stack<T, R, A>,
        /// Снимок тыла на старте (`old_rear` в C).
        rear: NonEmptyStack<T, R, A>,
        /// `reverse(rear)`, будущий хвост нового фронта.
        acc: Stack<T, R, A>,
    },
    /// Элементы `rfront` по одному переносятся на вершину `acc`.
    ///
    /// Инвариант: `1 <= ok <= |rfront|`. С пустыми `rfront` и `acc` — `Idle`:
    /// ротация не идёт ([`Rotation::idle`], [`Rotation::is_idle`]).
    Appending {
        rfront: Stack<T, R, A>,
        acc: Stack<T, R, A>,
    },
}

impl<T, R: RefCount, A: Allocator> Rotation<T, R, A> {
    /// Ротация не идёт.
    pub(crate) const fn idle() -> Self {
        Self::Appending {
            rfront: Stack::new(),
            acc: Stack::new(),
        }
    }

    #[inline]
    pub(crate) const fn is_idle(&self) -> bool {
        matches!(self, Self::Appending { rfront, .. } if rfront.is_empty())
    }
}

impl<T, R: RefCount, A: Allocator> Default for Rotation<T, R, A> {
    fn default() -> Self {
        Self::idle()
    }
}

// Вручную: derive потребовал бы `T: Clone`, хотя клонируются только указатели на узлы.
impl<T, R: RefCount, A: Allocator> Clone for Rotation<T, R, A> {
    #[inline]
    fn clone(&self) -> Self {
        match self {
            Self::Reversing {
                front,
                rfront,
                rear,
                acc,
            } => Self::Reversing {
                front: front.clone(),
                rfront: rfront.clone(),
                rear: rear.clone(),
                acc: acc.clone(),
            },
            Self::Appending { rfront, acc } => Self::Appending {
                rfront: rfront.clone(),
                acc: acc.clone(),
            },
        }
    }
}

/// Результат одного перехода (API по значению; используется в тестах).
#[cfg(test)]
pub(crate) enum Step<T, R: RefCount, A: Allocator> {
    /// Ротация идёт; второе поле — `ok`.
    Running(Rotation<T, R, A>, Len),
    /// Ротация завершена; внутри — новый фронт очереди.
    Done(Stack<T, R, A>),
}

impl<T: Clone, R: RefCount, A: Allocator + Clone> Rotation<T, R, A> {
    /// Начать ротацию — аналог `start_rotation`; `ok` становится `front_len`.
    ///
    /// `front` разделяет узлы с живым фронтом очереди; `rear` забирается
    /// у очереди целиком.
    #[inline]
    pub(crate) fn start(front: Stack<T, R, A>, rear: Stack<T, R, A>) -> Self {
        Self::Reversing {
            front,
            rfront: Stack::new(),
            rear: NonEmptyStack::new(rear).expect("ротация начинается при |rear| = |front| + 1"),
            acc: Stack::new(),
        }
    }

    /// Один шаг ротации на месте — аналог `rotate_step`. По завершении
    /// возвращает новый фронт, а состояние становится `Idle`.
    #[inline]
    pub(crate) fn step_mut(&mut self, ok: &mut Len, alloc: &A) -> Option<Stack<T, R, A>> {
        match self {
            Self::Reversing {
                front,
                rfront,
                rear,
                acc,
            } => {
                let Some(y) = rear.pop_unless_last() else {
                    return self.finish_reversing(*ok, alloc);
                };
                acc.push(y, alloc);
                if let Some(x) = front.pop() {
                    rfront.push(x, alloc);
                }
                None
            }
            Self::Appending { rfront, acc } => {
                // Пустой `rfront` — `Idle`.
                let x = rfront.pop()?;
                acc.push(x, alloc);
                *ok -= 1;
                if *ok == 0 {
                    let acc = mem::take(acc);
                    *self = Self::idle();
                    Some(acc)
                } else {
                    None
                }
            }
        }
    }

    /// Последний шаг `Reversing`: тыл из одного элемента расходуется, фаза
    /// переходит в `Appending` (или сразу завершается).
    #[cold]
    fn finish_reversing(&mut self, ok: Len, alloc: &A) -> Option<Stack<T, R, A>> {
        let Self::Reversing {
            mut front,
            mut rfront,
            rear,
            mut acc,
        } = mem::take(self)
        else {
            unreachable!("вызывается только из Reversing");
        };
        acc.push(rear.into_last(), alloc);
        if let Some(x) = front.pop() {
            rfront.push(x, alloc);
        }
        debug_assert!(front.is_empty(), "front исчерпывается вместе с rear");
        self.enter_appending(ok, rfront, acc)
    }

    /// Учесть `pop` из живого фронта очереди во время ротации.
    ///
    /// Выброшенный элемент старого фронта больше не нужно копировать в `acc`,
    /// поэтому `ok` уменьшается. Если копировать больше нечего, `acc` уже
    /// является новым фронтом — он возвращается, состояние становится `Idle`.
    /// Вне ротации ничего не делает.
    #[inline]
    pub(crate) fn invalidate_mut(&mut self, ok: &mut Len) -> Option<Stack<T, R, A>> {
        const MSG: &str = "pop во время ротации: ok > 0, иначе фронт очереди пуст";
        match self {
            Self::Reversing { .. } => {
                *ok = ok.checked_sub(1).expect(MSG);
                None
            }
            Self::Appending { rfront, .. } if rfront.is_empty() => None,
            Self::Appending { acc, .. } => {
                *ok = ok.checked_sub(1).expect(MSG);
                if *ok == 0 {
                    let acc = mem::take(acc);
                    *self = Self::idle();
                    Some(acc)
                } else {
                    None
                }
            }
        }
    }

    /// Войти в `Appending`; при `ok == 0` — сразу готово.
    ///
    /// Остаток `rfront` в этом случае — уже выброшенные `pop`-ом элементы,
    /// он освобождается вместе со стеком.
    #[inline]
    fn enter_appending(
        &mut self,
        ok: Len,
        rfront: Stack<T, R, A>,
        acc: Stack<T, R, A>,
    ) -> Option<Stack<T, R, A>> {
        if ok == 0 {
            *self = Self::idle();
            Some(acc)
        } else {
            *self = Self::Appending { rfront, acc };
            None
        }
    }

    #[cfg(test)]
    pub(crate) fn step(mut self, mut ok: Len) -> Step<T, R, A>
    where
        A: Default,
    {
        match self.step_mut(&mut ok, &A::default()) {
            Some(front) => Step::Done(front),
            None => Step::Running(self, ok),
        }
    }

    #[cfg(test)]
    pub(crate) fn invalidate(mut self, mut ok: Len) -> Step<T, R, A> {
        match self.invalidate_mut(&mut ok) {
            Some(front) => Step::Done(front),
            None => Step::Running(self, ok),
        }
    }
}

#[cfg(test)]
impl<T: Clone, R: RefCount, A: Allocator + Clone + Default> Step<T, R, A> {
    /// Ещё один шаг; на завершённой ротации ничего не делает.
    pub(crate) fn step(self) -> Self {
        match self {
            Self::Running(rot, ok) => rot.step(ok),
            done @ Self::Done(_) => done,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::refcount::Local;
    use std::alloc::Global;

    type Stack<T> = crate::stack::Stack<T, Local, Global>;
    type Rotation<T> = super::Rotation<T, Local, Global>;
    type Step<T> = super::Step<T, Local, Global>;

    /// Стек, у которого `items[0]` на вершине.
    fn stack(items: &[usize]) -> Stack<usize> {
        items
            .iter()
            .rev()
            .fold(Stack::new(), |s, &x| Stack::cons(x, s, &Global))
    }

    fn to_vec(s: &Stack<usize>) -> Vec<usize> {
        s.iter().copied().collect()
    }

    /// Очередь `1..=n` в виде `front = [1..=f]`, `rear = [n..=f+1]`,
    /// где `n = 2f + 1` — момент старта ротации.
    fn fixture(f: usize) -> (Stack<usize>, Stack<usize>) {
        let front: Vec<_> = (1..=f).collect();
        let rear: Vec<_> = (f + 1..=2 * f + 1).rev().collect();
        (stack(&front), stack(&rear))
    }

    fn start(f: usize) -> Step<usize> {
        let (front, rear) = fixture(f);
        Step::Running(Rotation::start(front, rear), Len::try_from(f).unwrap())
    }

    /// Прогнать ротацию до конца; перед шагом `i` вызвать `invalidate`,
    /// если `pop_before(i)`. Возвращает (новый фронт, число шагов, число pop).
    fn run(
        mut step: Step<usize>,
        pop_before: impl Fn(usize) -> bool,
    ) -> (Vec<usize>, usize, usize) {
        let (mut steps, mut pops) = (0, 0);
        loop {
            if let Step::Running(rot, ok) = step {
                step = if pop_before(steps) {
                    pops += 1;
                    rot.invalidate(ok)
                } else {
                    Step::Running(rot, ok)
                };
            }
            if let Step::Running(rot, ok) = step {
                steps += 1;
                step = rot.step(ok);
            }
            if let Step::Done(front) = step {
                return (to_vec(&front), steps, pops);
            }
        }
    }

    #[test]
    fn layout_has_no_tag() {
        assert_eq!(size_of::<Rotation<i32>>(), 4 * size_of::<usize>());
    }

    #[test]
    fn idle_stays_idle() {
        let rot: Rotation<usize> = Rotation::default();
        assert!(rot.is_idle());
        assert!(matches!(rot.step(0), Step::Running(r, 0) if r.is_idle()));
        assert!(matches!(
            Rotation::<usize>::idle().invalidate(0),
            Step::Running(r, 0) if r.is_idle()
        ));
    }

    #[test]
    fn single_element_rotation_finishes_in_one_step() {
        // push в пустую очередь: front = [], rear = [1].
        let Step::Done(front) = start(0).step() else {
            panic!("ожидалось завершение за один шаг");
        };
        assert_eq!(to_vec(&front), [1]);
    }

    #[test]
    fn phases_take_f_plus_one_then_f_steps() {
        let f = 3;
        let mut step = start(f);
        for _ in 0..f {
            step = step.step();
            assert!(matches!(step, Step::Running(Rotation::Reversing { .. }, _)));
        }
        step = step.step();
        let Step::Running(Rotation::Appending { rfront, acc }, ok) = &step else {
            panic!("после f + 1 шагов ожидалась фаза Appending");
        };
        assert_eq!(*ok, 3);
        assert_eq!(to_vec(rfront), [3, 2, 1]);
        assert_eq!(to_vec(acc), [4, 5, 6, 7]);

        for _ in 0..f - 1 {
            step = step.step();
            assert!(
                matches!(&step, Step::Running(r @ Rotation::Appending { .. }, _) if !r.is_idle())
            );
        }
        let Step::Done(front) = step.step() else {
            panic!("после 2f + 1 шагов ожидалось завершение");
        };
        assert_eq!(to_vec(&front), [1, 2, 3, 4, 5, 6, 7]);
    }

    #[test]
    fn rotation_does_not_modify_source_stacks() {
        let (front, rear) = fixture(4);
        let rot = Rotation::start(front.clone(), rear.clone());
        let (result, _, _) = run(Step::Running(rot, 4), |_| false);
        assert_eq!(result, (1..=9).collect::<Vec<_>>());
        assert_eq!(to_vec(&front), [1, 2, 3, 4]);
        assert_eq!(to_vec(&rear), [9, 8, 7, 6, 5]);
    }

    #[test]
    fn invalidate_in_appending_with_last_ok_finishes_immediately() {
        let rot = Rotation::Appending {
            rfront: stack(&[2, 1]),
            acc: stack(&[3, 4]),
        };
        let Step::Done(front) = rot.invalidate(1) else {
            panic!("ok стал 0 — acc уже новый фронт");
        };
        assert_eq!(to_vec(&front), [3, 4]);
    }

    #[test]
    #[should_panic(expected = "ok > 0")]
    fn invalidate_past_zero_panics() {
        let rot = Rotation::<usize>::start(Stack::new(), stack(&[1, 2]));
        let _ = rot.invalidate(0);
    }

    /// Все размеры фронта и все расписания pop'ов «каждые `gap` шагов, не
    /// больше `k` раз»: результат — оставшиеся элементы в порядке FIFO, а
    /// число шагов не превышает 2f + 1 (на этом держится O(1) очереди).
    #[test]
    fn exhaustive_small_schedules() {
        for f in 0..10 {
            for k in 0..=f {
                for gap in 1..=3 {
                    let pop_before = |i: usize| i.is_multiple_of(gap) && i / gap < k;
                    let (result, steps, pops) = run(start(f), pop_before);
                    let expected: Vec<_> = (pops + 1..=2 * f + 1).collect();
                    assert_eq!(result, expected, "f={f} k={k} gap={gap}");
                    assert!(steps <= 2 * f + 1, "f={f} k={k} gap={gap}: {steps} шагов");
                }
            }
        }
    }
}
