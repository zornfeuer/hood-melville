#ifndef AISD_LABS_2025_OPTION_H
#define AISD_LABS_2025_OPTION_H

#include <assert.h>
#include <stddef.h>

typedef enum option_tag { SOME, NONE } option_tag;

typedef struct option {
  option_tag tag;
  int value;
} option;

static inline void make_none(option *op) { op->tag = NONE; }

static inline void make_some(option *op, int value) {
  op->tag = SOME;
  op->value = value;
}

static inline option some(int value) { return (option){.tag = SOME, .value = value}; }
static inline option none(void)      { return (option){.tag = NONE}; }

static inline int is_some(const option *op) { return op->tag == SOME; }

static inline int is_none(const option *op) { return op->tag == NONE; }

static inline int unwrap_or(option *op, int value) {
  int result;
  switch (op->tag) {
  case SOME:
    result = op->value;
    make_none(op);
    break;
  case NONE:
    result = value;
  }
  return result;
}

static inline int unwrap(option *op) {
  assert(op != NULL);
  assert(is_some(op));
  int result = op->value;
  make_none(op);
  return result;
}

#endif // AISD_LABS_2025_OPTION_H
