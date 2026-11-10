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
`strq_front`, `strq_clone`, `strq_destroy`, итератор `strq_iter_*`.

Rust:

```rust
use hood_melville::Queue;

let q1 = Queue::new().push(1).push(2);
let q2 = q1.pop().unwrap();
assert_eq!(q1.front(), Some(&1)); // старая версия не изменилась
assert_eq!(q2.front(), Some(&2));
```

## Результаты (Apple M5, 4 раскладки кода × 12 раундов)

Такты, медиана быстрого режима; обе реализации собраны с `-O3`/fat LTO и
`native`, версии хранятся по значению.

| сценарий | C, млн | Rust, млн | Rust / C (95% CI) |
|---|---:|---:|---|
| fill 10^6 | 181.6 | 203.7 | 1.122 (1.096–1.176) |
| mixed 10^6 | 359.0 | 405.2 | 1.129 (1.104–1.164) |
| persistent 10^6 | 465.8 | 533.8 | 1.146 (1.112–1.167) |
| branching 10^6 | 211.3 | 216.7 | 1.026 (1.004–1.047) |
| fill 10^7 | 1864.1 | 2036.2 | 1.092 (1.051–1.120) |
| mixed 10^7 | 3602.1 | 3913.8 | 1.087 (1.049–1.152) |

C сейчас быстрее во всех сценариях; Rust выполняет на 10–13% больше
инструкций — это следующая цель исследования. Подробности, распределения и
оговорки (режимы аллокатора, влияние раскладки кода) — `docs/benchmarks.md`.

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
```

## Происхождение

Проект вырос из лабораторной работы курса «Алгоритмы и структуры данных»:
постановка задачи и исходный набросок C API — Mark Kochanov. Обе реализации,
оптимизации, доказательство и методика измерений — zornfeuer (ранние коммиты
C-реализации в курсовом репозитории — под прежним именем пользователя
stahlfeuer).

## Лицензия

[MIT](LICENSE).
