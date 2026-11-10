//! Инкрементальная ротация Hood–Melville: пошаговое построение
//! `new_front = front ++ reverse(rear)`.
//!
//! Аналог `rotation` / `rotate_step` / `finish_rotation` из `c/include/fqueue_template.h`.
//! Вместо tagged union — enum с данными; шаг меняет состояние на месте
//! (`step_mut(&mut self)`), как `rotate_step` в C. Готовый фронт возвращается
//! из шага, а состояние сразу становится `Idle`.
//!
//! Переходы по значению (`step(self) -> Step`) читались красивее, но после
//! компиляции каждый шаг перемещал 48-байтный enum через память и вызывал
//! `drop_in_place` для заведомо пустых значений; API по значению оставлен
//! только тестам.

use std::mem;

use crate::stack::Stack;

/// Тип длин и счётчика `ok` (см. поля `Queue`); в C — `fqueue_impl_len`.
pub(crate) type Len = u32;

/// Состояние ротации. Хранит четыре из шести стеков очереди.
#[derive(Default)]
pub(crate) enum Rotation<T> {
    /// Ротация не идёт.
    #[default]
    Idle,
    /// Параллельно разворачиваются `rear` → `acc` и `front` → `rfront`.
    ///
    /// Инвариант: `|rear| == |front| + 1`, оба уменьшаются на каждом шаге.
    Reversing {
        /// Сколько элементов старого фронта ещё живы в очереди и не скопированы в `acc`.
        ok: Len,
        /// Снимок фронта на старте (`old_front` в C).
        front: Stack<T>,
        /// `reverse(front)`, строится по ходу (`rfacc` в C).
        rfront: Stack<T>,
        /// Снимок тыла на старте (`old_rear` в C).
        rear: Stack<T>,
        /// `reverse(rear)`, будущий хвост нового фронта.
        acc: Stack<T>,
    },
    /// Элементы `rfront` по одному переносятся на вершину `acc`.
    ///
    /// Инвариант: `1 <= ok <= |rfront|`.
    Appending {
        ok: Len,
        rfront: Stack<T>,
        acc: Stack<T>,
    },
}

// Вручную: derive потребовал бы `T: Clone`, хотя клонируются только указатели на узлы.
impl<T> Clone for Rotation<T> {
    fn clone(&self) -> Self {
        match self {
            Self::Idle => Self::Idle,
            Self::Reversing {
                ok,
                front,
                rfront,
                rear,
                acc,
            } => Self::Reversing {
                ok: *ok,
                front: front.clone(),
                rfront: rfront.clone(),
                rear: rear.clone(),
                acc: acc.clone(),
            },
            Self::Appending { ok, rfront, acc } => Self::Appending {
                ok: *ok,
                rfront: rfront.clone(),
                acc: acc.clone(),
            },
        }
    }
}

/// Результат одного перехода (API по значению; используется в тестах).
#[cfg(test)]
pub(crate) enum Step<T> {
    Running(Rotation<T>),
    /// Ротация завершена; внутри — новый фронт очереди.
    Done(Stack<T>),
}

impl<T: Clone> Rotation<T> {
    /// Начать ротацию — аналог `start_rotation`.
    ///
    /// `front` разделяет узлы с живым фронтом очереди; `rear` забирается
    /// у очереди целиком, `front_len == |front|`.
    pub(crate) fn start(front: Stack<T>, front_len: Len, rear: Stack<T>) -> Self {
        debug_assert!(
            !rear.is_empty(),
            "ротация начинается при |rear| = |front| + 1"
        );
        Self::Reversing {
            ok: front_len,
            front,
            rfront: Stack::new(),
            rear,
            acc: Stack::new(),
        }
    }

    /// Один шаг ротации на месте — аналог `rotate_step`. По завершении
    /// возвращает новый фронт, а состояние становится `Idle`.
    pub(crate) fn step_mut(&mut self) -> Option<Stack<T>> {
        match self {
            Self::Idle => None,
            Self::Reversing {
                ok,
                front,
                rfront,
                rear,
                acc,
            } => {
                let y = rear.pop().expect("Reversing: rear не пуст до конца фазы");
                acc.push(y);
                if let Some(x) = front.pop() {
                    rfront.push(x);
                }
                if !rear.is_empty() {
                    return None;
                }
                debug_assert!(front.is_empty(), "front исчерпывается вместе с rear");
                let (ok, rfront, acc) = (*ok, mem::take(rfront), mem::take(acc));
                self.enter_appending(ok, rfront, acc)
            }
            Self::Appending { ok, rfront, acc } => {
                let x = rfront.pop().expect("Appending: ok <= |rfront|");
                acc.push(x);
                *ok -= 1;
                if *ok == 0 {
                    let acc = mem::take(acc);
                    *self = Self::Idle;
                    Some(acc)
                } else {
                    None
                }
            }
        }
    }

