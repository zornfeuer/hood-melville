// Параллельная нагрузка на fqueue_sync (#6). Rust-двойник —
// rust/examples/fqueue_par.rs (там описание сценариев), Zig —
// zig/bench/fqueue_par.zig; сценарии и контрольные суммы совпадают.
//
// Использование:
//   fqueue_par share <pool|threads> <share> <P>
//     pool — OpenMP, `parallel for schedule(dynamic)` (только в сборке с
//     -fopenmp); threads — pthreads, задачи разбиты между P потоками заранее.
//     Печатает контрольную сумму и время параллельной фазы, нс.
//   fqueue_par snapshot <R>
//     писатель и R читателей снимков (pthreads). Печатает контрольную сумму
//     писателя, его время (нс), число прочитанных снимков и элементов.

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "fqueue.h"

// С -DFQUEUE_BENCH_POOL — то же на fqueue_pooled_sync (пул узлов,
// docs/pool.md).
#ifdef FQUEUE_BENCH_POOL
#define fqueue_sync               fqueue_pooled_sync
#define fqueue_sync_clone         fqueue_pooled_sync_clone
#define fqueue_sync_destroy       fqueue_pooled_sync_destroy
#define fqueue_sync_front         fqueue_pooled_sync_front
#define fqueue_sync_init          fqueue_pooled_sync_init
#define fqueue_sync_iter_destroy  fqueue_pooled_sync_iter_destroy
#define fqueue_sync_iter_init     fqueue_pooled_sync_iter_init
#define fqueue_sync_iter_next     fqueue_pooled_sync_iter_next
#define fqueue_sync_iter          fqueue_pooled_sync_iter
#define fqueue_sync_pop_in_place  fqueue_pooled_sync_pop_in_place
#define fqueue_sync_pop           fqueue_pooled_sync_pop
#define fqueue_sync_push_in_place fqueue_pooled_sync_push_in_place
#define fqueue_sync_push          fqueue_pooled_sync_push
#endif

