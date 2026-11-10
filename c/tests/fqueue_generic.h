// Two more instantiations of fqueue_template.h for the tests: a struct and a
// pointer element type, declared (and, in fqueue_generic.c, defined) in one
// translation unit.
#pragma once

typedef struct {
  int x, y;
} point;

#define FQUEUE_NAME pointq
#define FQUEUE_T    point
#include "fqueue_template.h"

#define FQUEUE_NAME strq
#define FQUEUE_T    const char *
#include "fqueue_template.h"
