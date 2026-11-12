// Подстановка jemalloc через DYLD_INSERT_LIBRARIES с прямыми вызовами (#8).
//
// libjemalloc на macOS, вставленный сам по себе, только регистрируется зоной
// по умолчанию: malloc/free идут через libmalloc, и каждый free ищет зону,
// владеющую указателем (C, fill 10^6: в 1.65 раза медленнее системного
// malloc против 1.007 при прямой линковке -ljemalloc). mimalloc перехватывает
// вызовы сам (__interpose); эта прокладка делает то же для jemalloc: вызовы
// из всех образов, кроме самой прокладки, идут прямо в jemalloc.
// malloc_size не перехватывается: системные библиотеки зовут его и на чужих
// указателях (обязан вернуть 0), а sallocx на них падает; через зоны его
// обслуживает зона jemalloc с проверкой принадлежности. На горячем пути его
// нет.
//
// Сборка brew — без префикса, функции jemalloc называются как в libSystem,
// поэтому замены вызывают нестандартный API (mallocx, dallocx, …): его имена
// с libSystem не совпадают. Ссылки на malloc/free в таблице перехвата должны
// разрешиться в libSystem — для этого -lSystem стоит перед -ljemalloc (без
// него они уходят в libjemalloc; проверка: nm -m, «from libSystem»):
//   cc -O2 -dynamiclib -I$(brew --prefix jemalloc)/include \
//       -DJEMALLOC_DYLIB='"'$(brew --prefix
//       jemalloc)/lib/libjemalloc.2.dylib'"' \
//       -o build/alloc/jemalloc_interpose.dylib
//       bench/alloc/jemalloc_interpose.c \ -lSystem -L$(brew --prefix
//       jemalloc)/lib -ljemalloc
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jemalloc/jemalloc.h>

// Быстрый путь jemalloc есть у его malloc/free, но не у mallocx/dallocx
// (C, fill 10^6: прокладка на одних mallocx/dallocx медленнее прямой
// линковки на 25%). Поэтому настоящие malloc/free jemalloc берутся по адресу
// из libjemalloc (dlsym по её handle ищет сначала в ней самой); до этого —
// ранние вызовы из инициализаторов библиотек — mallocx/dallocx: куча та же,
// чужих указателей не появляется.
static void *(*real_malloc)(size_t);
static void (*real_free)(void *);

__attribute__((constructor)) static void resolve(void) {
  void *h = dlopen(JEMALLOC_DYLIB, RTLD_NOLOAD | RTLD_LAZY);
  void *m = h ? dlsym(h, "malloc") : NULL, *f = h ? dlsym(h, "free") : NULL;
  Dl_info i;
  if (!m || !f || !dladdr(m, &i) || !strstr(i.dli_fname, "jemalloc")) abort();
  real_malloc = (void *(*)(size_t))m;
  real_free = (void (*)(void *))f;
}

static void *je_malloc_(size_t n) {
  if (__builtin_expect(real_malloc != NULL, 1)) return real_malloc(n);
  return mallocx(n ? n : 1, 0);
}

static void je_free_(void *p) {
  if (__builtin_expect(real_free != NULL, 1)) {
    real_free(p);
    return;
  }
  if (p) dallocx(p, 0);
}

static void *je_calloc_(size_t n, size_t m) {
  size_t s;
  if (__builtin_mul_overflow(n, m, &s)) return NULL;
  return mallocx(s ? s : 1, MALLOCX_ZERO);
}

static void *je_realloc_(void *p, size_t n) {
  if (!p) return je_malloc_(n);
  return rallocx(p, n ? n : 1, 0);
}

static size_t je_malloc_good_size_(size_t n) { return nallocx(n ? n : 1, 0); }

static int je_posix_memalign_(void **out, size_t align, size_t n) {
  if (align < sizeof(void *) || (align & (align - 1))) return 22; // EINVAL
  void *p = mallocx(n ? n : 1, MALLOCX_ALIGN(align));
  if (!p) return 12; // ENOMEM
  *out = p;
  return 0;
}

static void *je_aligned_alloc_(size_t align, size_t n) {
  if (!align || (align & (align - 1))) return NULL;
  return mallocx(n ? n : 1, MALLOCX_ALIGN(align));
}

static void *je_valloc_(size_t n) {
  return mallocx(n ? n : 1, MALLOCX_ALIGN((size_t)getpagesize()));
}

#define INTERPOSE(name)                                                        \
  __attribute__((used)) static const struct {                                  \
    const void *replacement, *replacee;                                        \
  } interpose_##name __attribute__((section("__DATA,__interpose"))) = {        \
      (const void *)je_##name##_, (const void *)(name)}

INTERPOSE(malloc);
INTERPOSE(free);
INTERPOSE(calloc);
INTERPOSE(realloc);
INTERPOSE(malloc_good_size);
INTERPOSE(posix_memalign);
INTERPOSE(aligned_alloc);
INTERPOSE(valloc);
