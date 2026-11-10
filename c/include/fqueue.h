#ifndef HOOD_MELVILLE_FQUEUE_H
#define HOOD_MELVILLE_FQUEUE_H

// The queue of fqueue_template.h instantiated for int: the type `fqueue` and
// fqueue_init, fqueue_push, fqueue_pop, ... See fqueue_template.h for the
// documentation; the definitions are in src/fqueue.c.

// The library is built with hidden visibility, so that a shared libfqueue
// exports exactly these functions.
#if defined(__GNUC__) || defined(__clang__)
#define FQUEUE_API __attribute__((visibility("default")))
#endif

#define FQUEUE_NAME fqueue
#define FQUEUE_T    int
#include "fqueue_template.h"

#endif // HOOD_MELVILLE_FQUEUE_H
