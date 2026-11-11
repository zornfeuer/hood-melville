// LIFO, плюс счёт освобождений блоков, выделенных другим потоком (по последнему выделившему);
// итог печатается в stderr при выходе.
// 16-байтовые блоки из общей арены кусками по 64 КБ на поток, свой список
// свободных у каждого потока (освобождённый чужой блок остаётся у
// освободившего). Остальное — системный malloc.
#include <malloc/malloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA ((size_t)8 << 30)
#define CHUNK ((size_t)64 << 10)
static char *arena;
static _Atomic size_t used;
static _Thread_local struct { _Alignas(128) void *free_list; char *cur, *end; unsigned id; char pad[100]; } tl;
static _Atomic unsigned nthreads; static unsigned char *owner;
static _Atomic unsigned long long nlocal, nremote, nmalloc;
static unsigned myid(void) { if (!tl.id) tl.id = ++nthreads; return tl.id; }
__attribute__((destructor)) static void report(void) {
  fprintf(stderr, "frees local %llu remote %llu (%.1f%%), mallocs %llu\n", (unsigned long long)nlocal, (unsigned long long)nremote,
          100.0 * nremote / (nlocal + nremote + 1e-9), (unsigned long long)nmalloc);
}
#define free_list tl.free_list
#define cur tl.cur
#define end tl.end

static int mine(void *p) { return arena && (char *)p >= arena && (char *)p < arena + ARENA; }
static void *my_malloc(size_t n) {
  if (n != 16) return malloc(n);
  if (!arena) {
    char *a = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    unsigned char *ow = mmap(NULL, ARENA / 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    char *expected = NULL;
    if (!atomic_compare_exchange_strong((_Atomic(char *) *)&arena, &expected, a)) { munmap(a, ARENA); munmap(ow, ARENA / 16); }
    else owner = ow;
  }
  atomic_fetch_add_explicit(&nmalloc, 1, memory_order_relaxed);
  if (free_list) { void *p = free_list; free_list = *(void **)p; owner[((char *)p - arena) / 16] = myid(); return p; }
  if (cur == end) { size_t o = atomic_fetch_add(&used, CHUNK); if (o + CHUNK > ARENA) abort(); cur = arena + o; end = cur + CHUNK; }
  void *p = cur; cur += 16; owner[((char *)p - arena) / 16] = myid(); return p;
}
static void my_free(void *p) {
  if (!p) return;
  if (!mine(p)) { free(p); return; }
  if (owner[((char *)p - arena) / 16] == myid()) atomic_fetch_add_explicit(&nlocal, 1, memory_order_relaxed);
  else atomic_fetch_add_explicit(&nremote, 1, memory_order_relaxed);
  *(void **)p = free_list; free_list = p;
}
static void *my_realloc(void *p, size_t n) {
  if (!p || !mine(p)) return realloc(p, n);
  void *q = my_malloc(n); if (q) { memcpy(q, p, n < 16 ? n : 16); my_free(p); } return q;
}
static size_t my_malloc_size(const void *p) { return mine((void *)p) ? 16 : malloc_size(p); }
__attribute__((used)) static struct { const void *a, *b; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)my_malloc, (const void *)malloc}, {(const void *)my_free, (const void *)free},
    {(const void *)my_realloc, (const void *)realloc}, {(const void *)my_malloc_size, (const void *)malloc_size}};
