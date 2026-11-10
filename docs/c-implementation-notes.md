> Заметки об исходной C-реализации (написаны под прежним именем пользователя
> stahlfeuer). Сохранены как
> исторический документ: вывод о необходимости двух шагов ротации на операцию
> (раздел «Почему 2 шага `rotate_step` на операцию») для текущего кода неверен —
> см. `docs/proof.md`, раздел 8; таблица бенчмарков здесь получена прежней
> методикой (см. `docs/benchmarks.md`), а «аномалия N = 10 000» объясняется
> фазой ротации, а не кэшем.

# Функциональная очередь Hood–Melville — разбор реализации

**Алгоритм:** Hood & Melville, «Real-time queue operations in pure LISP» (1981)  
**Файлы:** `c/include/fqueue.h`, `c/src/fqueue.c`, `c/tests/fqueue_test.cxx`

---

## Бенчмарки — доказательство O(1)

Запуск: `./persistent_test "[benchmark]" --benchmark-samples 100`

### `fqueue_push` — одна операция при разных N

| N очереди | среднее время |
|-----------|--------------|
| 100       | 29 нс        |
| 1 000     | 29 нс        |
| 10 000    | 81 нс ¹      |
| 100 000   | 29 нс        |

### `fqueue_pop` — одна операция при разных N

| N очереди | среднее время |
|-----------|--------------|
| 100       | 21 нс        |
| 1 000     | 22 нс        |
| 10 000    | 79 нс ¹      |
| 100 000   | 21 нс        |

### rotation-trigger push vs normal push при N=10 000

| Операция               | среднее время |
|------------------------|--------------|
| push, запускающий ротацию | 81 нс     |
| обычный push           | 89 нс        |

**Вывод:** время не растёт с N. N=100 000 стоит столько же, сколько N=100.
При O(n) N=100 000 было бы в 1000 раз медленнее N=100. Этого нет.

> ¹ **Аномалия на N=10 000** — не алгоритмическая, а кэшевая. 10 000 узлов × 24 байта ≈ 240 КБ — граница L2/L3 кэша. Время не растёт монотонно с N (N=100 000 снова быстрый), поэтому это не O(n).

---

## Алгоритм

### Идея: очередь на двух стеках

Очередь хранится как два односвязных списка:
- **`front`** — голова содержит первый элемент очереди, отсюда делается `pop`
- **`rear`** — голова содержит *последний* добавленный элемент (порядок обратный), сюда идёт `push`

Инвариант: `|front| ≥ |rear|`. Как только `rear` обгоняет `front` (после push),
запускается **инкрементальная ротация** — ленивое перестроение `rear` в `front`.

### 6 стеков (списков)

| Список      | Роль                                                    |
|-------------|---------------------------------------------------------|
| `front`     | Живой фронт, отсюда dequeue                             |
| `rear`      | Живой тыл, сюда push (в обратном порядке)               |
| `old_front` | Снапшот `front` на старте ротации                       |
| `old_rear`  | Снапшот `rear` на старте ротации                        |
| `acc`       | Строящийся новый `front`                                |
| `rfacc`     | `reverse(old_front)` — строится в REVERSING, потребляется в APPENDING |

### Фазы ротации

Ротация строит `new_front = old_front ++ reverse(old_rear)` пошагово.

**REVERSING** (`|old_rear|` шагов — два процесса параллельно):
```
acc  = cons(old_rear.head, acc)     // строим reverse(old_rear)
rfacc = cons(old_front.head, rfacc) // строим reverse(old_front)
```
По завершении: `acc = [4,5,6]`, `rfacc = [3,2,1]` (для front=[1,2,3], rear=[6,5,4])

**APPENDING** (`ok` шагов — `ok = |old_front|` при старте):
```
acc = cons(rfacc.head, acc)         // prepend reverse(old_front) на acc
ok--
```
По завершении: `acc = [1,2,3,4,5,6]` → становится новым `front`

**Счётчик `ok`:** если во время ротации делается `pop`, `ok` декрементируется.
При `ok == 0` в APPENDING — завершаем ротацию немедленно: выброшенные элементы
не нужно копировать в новый `front`.

### Почему 2 шага `rotate_step` на операцию, а не 1

При старте ротации: `front_len = f`, `rear_len = f + 1`.
Полная ротация требует `2f + 1` шагов (REVERSING: `f+1`, APPENDING: `f`).

С 1 шагом на операцию: за `f` pop'ов до опустошения `front` накапливается
лишь `f + 1` шагов — ротация не успевает, следующий pop падает на `NULL->next`.

С 2 шагами на операцию: за triggering push + `f` pop'ов = `f + 1` операций
выполняется `2(f + 1) = 2f + 2 ≥ 2f + 1` шагов — всегда хватает.

---

## Разбор кода

### Структуры данных

#### `node` — атом памяти

```c
typedef struct node {
  int value;
  int refcount;
  struct node *next;
} node;
```

