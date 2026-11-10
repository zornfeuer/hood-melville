// The int queue from the library and an own instantiation of the template
// for strings, in one file.
#include <stdio.h>
#include <stdlib.h>

#include <fqueue.h>

#define FQUEUE_NAME strq
#define FQUEUE_T    const char *
#include <fqueue_template.h>

#define FQUEUE_NAME strq
#define FQUEUE_T    const char *
#define FQUEUE_IMPLEMENTATION
#include <fqueue_template.h>

static void check(fqueue_status s) {
  if (s == FQUEUE_OK) return;
  fprintf(stderr, "fqueue: %s\n", fqueue_strerror(s));
  exit(1);
}

int main(void) {
  fqueue a, b;
  fqueue_init(&a);
  check(fqueue_push(&b, &a, 42));
  int x = 0;
  fqueue_front(&b, &x);
  printf("int front: %d, old version size: %zu\n", x, fqueue_size(&a));
  fqueue_destroy(&a);
  fqueue_destroy(&b);

  strq s, t;
  strq_init(&s);
  const char *words[] = {"hood", "melville", "1981"};
  for (int i = 0; i < 3; ++i) {
    check(strq_push(&t, &s, words[i]));
    strq_destroy(&s);
    s = t;
  }
  strq_iter it;
  check(strq_iter_init(&it, &s));
  const char *w;
  fqueue_status st;
  while ((st = strq_iter_next(&it, &w)) == FQUEUE_OK)
    printf("%s ", w);
  strq_iter_destroy(&it);
  check(st == FQUEUE_EMPTY ? FQUEUE_OK : st);
  printf("\n");
  strq_destroy(&s);
  return 0;
}
