// Карантин: освобождённый блок переиспользуется после 256 следующих
// освобождений этого потока (кольцо, затем список свободных). Блоки из общей
// арены кусками по 64 КБ на поток, свой список свободных у каждого потока
// (освобождённый чужой блок остаётся у освободившего). Остальное — системный
// malloc.
#include <malloc/malloc.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA ((size_t)8 << 30)
#define CHUNK ((size_t)64 << 10)
static char *arena;
static _Atomic size_t used;
enum { QN = 256 };
static _Thread_local struct {
  _Alignas(128) void *free_list;
  char *cur, *end;
  unsigned qh, qn;
  char pad[96];
  void *ring[QN];
} tl;
#define free_list tl.free_list
#define cur       tl.cur
#define end       tl.end

static int mine(void *p) {
  return arena && (char *)p >= arena && (char *)p < arena + ARENA;
}
static void *my_malloc(size_t n) {
  if (n != 16) return malloc(n);
  if (!arena) {
    char *a = mmap(NULL, ARENA, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    char *expected = NULL;
    if (!atomic_compare_exchange_strong((_Atomic(char *) *)&arena, &expected,
                                        a))
      munmap(a, ARENA);
  }
  if (free_list) {
    void *p = free_list;
    free_list = *(void **)p;
    return p;
  }
  if (cur == end) {
    size_t o = atomic_fetch_add(&used, CHUNK);
    if (o + CHUNK > ARENA) abort();
    cur = arena + o;
    end = cur + CHUNK;
  }
  void *p = cur;
  cur += 16;
  return p;
}
static void my_free(void *p) {
  if (!p) return;
  if (!mine(p)) {
    free(p);
    return;
  }
  if (tl.qn == QN) {
    void *o = tl.ring[tl.qh];
    tl.qh = (tl.qh + 1) % QN;
    tl.qn--;
    *(void **)o = free_list;
    free_list = o;
  }
  tl.ring[(tl.qh + tl.qn) % QN] = p;
  tl.qn++;
}
static void *my_realloc(void *p, size_t n) {
  if (!p || !mine(p)) return realloc(p, n);
  void *q = my_malloc(n);
  if (q) {
    memcpy(q, p, n < 16 ? n : 16);
    my_free(p);
  }
  return q;
}
static size_t my_malloc_size(const void *p) {
  return mine((void *)p) ? 16 : malloc_size(p);
}
__attribute__((used)) static struct {
  const void *a, *b;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)my_malloc, (const void *)malloc},
    {(const void *)my_free, (const void *)free},
    {(const void *)my_realloc, (const void *)realloc},
    {(const void *)my_malloc_size, (const void *)malloc_size}};
