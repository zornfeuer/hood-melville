#include "fqueue_generic.h"

#define FQUEUE_NAME pointq
#define FQUEUE_T    point
#define FQUEUE_IMPLEMENTATION
#include "fqueue_template.h"

#define FQUEUE_NAME strq
#define FQUEUE_T    const char *
#define FQUEUE_IMPLEMENTATION
#include "fqueue_template.h"
