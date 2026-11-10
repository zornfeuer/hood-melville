//! Property-based тесты: очередь против эталонной `VecDeque`.

use std::collections::VecDeque;
use std::rc::Rc;

use hood_melville::Queue;
use proptest::prelude::*;

#[derive(Clone, Debug)]
enum Op {
    Push(u32),
    Pop,
}

/// Push'ей вдвое больше, чем pop'ов, — очереди успевают вырасти и пройти
/// через несколько ротаций.
fn op() -> impl Strategy<Value = Op> {
    prop_oneof![2 => any::<u32>().prop_map(Op::Push), 1 => Just(Op::Pop)]
}

fn apply(q: &Queue<u32>, model: &VecDeque<u32>, op: &Op) -> (Queue<u32>, VecDeque<u32>) {
    let mut model = model.clone();
    match *op {
        Op::Push(x) => {
            model.push_back(x);
            (q.push(x), model)
        }
        Op::Pop => {
            if let Some(q) = q.pop() {
                assert!(model.pop_front().is_some(), "pop удался на пустой модели");
                (q, model)
            } else {
                assert!(model.is_empty(), "pop не удался на непустой модели");
                (q.clone(), model)
            }
        }
    }
}

fn same(q: &Queue<u32>, model: &VecDeque<u32>) -> bool {
    q.len() == model.len() && q.front() == model.front() && q.iter().eq(model.iter().copied())
}

/// Под Miri — немного случаев и без файла с найденными падениями
/// (изоляция Miri запрещает доступ к файловой системе).
fn config() -> ProptestConfig {
    if cfg!(miri) {
        ProptestConfig {
            cases: 8,
            failure_persistence: None,
            ..ProptestConfig::default()
        }
    } else {
        ProptestConfig::with_cases(512)
    }
}

proptest! {
    #![proptest_config(config())]

    /// Линейная история: после каждой операции совпадают длина и голова,
    /// в конце — всё содержимое.
    #[test]
    fn linear_history_matches_model(ops in prop::collection::vec(op(), 0..600)) {
        let (mut q, mut model) = (Queue::new(), VecDeque::new());
        for op in &ops {
            (q, model) = apply(&q, &model, op);
            prop_assert_eq!(q.len(), model.len());
            prop_assert_eq!(q.front(), model.front());
        }
        prop_assert!(same(&q, &model));
    }

    /// Ветвящаяся история: каждая операция применяется к произвольной из уже
    /// созданных версий. В конце каждая версия совпадает со своей моделью —
    /// ни одна операция не испортила чужую версию.
    #[test]
    fn every_version_survives_branching(
        steps in prop::collection::vec((any::<prop::sample::Index>(), op()), 0..400),
    ) {
        let mut versions = vec![(Queue::new(), VecDeque::new())];
        for (which, op) in &steps {
            let (q, model) = which.get(&versions);
            let derived = apply(q, model, op);
            versions.push(derived);
        }
        for (q, model) in &versions {
            prop_assert!(same(q, model));
        }
    }

    /// Операция не меняет свой аргумент.
    #[test]
    fn operations_do_not_modify_their_input(
        prefix in prop::collection::vec(op(), 0..300),
        op in op(),
    ) {
        let (mut q, mut model) = (Queue::new(), VecDeque::new());
        for op in &prefix {
            (q, model) = apply(&q, &model, op);
        }
        let _derived = apply(&q, &model, &op);
        prop_assert!(same(&q, &model));
    }

    /// `collect` и `iter` — взаимно обратные.
    #[test]
    fn collect_iter_roundtrip(items in prop::collection::vec(any::<u32>(), 0..500)) {
        let q: Queue<u32> = items.iter().copied().collect();
        prop_assert_eq!(q.len(), items.len());
        prop_assert_eq!(q.iter().collect::<Vec<_>>(), items);
    }

    /// Равенство — по содержимому, независимо от истории операций.
    #[test]
    fn equality_is_by_contents(
        a in prop::collection::vec(op(), 0..200),
        b in prop::collection::vec(op(), 0..200),
    ) {
        let build = |ops: &[Op]| {
            ops.iter().fold((Queue::new(), VecDeque::new()), |(q, m), op| apply(&q, &m, op))
        };
        let (qa, ma) = build(&a);
        let (qb, mb) = build(&b);
        prop_assert_eq!(qa == qb, ma == mb);
        let rebuilt: Queue<u32> = ma.iter().copied().collect();
        prop_assert_eq!(&qa, &rebuilt);
    }

    /// После удаления всех версий не остаётся ни одного элемента: каждый
    /// элемент держит ссылку на общий `Rc`, и в конце она снова единственная.
    #[test]
    fn dropping_all_versions_frees_every_element(
        steps in prop::collection::vec((any::<prop::sample::Index>(), any::<bool>()), 0..300),
    ) {
        let tracker = Rc::new(());
        let mut versions = vec![Queue::new()];
        for (which, is_push) in &steps {
            let q = which.get(&versions);
            let derived = if *is_push { Some(q.push(Rc::clone(&tracker))) } else { q.pop() };
            versions.extend(derived);
        }
        drop(versions);
        prop_assert_eq!(Rc::strong_count(&tracker), 1);
    }
}