Обычный узел списка с **reference counting**. Разные версии очереди разделяют
одни и те же узлы — `refcount` считает, сколько владельцев у каждого узла.
Когда счётчик падает до нуля, узел никому не нужен и освобождается.

#### `rotation` — tagged union для состояния ротации

```c
typedef struct {
  hm_phase tag;
  union {
    hm_reversing  reversing;  // old_front, old_rear, acc, rfacc, ok
    hm_appending  appending;  // acc, rfacc, ok
  };
} rotation;
```

Ключевое архитектурное решение: поля `old_front` и `old_rear` физически существуют
**только в REVERSING-варианте** union'а. В APPENDING они недоступны через типы —
к тому времени они уже NULL и не нужны. Нельзя случайно обратиться к `old_front`
в не той фазе. Идея из Rust-перечислений с данными (`enum Phase { Reversing { ... }, Appending { ... } }`).

#### `fqueue`

```c
struct fqueue {
  int      size, front_len, rear_len;
  node    *front, *rear;
  rotation rot;
};
```

---

### Управление памятью

#### `node_retain` / `node_release`

```c
static node *node_retain(node *n) {
  if (n) n->refcount++;
  return n;
}

static void node_release(node *n) {
  while (n && --n->refcount == 0) {
    node *next = n->next;
    free(n);
    n = next;
  }
}
```

`retain` — "у узла появился ещё один владелец".
`release` — "отказываюсь от владения". Итеративный цикл по цепочке:
если освобождённый узел был последним владельцем своего `next`, освобождается и он — 
и так до первого живого узла. Без рекурсии, без стека.

#### `cons`

```c
static node *cons(int value, node *next) {
  node *n = malloc(sizeof(node));
  if (!n) abort();
  n->value    = value;
  n->refcount = 1;
  n->next     = node_retain(next);
  return n;
}
```

Создаёт новый узел, пристёгивая его к существующей цепочке.
`node_retain(next)` сообщает хвосту о новом владельце.
`abort()` при OOM — поведение аналогично Rust-аллокатору.

#### `node_advance`

```c
static node *node_advance(node *n) {
  node *next = node_retain(n ? n->next : NULL);
  node_release(n);
  return next;
}
```

"Перешагнуть" указатель с корректной передачей владения.
Сначала `retain(next)` (+1 к счётчику следующего), потом `release(n)` (−1 к текущему).
**Порядок важен:** если сделать `release` первым, `n` может освободить `next`,
и мы потеряем указатель.

#### `cons_prepend`

```c
static void cons_prepend(node **dst, int value) {
  node *old = *dst;
  *dst = cons(value, old);
  node_release(old);
}
```

Атомарный `acc = cons(value, acc)` с корректным refcounting.
`cons` внутри делает `retain(old)`. После этого старый retain, который держал
`*dst`, больше не нужен — `node_release(old)` его снимает.
Итого refcount `old` не изменился — он просто сменил владельца.

---

### `fqueue_copy` — основа персистентности

```c
static fqueue *fqueue_copy(const fqueue *q) {
  fqueue *q2 = malloc(sizeof(fqueue));
  if (!q2) abort();
  *q2 = *q;                   // битовая копия всех полей
  node_retain(q2->front);
  node_retain(q2->rear);
  rotation_retain(&q2->rot);  // retain всех узлов ротации согласно тегу
  return q2;
}
```

`*q2 = *q` копирует все указатели — обе версии очереди теперь смотрят на **те же узлы**.
Три `retain`-вызова сообщают этим узлам о новом совладельце. Узлы не копируются —
они разделяются. Изменение одной версии не затрагивает другую.

`rotation_retain` — умный retain через `switch (tag)`: в HM_IDLE ничего не делает,
в HM_REVERSING удерживает 4 указателя, в HM_APPENDING — 2.

---

### Машинерия ротации

#### `start_rotation`

```c
static void start_rotation(fqueue *q) {
  q->rot.tag                 = HM_REVERSING;
  q->rot.reversing.old_front = node_retain(q->front); // снапшот front
  q->rot.reversing.old_rear  = q->rear;               // передаём владение
  q->rear                    = NULL;
  q->rear_len                = 0;
  q->rot.reversing.acc       = NULL;
  q->rot.reversing.rfacc     = NULL;
  q->rot.reversing.ok        = q->front_len;
}
```

`old_rear = q->rear` без `retain` — **передача владения**, а не копирование.
`q->rear = NULL` сигнализирует, что `rear` больше не держит эту ссылку.
Новые `push` во время ротации пойдут в новый пустой `rear`, не мешая ротации.

#### `rotate_step`

