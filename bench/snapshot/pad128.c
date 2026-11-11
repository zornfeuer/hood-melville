// 16-байтовые выделения — каждый на своей 128-байтовой строке (системный aligned_alloc).
// PASS=1 — только перехват, без выравнивания (контроль).
#include <stdlib.h>
static int pass;
__attribute__((constructor)) static void init(void) { pass = getenv("PASS") != NULL; }
static void *my_malloc(size_t n) { return n == 16 && !pass ? aligned_alloc(128, 128) : malloc(n); }
__attribute__((used)) static struct { const void *a, *b; } interposers[] __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)my_malloc, (const void *)malloc}};