enum { TASKS = 256, OPS = 40000, SHARED = 64 };
enum { SNAP_SIZE = 4096, SNAP_PAIRS = 1000000, PERIOD = 512 };

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t xorshift(uint64_t x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

static uint64_t mix(uint64_t h, int x) { return h * 31 + (uint64_t)x; }

__attribute__((cold, noinline, noreturn)) static void die(fqueue_status s) {
  fprintf(stderr, "fqueue: %s\n", fqueue_strerror(s));
  exit(1);
}

static void check(fqueue_status s) {
  if (s != FQUEUE_OK) die(s);
}

static fqueue_sync shared[SHARED];

static void build_shared(void) {
  fqueue_sync q, next;
  fqueue_sync_init(&q);
  for (int j = 0; j < SHARED * 24; ++j) {
    if (j % 24 == 0) fqueue_sync_clone(&shared[j / 24], &q);
    if (j % 3 == 2)
      check(fqueue_sync_pop(&next, &q));
    else
      check(fqueue_sync_push(&next, &q, j));
    fqueue_sync_destroy(&q);
    q = next;
  }
  fqueue_sync_destroy(&q);
}

static uint64_t run_task(uint64_t share, uint64_t t) {
  uint64_t x = 0x9E3779B97F4A7C15u ^ (t + 1) * 0xBF58476D1CE4E5B9u;
  uint64_t h = 0;
  fqueue_sync q;
  fqueue_sync_init(&q);
  for (int k = 0; k < OPS; ++k) {
    x = xorshift(x);
    const fqueue_sync *parent =
        x % 100 < share ? &shared[(x >> 8) % SHARED] : &q;
    fqueue_sync child;
    int v;
    if (!fqueue_sync_front(parent, &v)) {
      check(fqueue_sync_push(&child, parent, k));
    } else {
      h = mix(h, v);
      if (((x >> 40) & 1) == 0)
        check(fqueue_sync_pop(&child, parent));
      else
        check(fqueue_sync_push(&child, parent, k));
    }
    fqueue_sync_destroy(&q);
    q = child;
  }
  fqueue_sync_destroy(&q);
  return h;
}

// Без OpenMP только сообщает об ошибке и *elapsed не пишет.
// NOLINTNEXTLINE(readability-non-const-parameter)
static uint64_t run_pool(uint64_t share, int p, uint64_t *elapsed) {
#ifdef _OPENMP
  // Потоки OpenMP создаются при первой параллельной области — до замера,
  // как пул rayon.
#pragma omp parallel num_threads(p)
  {
  }
  uint64_t t0 = now_ns(), h = 0;
#pragma omp parallel for schedule(dynamic) num_threads(p) reduction(+ : h)
  for (int t = 0; t < TASKS; ++t) h += run_task(share, (uint64_t)t);
  *elapsed = now_ns() - t0;
  return h;
#else
  (void)share, (void)p, (void)elapsed;
  fprintf(stderr, "fqueue_par: built without OpenMP, pool mode unavailable\n");
  exit(2);
#endif
}

typedef struct {
  uint64_t share;
  int first, step;
  uint64_t h;
} worker;

static void *run_worker(void *arg) {
  worker *w = arg;
  w->h = 0;
  for (int t = w->first; t < TASKS; t += w->step)
    w->h += run_task(w->share, (uint64_t)t);
  return NULL;
}

static uint64_t run_threads(uint64_t share, int p, uint64_t *elapsed) {
  pthread_t *threads = malloc((size_t)p * sizeof *threads);
  worker *workers = malloc((size_t)p * sizeof *workers);
  if (!threads || !workers) abort();
  uint64_t t0 = now_ns(), h = 0;
  for (int i = 0; i < p; ++i) {
    workers[i] = (worker){.share = share, .first = i, .step = p};
    if (pthread_create(&threads[i], NULL, run_worker, &workers[i]) != 0)
      abort();
  }
  for (int i = 0; i < p; ++i) {
    pthread_join(threads[i], NULL);
    h += workers[i].h;
  }
  *elapsed = now_ns() - t0;
  free(threads);
  free(workers);
  return h;
}

static pthread_mutex_t slot_mutex = PTHREAD_MUTEX_INITIALIZER;
static fqueue_sync slot;
static atomic_bool done;

typedef struct {
  uint64_t snapshots, elements, sum;
} reader;

static void *run_reader(void *arg) {
  uint64_t snapshots = 0, elements = 0, sum = 0;
  while (!atomic_load_explicit(&done, memory_order_relaxed)) {
    // Итератор обходит копию снимка; копия снимается под мьютексом.
    fqueue_sync_iter it;
    pthread_mutex_lock(&slot_mutex);
    fqueue_sync_iter_init(&it, &slot);
    pthread_mutex_unlock(&slot_mutex);
    int v;
    fqueue_status s;
    while ((s = fqueue_sync_iter_next(&it, &v)) == FQUEUE_OK) {
      sum = mix(sum, v);
      elements++;
    }
    if (s != FQUEUE_EMPTY) die(s);
    fqueue_sync_iter_destroy(&it);
    snapshots++;
  }
  *(reader *)arg = (reader){snapshots, elements, sum};
  return NULL;
}

static uint64_t run_snapshot(int readers, uint64_t *elapsed,
                             uint64_t *snapshots, uint64_t *elements) {
  fqueue_sync q;
  fqueue_sync_init(&q);
  for (int i = 0; i < SNAP_SIZE; ++i) check(fqueue_sync_push_in_place(&q, i));
  fqueue_sync_clone(&slot, &q);
  pthread_t *threads = malloc((size_t)readers * sizeof *threads);
  reader *results = malloc((size_t)readers * sizeof *results);
  if (readers && (!threads || !results)) abort();
  for (int i = 0; i < readers; ++i)
    if (pthread_create(&threads[i], NULL, run_reader, &results[i]) != 0)
      abort();
  uint64_t t0 = now_ns(), h = 0;
  for (int k = 0; k < SNAP_PAIRS; ++k) {
    check(fqueue_sync_push_in_place(&q, SNAP_SIZE + k));
    int v = 0;
    check(fqueue_sync_pop_in_place(&q, &v));
    h = mix(h, v);
    if ((k + 1) % PERIOD == 0) {
      // Копия снимается и прежний снимок освобождается вне мьютекса.
      fqueue_sync snapshot, old;
      fqueue_sync_clone(&snapshot, &q);
      pthread_mutex_lock(&slot_mutex);
      old = slot;
      slot = snapshot;
      pthread_mutex_unlock(&slot_mutex);
      fqueue_sync_destroy(&old);
    }
  }
  *elapsed = now_ns() - t0;
  atomic_store_explicit(&done, true, memory_order_relaxed);
  *snapshots = *elements = 0;
  for (int i = 0; i < readers; ++i) {
    pthread_join(threads[i], NULL);
    *snapshots += results[i].snapshots;
    *elements += results[i].elements;
  }
  free(threads);
  free(results);
  fqueue_sync_destroy(&slot);
  fqueue_sync_destroy(&q);
  return h;
}

// Целое из аргумента командной строки: -1, если это не число в [0, INT_MAX].
static int parse_arg(const char *s) {
  char *end;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno || end == s || *end || v < 0 || v > INT_MAX) return -1;
  return (int)v;
}

static int usage(void) {
  fprintf(stderr,
          "usage: fqueue_par share <pool|threads> <share 0..100> <threads>\n"
          "       fqueue_par snapshot <readers>\n");
  return 2;
}

int main(int argc, char **argv) {
  if (argc == 5 && strcmp(argv[1], "share") == 0) {
    int share = parse_arg(argv[3]), p = parse_arg(argv[4]);
    if (share < 0 || share > 100 || p <= 0) return usage();
    build_shared();
    uint64_t h, elapsed;
    if (strcmp(argv[2], "pool") == 0)
      h = run_pool((uint64_t)share, p, &elapsed);
    else if (strcmp(argv[2], "threads") == 0)
      h = run_threads((uint64_t)share, p, &elapsed);
    else
      return usage();
    printf("%llu %llu\n", (unsigned long long)h, (unsigned long long)elapsed);
    for (int i = 0; i < SHARED; ++i) fqueue_sync_destroy(&shared[i]);
    return 0;
  }
  if (argc == 3 && strcmp(argv[1], "snapshot") == 0) {
    int readers = parse_arg(argv[2]);
    if (readers < 0) return usage();
    uint64_t elapsed, snapshots, elements;
    uint64_t h = run_snapshot(readers, &elapsed, &snapshots, &elements);
    printf("%llu %llu %llu %llu\n", (unsigned long long)h,
           (unsigned long long)elapsed, (unsigned long long)snapshots,
           (unsigned long long)elements);
    return 0;
  }
  return usage();
}