```c
static void rotate_step(fqueue *q) {
  if (q->rot.tag == HM_IDLE) return;

  if (q->rot.tag == HM_REVERSING) {
    cons_prepend(&q->rot.reversing.acc,   q->rot.reversing.old_rear->value);
    q->rot.reversing.old_rear = node_advance(q->rot.reversing.old_rear);

    if (q->rot.reversing.old_front != NULL) {
      cons_prepend(&q->rot.reversing.rfacc, q->rot.reversing.old_front->value);
      q->rot.reversing.old_front = node_advance(q->rot.reversing.old_front);
    }

    if (q->rot.reversing.old_rear != NULL) return;

    // old_rear исчерпан — переход в APPENDING.
    // Читаем поля ДО перезаписи union.
    node *acc = q->rot.reversing.acc, *rfacc = q->rot.reversing.rfacc;
    int   ok  = q->rot.reversing.ok;
    q->rot.tag = HM_APPENDING;
    q->rot.appending = (hm_appending){ acc, rfacc, ok };
    // fall through — первый APPENDING-шаг в том же вызове
  }

  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0) {
    finish_rotation(q);
    return;
  }
  cons_prepend(&q->rot.appending.acc, q->rot.appending.rfacc->value);
  q->rot.appending.ok--;
  q->rot.appending.rfacc = node_advance(q->rot.appending.rfacc);
  if (q->rot.appending.rfacc == NULL || q->rot.appending.ok == 0)
    finish_rotation(q);
}
```

Один вызов = один шаг. Переход REVERSING→APPENDING происходит с **явной
передачей полей** через локальные переменные — это защита от UB при перезаписи union:
читаем из `reversing`, перезаписываем `tag`, пишем в `appending`. После перехода
в том же вызове выполняется первый APPENDING-шаг (fall through без `return`).

#### `finish_rotation`

```c
static void finish_rotation(fqueue *q) {
  node_release(q->front);
  q->front     = q->rot.appending.acc;  // acc → новый front
  node_release(q->rot.appending.rfacc); // хвост rfacc (ok==0 случай) выбрасываем
  q->front_len = q->size - q->rear_len;
  q->rot.tag   = HM_IDLE;
}
```

`acc` накопил готовый `front = old_front ++ reverse(old_rear)` — передаём
ему владение. Старый `front` освобождаем. Если `ok` упал до нуля раньше
исчерпания `rfacc`, хвост `rfacc` содержит элементы, которые уже были выброшены
через `pop` — они нам не нужны, просто отпускаем.

---

### Публичный API

#### `fqueue_push`

```c
fqueue *fqueue_push(fqueue *q, int value) {
  fqueue *q2     = fqueue_copy(q);
  node *old_rear = q2->rear;
  q2->rear       = cons(value, old_rear);
  node_release(old_rear);
  q2->rear_len++;
  q2->size++;
  if (q2->rot.tag == HM_IDLE && q2->rear_len > q2->front_len)
    start_rotation(q2);
  rotate_step(q2);
  rotate_step(q2);
  return q2;
}
```

`cons(value, old_rear)` создаёт узел в голове `rear`. Старый `rear` продолжает
жить — на него по-прежнему смотрит оригинальная версия `q` (через `q->rear`).
Две версии разделяют хвост — структурный шаринг без копирования.

#### `fqueue_pop`

```c
fqueue *fqueue_pop(fqueue *q) {
  if (q->size == 0) return NULL;
  fqueue *q2     = fqueue_copy(q);
  node *old_head = q2->front;
  q2->front      = node_retain(old_head->next);
  node_release(old_head);
  q2->front_len--;
  q2->size--;
  if (q2->rot.tag == HM_REVERSING)
    q2->rot.reversing.ok = q2->rot.reversing.ok > 0 ? q2->rot.reversing.ok - 1 : 0;
  else if (q2->rot.tag == HM_APPENDING)
    q2->rot.appending.ok = q2->rot.appending.ok > 0 ? q2->rot.appending.ok - 1 : 0;
  else if (q2->rear_len > q2->front_len)
    start_rotation(q2);
  rotate_step(q2);
  rotate_step(q2);
  return q2;
}
```

`front` сдвигается на `next`. Декремент `ok` сообщает ротации, что один
"живой" элемент старого фронта выброшен — его незачем включать в новый `front`.

#### `fqueue_front`

```c
option fqueue_front(const fqueue *q) {
  if (!q->front) return none();
  return some(q->front->value);
}
```

Возвращает `option` вместо сентинела `-1` — тип явно выражает возможность
отсутствия значения. Вызывающий обязан проверить `is_some`/`is_none`.

---

## Изменения относительно оригинального задания

| Что | Зачем |
|-----|-------|
| `rotation` — tagged union | Поля `old_front`/`old_rear` недоступны в APPENDING; impossible state unrepresentable |
| `const` на `is_empty`, `size`, `front` | Читающие функции не мутируют очередь |
| `fqueue_front` → `option` | Нет магического `-1`; тип документирует «нет значения» |
| `abort()` при OOM | Явная паника вместо разыменования NULL |
| 2 × `rotate_step` | Гарантия завершения ротации до опустошения `front` |

---

## Источник

Hood R., Melville R. Real-time queue operations in pure LISP.
*Information Processing Letters*, 1981. 13(2). P. 50–54.
DOI: [10.1016/0020-0190(81)90030-2](https://doi.org/10.1016/0020-0190(81)90030-2)
