# hood-melville

Persistent real-time queue of Hood and Melville in C and Rust: worst-case O(1)
`push`, `pop` and `front`, every operation returns a new version.

Исследование и две реализации функциональной (персистентной) очереди
реального времени на шести стеках по статье
[R. Hood, R. Melville. Real-time queue operations in pure LISP. *Information
Processing Letters*, 1981, 13(2), 50–54](https://doi.org/10.1016/0020-0190(81)90030-2).

Цель — две максимально быстрые и корректные реализации одного алгоритма с
одинаковой семантикой и честное измерение того, какая из них быстрее.

## Что внутри

| путь | содержимое |
|---|---|
| `c/` | библиотека на C17: шаблон `include/fqueue_template.h` для любого типа элемента, экземпляр для `int` (`include/fqueue.h`, `src/fqueue.c`), тесты на Catch2, нагрузка для бенчмарков |
| `rust/` | крейт `hood-melville`; `unsafe` только в `src/node.rs` (свой счётчик ссылок в узле), проверяется Miri |
| `bench/compare.py` | сравнение бинарников: чередование запусков, режимы аллокатора, несколько раскладок кода, бутстреп-интервалы |
| `docs/proof.md` | доказательство корректности и worst-case O(1) при одном шаге ротации на операцию |
| `docs/benchmarks.md` | методика измерений, журнал оптимизаций (что принято и что отклонено), результаты |
| `docs/c-implementation-notes.md` | заметки об исходной C-реализации (исторический документ) |

## Гарантии

- `push`/`pop` строят новую версию, не изменяя аргумент; версии разделяют узлы.
  Если старая версия больше не нужна, её можно изменить на месте
  (`push_mut`/`pop_mut` в Rust, `fqueue_push_in_place`/`fqueue_pop_in_place`
  в C): без копирования версии и освобождения старой, другие версии не
  затрагиваются.
- Worst-case O(1) на построение версии и доступ: не больше 3 новых узлов на
  `push` и 2 на `pop`; для обобщённого `T` — плюс не более двух `T::clone`.
- Уничтожение версии **не** O(1) в худшем случае: освобождается всё, чем она
  владела монопольно. Амортизированно это O(1) на операцию.
- Доказательство — `docs/proof.md`; проверки — полный перебор коротких историй,
  property-based тесты против эталонной модели (включая ветвящиеся истории),
  Miri для Rust, ASan/UBSan для C.

## Использование

C — версии хранятся по значению в памяти вызывающего, как `Queue` в Rust:

```c
#include "fqueue.h"

fqueue a, b;
fqueue_init(&a);
fqueue_status s = fqueue_push(&b, &a, 42); // a не изменилась
if (s != FQUEUE_OK) {                      // FQUEUE_NOMEM / FQUEUE_OVERFLOW:
  fprintf(stderr, "%s\n", fqueue_strerror(s)); // b не инициализирована
  ...
}
int x;
fqueue_front(&b, &x);           // true, x == 42
fqueue_destroy(&a);
fqueue_destroy(&b);
```

Библиотека не завершает процесс: нехватка памяти и переполнение длины или
счётчика ссылок возвращаются как `fqueue_status`, аргумент при этом не
меняется. В Rust те же ситуации, как принято в языке, — паника (`push` в
очередь из `u32::MAX` элементов) или `abort` (переполнение счётчика, OOM).

Очередь для своего типа элемента — экземпляр шаблона (элементы копируются
присваиванием, как в klib):

```c
// strq.h                          // strq.c
#define FQUEUE_NAME strq           #include "strq.h"
#define FQUEUE_T    const char *   #define FQUEUE_NAME strq
#include "fqueue_template.h"       #define FQUEUE_T    const char *
                                   #define FQUEUE_IMPLEMENTATION
                                   #include "fqueue_template.h"
```

Получаются тип `strq` и функции `strq_init`, `strq_push`, `strq_pop`,
`strq_push_in_place`, `strq_pop_in_place`, `strq_front`, `strq_clone`,
`strq_destroy`, итератор `strq_iter_*`.

Изменение на месте — то же, что новая версия с удалением старой, но дешевле;
при ошибке очередь не меняется:

```c
fqueue q;
fqueue_init(&q);
fqueue_status s = fqueue_push_in_place(&q, 1);
int x;
s = fqueue_pop_in_place(&q, &x);   // FQUEUE_OK, x == 1; FQUEUE_EMPTY для пустой
```

Rust:

```rust
use hood_melville::Queue;

let q1 = Queue::new().push(1).push(2);
let q2 = q1.pop().unwrap();
assert_eq!(q1.front(), Some(&1)); // старая версия не изменилась
assert_eq!(q2.front(), Some(&2));

let mut q = q2.clone();
q.push_mut(3);                    // на месте; q2 не изменилась
assert_eq!(q.pop_mut(), Some(2));
```

## Результаты (Apple M5, 4 раскладки кода × 12 раундов)

Такты, медиана быстрого режима; обе реализации собраны с `-O3`/fat LTO и
`native`, версии хранятся по значению.

| сценарий | C, млн | Rust, млн | Rust / C (95% CI) |
|---|---:|---:|---|
| fill 10^6 | 179.6 | 195.7 | 1.090 (1.061–1.116) |
| mixed 10^6 | 349.0 | 396.7 | 1.137 (1.108–1.175) |
| persistent 10^6 | 459.5 | 514.0 | 1.119 (1.082–1.142) |
| branching 10^6 | 198.5 | 200.2 | 1.009 (0.998–1.043) |
| fill 10^7 | 1834.6 | 2060.1 | 1.123 (1.071–1.165) |
| mixed 10^7 | 3534.5 | 4002.3 | 1.132 (1.072–1.181) |

C быстрее на 9–14% везде, кроме `branching` (разница в пределах шума), при
равных условиях: одинаковая схема владения узлами, тип длин `u32`, проверки
переполнения в обеих реализациях (в C — с кодами ошибок). Rust выполняет на
8–10% больше инструкций — это следующая цель исследования. Подробности,
распределения и оговорки (режимы аллокатора, влияние раскладки кода) —
`docs/benchmarks.md`.

## Установка C-библиотеки

```
cmake -S c -B build/c -DCMAKE_BUILD_TYPE=Release [-DBUILD_SHARED_LIBS=ON]
cmake --build build/c
cmake --install build/c --prefix <prefix>
```

Ставятся `libfqueue` (очередь для `int`), заголовки `fqueue.h`,
`fqueue_common.h`, `fqueue_template.h` и пакет CMake:

```cmake
find_package(hood_melville 0.1 REQUIRED)
target_link_libraries(app PRIVATE hood_melville::fqueue)
```

Подключать можно и через `add_subdirectory`/`FetchContent` — тесты и нагрузка
тогда не собираются (`HOOD_MELVILLE_BUILD_TESTS`, `HOOD_MELVILLE_BUILD_BENCH`).
Разделяемая библиотека экспортирует только публичные функции. Пример —
`c/examples/consumer` (`just c-install-check`).

## Запуск

Нужны `cmake`, C/C++-компилятор, Rust (stable; nightly с `miri` — для
`rust-miri`), [`just`](https://github.com/casey/just), Python 3.

```
just test                    # C- и Rust-тесты
just c-asan                  # C-тесты под ASan/UBSan
just c-leaks                 # C-тесты под leaks (macOS)
just c-install-check         # установка C-библиотеки и пример через find_package
just rust-lint               # rustfmt + clippy::pedantic
just rust-miri               # Rust-тесты под Miri
just bench-compare-layouts   # сравнение C и Rust (несколько минут)
just bench-compare-mut       # то же для операций на месте
```

## Происхождение

Проект вырос из лабораторной работы курса «Алгоритмы и структуры данных»:
постановка задачи и исходный набросок C API — Mark Kochanov. Обе реализации,
оптимизации, доказательство и методика измерений — zornfeuer (ранние коммиты
C-реализации в курсовом репозитории — под прежним именем пользователя
stahlfeuer).

## Лицензия

[MIT](LICENSE).
