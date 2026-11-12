// Счёт обращений к системному аллокатору (#14): malloc, calloc, realloc,
// free, posix_memalign, aligned_alloc — через DYLD_INSERT_LIBRARIES; итог при
// выходе в stderr одной строкой «alloc-calls malloc=… free=… … bytes=…».
// Для вариантов на malloc это выделения узлов, для пула (docs/pool.md) —
// плиты и кэши потоков. Только для подсчёта: на такты не годится.
//   cc -O2 -dynamiclib -o build/alloc/count.dylib bench/alloc/count.c
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static _Atomic unsigned long long n_malloc, n_calloc, n_realloc, n_free,
    n_memalign, bytes;

static void *my_malloc(size_t n) {
  atomic_fetch_add_explicit(&n_malloc, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&bytes, n, memory_order_relaxed);
  return malloc(n);
}
static void *my_calloc(size_t n, size_t m) {
  atomic_fetch_add_explicit(&n_calloc, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&bytes, n * m, memory_order_relaxed);
  return calloc(n, m);
}
static void *my_realloc(void *p, size_t n) {
  atomic_fetch_add_explicit(&n_realloc, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&bytes, n, memory_order_relaxed);
  return realloc(p, n);
}
static void my_free(void *p) {
  if (p) atomic_fetch_add_explicit(&n_free, 1, memory_order_relaxed);
  free(p);
}
static int my_posix_memalign(void **out, size_t a, size_t n) {
  atomic_fetch_add_explicit(&n_memalign, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&bytes, n, memory_order_relaxed);
  return posix_memalign(out, a, n);
}
static void *my_aligned_alloc(size_t a, size_t n) {
  atomic_fetch_add_explicit(&n_memalign, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&bytes, n, memory_order_relaxed);
  return aligned_alloc(a, n);
}

__attribute__((destructor)) static void report(void) {
  fprintf(stderr,
          "alloc-calls malloc=%llu calloc=%llu realloc=%llu memalign=%llu "
          "free=%llu bytes=%llu\n",
          (unsigned long long)n_malloc, (unsigned long long)n_calloc,
          (unsigned long long)n_realloc, (unsigned long long)n_memalign,
          (unsigned long long)n_free, (unsigned long long)bytes);
}

#define INTERPOSE(name)                                                        \
  __attribute__((used)) static const struct {                                  \
    const void *replacement, *replacee;                                        \
  } interpose_##name __attribute__((section("__DATA,__interpose"))) = {        \
      (const void *)my_##name, (const void *)(name)}

INTERPOSE(malloc);
INTERPOSE(calloc);
INTERPOSE(realloc);
INTERPOSE(free);
INTERPOSE(posix_memalign);
INTERPOSE(aligned_alloc);
