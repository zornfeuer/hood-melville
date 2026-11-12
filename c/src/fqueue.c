#include "fqueue.h"

#define FQUEUE_NAME fqueue
#define FQUEUE_T    int
#define FQUEUE_IMPLEMENTATION
#include "fqueue_template.h"

#define FQUEUE_NAME fqueue_sync
#define FQUEUE_T    int
#define FQUEUE_ATOMIC
#define FQUEUE_IMPLEMENTATION
#include "fqueue_template.h"

#define FQUEUE_NAME fqueue_pooled
#define FQUEUE_T    int
#define FQUEUE_NODE_POOL
#define FQUEUE_IMPLEMENTATION
#include "fqueue_template.h"

#define FQUEUE_NAME fqueue_pooled_sync
#define FQUEUE_T    int
#define FQUEUE_ATOMIC
#define FQUEUE_NODE_POOL
#define FQUEUE_IMPLEMENTATION
#include "fqueue_template.h"
