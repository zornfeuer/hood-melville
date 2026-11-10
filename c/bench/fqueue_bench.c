// Нагрузка для сравнения с Rust-реализацией (`just bench-compare`).
// Rust-двойник: rust/examples/fqueue_bench.rs —
// сценарии и контрольная сумма должны совпадать один в один.
//
// Использование: fqueue_bench <fill|mixed|persistent|branching> <N>
// Печатает контрольную сумму извлечённых значений (зависит от порядка).
// С FQ_PHASES=1 печатает в stderr длительность фаз (build/drain/destroy).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fqueue.h"

static int      phases_on;
static uint64_t phase_t0;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

// Отметка конца фазы `name` (и начала следующей); без FQ_PHASES — no-op.
static void phase(const char *name) {
  if (!phases_on) return;
  uint64_t t = now_ns();
  if (name) fprintf(stderr, "phase %s %.3f ms\n", name, (double)(t - phase_t0) / 1e6);
  phase_t0 = t;
}

static uint64_t mix(uint64_t h, int x) { return h * 31 + (uint64_t)x; }

static uint64_t mix_front(uint64_t h, const fqueue *q) {
  option o = fqueue_front(q);
  return mix(h, o.value);
}

// Версии хранятся по значению (API fqueue_*_into), как Vec<Queue> в Rust.

// Линейная история: два буфера; новая версия пишется в свободный, старая
// освобождается сразу после операции.
typedef struct {
  fqueue buf[2];
  int    cur;
} linear;

static const fqueue *cur(const linear *l) { return &l->buf[l->cur]; }

static void lin_push(linear *l, int value) {
  fqueue_push_into(&l->buf[l->cur ^ 1], &l->buf[l->cur], value);
  fqueue_release(&l->buf[l->cur]);
  l->cur ^= 1;
}

static void lin_pop(linear *l) {
  fqueue_pop_into(&l->buf[l->cur ^ 1], &l->buf[l->cur]);
  fqueue_release(&l->buf[l->cur]);
  l->cur ^= 1;
}

// N push'ей, затем опустошение.
static uint64_t run_fill(int n) {
  uint64_t h = 0;
  linear q = {.cur = 0};
  fqueue_init(&q.buf[0]);
  for (int i = 0; i < n; ++i)
    lin_push(&q, i);
  phase("build");
  while (!fqueue_is_empty(cur(&q))) {
    h = mix_front(h, cur(&q));
    lin_pop(&q);
  }
  phase("drain");
  fqueue_release(&q.buf[q.cur]);
  return h;
}

// push, push, pop до размера N, затем опустошение.
static uint64_t run_mixed(int n) {
  uint64_t h = 0;
  int i = 0;
  linear q = {.cur = 0};
  fqueue_init(&q.buf[0]);
  while (fqueue_size(cur(&q)) < n) {
    lin_push(&q, i++);
    lin_push(&q, i++);
    h = mix_front(h, cur(&q));
    lin_pop(&q);
  }
  phase("build");
  while (!fqueue_is_empty(cur(&q))) {
    h = mix_front(h, cur(&q));
    lin_pop(&q);
  }
  phase("drain");
  fqueue_release(&q.buf[q.cur]);
  return h;
}

typedef struct {
  fqueue *items;
  size_t  len;
  size_t  cap;
} versions;

// Место под следующую версию; указатели на элементы после вызова могут
// измениться (realloc), поэтому родителя берут уже после него.
static fqueue *next_slot(versions *v) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 1024;
    v->items = realloc(v->items, v->cap * sizeof *v->items);
    if (!v->items) abort();
  }
  return &v->items[v->len];
}

static const fqueue *last(const versions *v) { return &v->items[v->len - 1]; }

static void keep_push(versions *v, int value) {
  fqueue *out = next_slot(v);
  fqueue_push_into(out, last(v), value);
  v->len++;
}

static void keep_pop(versions *v) {
  fqueue *out = next_slot(v);
  fqueue_pop_into(out, last(v));
  v->len++;
}

static void destroy(versions *v) {
  for (size_t k = 0; k < v->len; ++k)
    fqueue_release(&v->items[k]);
  free(v->items);
}

// Как mixed, но все версии живы до конца и освобождаются в порядке создания.
static uint64_t run_persistent(int n) {
  uint64_t h = 0;
  int i = 0;
  versions v = {0};
  fqueue_init(next_slot(&v));
  v.len = 1;
  while (fqueue_size(last(&v)) < n) {
    keep_push(&v, i++);
    keep_push(&v, i++);
    h = mix_front(h, last(&v));
    keep_pop(&v);
  }
  phase("build");
  while (!fqueue_is_empty(last(&v))) {
    h = mix_front(h, last(&v));
    keep_pop(&v);
  }
  phase("drain");
  destroy(&v);
  phase("destroy");
  return h;
}

// Ветвящаяся история: BRANCHES активных веток. На каждом шаге случайная ветка
// порождает потомка от своей вершины (вершина заменяется потомком, прежняя
// версия остаётся живой). Раз в 64 шага ветка вместо этого ответвляется от
// случайной версии из всей истории. Голова родителя читается (повторное
// чтение старых версий); pop с вероятностью 1/3, иначе push. Выбор задаёт
// xorshift64 — одинаково в C и Rust. Все версии живы до конца и
// освобождаются в порядке создания.
static uint64_t run_branching(int n) {
  enum { BRANCHES = 8 };
  uint64_t x = 0x9E3779B97F4A7C15u, h = 0;
  size_t heads[BRANCHES] = {0};
  versions v = {0};
  fqueue_init(next_slot(&v));
  v.len = 1;
  for (int k = 0; k < n; ++k) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    size_t slot = (x >> 3) % BRANCHES;
    size_t idx = (x & 63) == 0 ? (x >> 12) % v.len : heads[slot];
    fqueue *child = next_slot(&v);
    const fqueue *parent = &v.items[idx];
    if (fqueue_is_empty(parent)) {
      fqueue_push_into(child, parent, k);
    } else {
      h = mix_front(h, parent);
      if ((x >> 40) % 3 == 0)
        fqueue_pop_into(child, parent);
      else
        fqueue_push_into(child, parent, k);
    }
    heads[slot] = v.len++;
  }
  phase("build");
  destroy(&v);
  phase("destroy");
  return h;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <fill|mixed|persistent|branching> <N>\n", argv[0]);
    return 2;
  }
  int n = atoi(argv[2]);
  const char *ph = getenv("FQ_PHASES");
  phases_on = ph && strcmp(ph, "1") == 0;
  phase(NULL);
  uint64_t h;
  if (strcmp(argv[1], "fill") == 0)
    h = run_fill(n);
  else if (strcmp(argv[1], "mixed") == 0)
    h = run_mixed(n);
  else if (strcmp(argv[1], "persistent") == 0)
    h = run_persistent(n);
  else if (strcmp(argv[1], "branching") == 0)
    h = run_branching(n);
  else {
    fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return 2;
  }
  printf("%llu\n", (unsigned long long)h);
  return 0;
}
