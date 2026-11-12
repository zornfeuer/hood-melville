// R читателей без писателя: каждый ITERS раз обходит копию одного снимка.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "fqueue.h"
enum { SNAP_SIZE = 4096, PAIRS = 300000, ITERS = 300 };
static fqueue_sync snap;
static void *reader(void *arg) {
  uint64_t sum = 0;
  for (int i = 0; i < ITERS; ++i) {
    fqueue_sync_iter it; int v;
    fqueue_sync_iter_init(&it, &snap);
    while (fqueue_sync_iter_next(&it, &v) == FQUEUE_OK) sum = sum * 31 + (uint64_t)v;
    fqueue_sync_iter_destroy(&it);
  }
  *(uint64_t *)arg = sum;
  return NULL;
}
int main(int argc, char **argv) {
  int r = atoi(argv[1]);
  fqueue_sync q; fqueue_sync_init(&q);
  for (int i = 0; i < SNAP_SIZE; ++i) if (fqueue_sync_push_in_place(&q, i)) abort();
  int v;
  for (int k = 0; k < PAIRS; ++k) { if (fqueue_sync_push_in_place(&q, SNAP_SIZE + k) || fqueue_sync_pop_in_place(&q, &v)) abort(); }
  fqueue_sync_clone(&snap, &q);
  pthread_t th[16]; uint64_t res[16];
  struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < r; ++i) pthread_create(&th[i], NULL, reader, &res[i]);
  for (int i = 0; i < r; ++i) pthread_join(th[i], NULL);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
  printf("%llu %.0f\n", (unsigned long long)res[0], (double)r * ITERS * SNAP_SIZE / ns * 1e3);
}