    /// Учесть `pop` из живого фронта очереди во время ротации.
    ///
    /// Выброшенный элемент старого фронта больше не нужно копировать в `acc`,
    /// поэтому `ok` уменьшается. Если копировать больше нечего, `acc` уже
    /// является новым фронтом — он возвращается, состояние становится `Idle`.
    pub(crate) fn invalidate_mut(&mut self) -> Option<Stack<T>> {
        const MSG: &str = "pop во время ротации: ok > 0, иначе фронт очереди пуст";
        match self {
            Self::Idle => None,
            Self::Reversing { ok, .. } => {
                *ok = ok.checked_sub(1).expect(MSG);
                None
            }
            Self::Appending { ok, acc, .. } => {
                *ok = ok.checked_sub(1).expect(MSG);
                if *ok == 0 {
                    let acc = mem::take(acc);
                    *self = Self::Idle;
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
    fn enter_appending(&mut self, ok: Len, rfront: Stack<T>, acc: Stack<T>) -> Option<Stack<T>> {
        if ok == 0 {
            *self = Self::Idle;
            Some(acc)
        } else {
            *self = Self::Appending { ok, rfront, acc };
            None
        }
    }

    #[cfg(test)]
    pub(crate) fn step(mut self) -> Step<T> {
        match self.step_mut() {
            Some(front) => Step::Done(front),
            None => Step::Running(self),
        }
    }

    #[cfg(test)]
    pub(crate) fn invalidate(mut self) -> Step<T> {
        match self.invalidate_mut() {
            Some(front) => Step::Done(front),
            None => Step::Running(self),
        }
    }
}

#[cfg(test)]
impl<T: Clone> Step<T> {
    /// Ещё один шаг; на завершённой ротации ничего не делает.
    pub(crate) fn step(self) -> Self {
        match self {
            Self::Running(rot) => rot.step(),
            done @ Self::Done(_) => done,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Стек, у которого `items[0]` на вершине.
    fn stack(items: &[usize]) -> Stack<usize> {
        items
            .iter()
            .rev()
            .fold(Stack::new(), |s, &x| Stack::cons(x, s))
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

    fn start(f: usize) -> Rotation<usize> {
        let (front, rear) = fixture(f);
        Rotation::start(front, Len::try_from(f).unwrap(), rear)
    }

    /// Прогнать ротацию до конца; перед шагом `i` вызвать `invalidate`,
    /// если `pop_before(i)`. Возвращает (новый фронт, число шагов, число pop).
    fn run(
        mut rot: Rotation<usize>,
        pop_before: impl Fn(usize) -> bool,
    ) -> (Vec<usize>, usize, usize) {
        let (mut steps, mut pops) = (0, 0);
        loop {
            let mut next = if pop_before(steps) {
                pops += 1;
                rot.invalidate()
            } else {
                Step::Running(rot)
            };
            if let Step::Running(r) = next {
                steps += 1;
                next = r.step();
            }
            match next {
                Step::Done(front) => return (to_vec(&front), steps, pops),
                Step::Running(r) => rot = r,
            }
        }
    }

    #[test]
    fn idle_stays_idle() {
        let rot: Rotation<usize> = Rotation::default();
        assert!(matches!(rot, Rotation::Idle));
        assert!(matches!(rot.step(), Step::Running(Rotation::Idle)));
        assert!(matches!(
            Rotation::<usize>::Idle.invalidate(),
            Step::Running(Rotation::Idle)
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
        let mut step = Step::Running(start(f));
        for _ in 0..f {
            step = step.step();
            assert!(matches!(step, Step::Running(Rotation::Reversing { .. })));
        }
        step = step.step();
        let Step::Running(Rotation::Appending { ok, rfront, acc }) = &step else {
            panic!("после f + 1 шагов ожидалась фаза Appending");
        };
        assert_eq!(*ok, 3);
        assert_eq!(to_vec(rfront), [3, 2, 1]);
        assert_eq!(to_vec(acc), [4, 5, 6, 7]);

        for _ in 0..f - 1 {
            step = step.step();
            assert!(matches!(step, Step::Running(Rotation::Appending { .. })));
        }
        let Step::Done(front) = step.step() else {
            panic!("после 2f + 1 шагов ожидалось завершение");
        };
        assert_eq!(to_vec(&front), [1, 2, 3, 4, 5, 6, 7]);
    }

    #[test]
    fn rotation_does_not_modify_source_stacks() {
        let (front, rear) = fixture(4);
        let rot = Rotation::start(front.clone(), 4, rear.clone());
        let (result, _, _) = run(rot, |_| false);
        assert_eq!(result, (1..=9).collect::<Vec<_>>());
        assert_eq!(to_vec(&front), [1, 2, 3, 4]);
        assert_eq!(to_vec(&rear), [9, 8, 7, 6, 5]);
    }

    #[test]
    fn invalidate_in_appending_with_last_ok_finishes_immediately() {
        let rot = Rotation::Appending {
            ok: 1,
            rfront: stack(&[2, 1]),
            acc: stack(&[3, 4]),
        };
        let Step::Done(front) = rot.invalidate() else {
            panic!("ok стал 0 — acc уже новый фронт");
        };
        assert_eq!(to_vec(&front), [3, 4]);
    }

    #[test]
    #[should_panic(expected = "ok > 0")]
    fn invalidate_past_zero_panics() {
        let rot = Rotation::<usize>::start(Stack::new(), 0, stack(&[1, 2]));
        let _ = rot.invalidate();
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
