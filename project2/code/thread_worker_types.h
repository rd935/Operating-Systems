#ifndef TW_TYPES_H
#define TW_TYPES_H

#define USE_THREAD 1

#ifndef TIMESLICE
#define TIMESLICE 5
#endif

#ifdef MLFQ
#define NUM_QS 4
#else
#define NUM_QS 1
#endif

#define READY 0
#define SCHEDULED 1
#define BLOCKED 2
#define STOPPED 4
#define JOINED	8

#include <err.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

typedef unsigned int worker_t;

typedef struct TCB {

    /* add important states in a thread control block */
    // thread Id
    // thread status
    // thread context
    // thread stack
    // thread priority
    // And more ...

    struct tcb_list *join;
    void *val;
    ucontext_t tcb_context;
    worker_t ID;
    unsigned int stat;
    
} tcb;

struct tcb_list {
    tcb *thr;
    struct tcb_list *n;
    struct tcb_list *p;
};

struct scheduler {
    tcb *run;
    struct tcb_list *q[NUM_QS];
    ucontext_t sched_context;
    unsigned int prior;
};


#endif
