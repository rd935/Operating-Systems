// File:	thread-worker.c

// List all group member's name:
/* Ritwika Das - rd935
/* Siya Vyas - sv694
*/
// username of iLab: 
// iLab Server: ilab1


#include "thread-worker.h"
#include "thread_worker_types.h"

#define STACK_SIZE 16 * 1024
#define QUANTUM 10 * 1000


// INITIALIZE ALL YOUR OTHER VARIABLES HERE
int init_scheduler_done = 0;
static struct scheduler *sched;
static uint open_tid, open_mutid;
static volatile unsigned int time_elapsed;


static void enable(void);
static void disable(void);
static void enqueue(tcb *thread, struct tcb_list **q);
static void sched_rr(struct tcb_list **q);
static void init_scheduler(void);
static void start_thread(void *(*function)(void *), void *arg);
static tcb *fetch(worker_t twait_id);
static void add_thread(struct tcb_list **join, tcb *waiter);
static void release(struct tcb_list *list);


/* create a new thread */
int worker_create(worker_t *thread, pthread_attr_t *attr,
                  void *(*function)(void *), void *arg)
{
    // - create Thread Control Block (TCB)
    // - create and initialize the context of this worker thread
    // - allocate space of stack for this thread to run
    // after everything is set, push this thread into run queue and
    // - make it ready for the execution.

    tcb *tcb;
    void *stack;

	 if (!sched) {

		init_scheduler(); 
 		enable();
    }

	if (attr){
        warnx("Passing thread attribute to %s, not implemented.", __func__);
    }

    if(!thread || !function){
        return -1;
    }


    tcb = malloc(sizeof(*tcb));
    stack = malloc(SIGSTKSZ);

    if (!tcb || !stack){
        err(-1, "%s: Error allocating %zu bytes.", __func__, !stack ? SIGSTKSZ : sizeof(*tcb));
    }

    if(getcontext(&tcb -> tcb_context) < 0) {
        warn("Error getting context for new thread");
        free(tcb);
        free(stack);
        return -1;
    }

	tcb->tcb_context.uc_stack.ss_flags = 0;
    tcb->tcb_context.uc_stack.ss_size = SIGSTKSZ;
    tcb->tcb_context.uc_link = &sched->run->tcb_context;
    tcb->tcb_context.uc_stack.ss_sp = stack;

    makecontext(&tcb->tcb_context, (void (*)(void)) start_thread, 2, function, arg);

    tcb->ID = open_tid;
    *thread = open_tid++;
    tcb->stat = READY;
    tcb->join = NULL;
    enqueue(tcb, &sched->q[0]);

    return 0;
};

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield()
{
    // - change worker thread's state from Running to Ready
    // - save context of this thread to its thread control block
    // - switch from thread context to scheduler context

	disable();
    return swapcontext(&sched->run->tcb_context, &sched->sched_context);
    return 0;

};

/* terminate a thread */
void worker_exit(void *value_ptr)
{
    // - if value_ptr is provided, save return value
    // - de-allocate any dynamic memory created when starting this thread (could be done here or elsewhere)

    tcb *call = sched->run;

    disable();
    call->val = value_ptr;
    call->stat = STOPPED;
    release(call->join);
    call->join = NULL;
    if (setcontext(&sched->sched_context) < 0){
        err(-1, "%s: Error", __func__);
    }
};

/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr)
{

    // - wait for a specific thread to terminate
    // - if value_ptr is provided, retrieve return value from joining thread
    // - de-allocate any dynamic memory created by the joining thread

    tcb *join2, *call = sched->run;

    if (thread >= open_tid){
        return -1;
    }

    join2 = fetch(thread);
    if (!join2){
        return -1;
    }

    if (join2->stat != STOPPED){
        disable();
        add_thread(&join2->join, call);
        call->stat = BLOCKED;
        swapcontext(&call->tcb_context, &sched->sched_context);
    }

    if (value_ptr) {
        *value_ptr = join2->val;
    }

    join2->stat = JOINED;

    return 0;

};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex,
                      const pthread_mutexattr_t *mutexattr)
{
    //- initialize data structures for this mutex

    if (mutexattr){
        warnx("%s given attribute argument. Not implemented, ignoring.", __func__);
    }

	if (!mutex) {
        return -1;
    }

	mutex->list = NULL;
    mutex->owner = NULL;	
    mutex->ID = open_mutid++;
    
    return 0;

};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex)
{

    // - use the built-in test-and-set atomic function to test the mutex
    // - if the mutex is acquired successfully, enter the critical section
    // - if acquiring mutex fails, push current thread into block list and
    // context switch to the scheduler thread

    tcb *call = sched->run;

	if (!mutex)
		return -1;


	while ((tcb *volatile) mutex->owner) {
		struct tcb_list *waiting;

		disable();
		call->stat = BLOCKED;
		waiting = malloc(sizeof(*waiting));

		if (!waiting) {
			err(-1, "%s: Error allocating %zu bytes.", __func__, sizeof(*waiting));
        }

		waiting->thr = call;
		waiting->n = mutex->list;
		mutex->list = waiting;
		swapcontext(&sched->run->tcb_context, &sched->sched_context);
	}

	mutex->owner = call;

    return 0;

};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex)
{
    // - release mutex and make it available again.
    // - put one or more threads in block list to run queue
    // so that they could compete for mutex later.

    sigset_t y;

	if (!mutex || mutex->owner != sched->run) {
        return -1;
    }
		
	sigemptyset(&y);
	sigaddset(&y, SIGPROF);
	sigprocmask(SIG_BLOCK, &y, NULL);
	mutex->owner = NULL;
	release(mutex->list);
	mutex->list = NULL;
	sigprocmask(SIG_UNBLOCK, &y, NULL);

    return 0;
};

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex)
{
    // - make sure mutex is not being used
    // - de-allocate dynamic memory created in worker_mutex_init

    if (!mutex) {
		return -1;
    }

	if (mutex->owner) {
		release(mutex->list);
    }

	mutex->owner = NULL;

    return 0;
};


static tcb *fetch(worker_t twait_id)
{
	int i;

	for (i = 0; i < NUM_QS; i++) {
		struct tcb_list *par, *h = sched->q[i];
		par = h;
		while (h) {
		    	par = par->n;
			if (par->thr->ID == twait_id)
				return par->thr;
		    	if (par == h)
			    	break;
		}
	}
	return NULL;
};

static void add_thread(struct tcb_list **join, tcb *waiter)
{
	struct tcb_list *new;

	new = malloc(sizeof(*new));
	if (!new)
		err(-1, "%s: Error allocating %zu bytes",
			__func__, sizeof(*new));
	new->thr = waiter;
	new->n = *join;
	*join = new;
};


static void release(struct tcb_list *list)
{
	struct tcb_list *temp;

	for (; list; list = temp) {
		temp = list->n;
		list->thr->stat = READY;
		free(list);
	}
};

static void list_add(struct tcb_list *new, struct tcb_list *current,
			  				struct tcb_list *previous)
{
	new->n = current;
	new->p = previous;
	current->p = new;
	previous->n = new;
};


static void list_del(struct tcb_list *previous, struct tcb_list *next)
{
	previous->n = next;
	next->p = previous;
};

static void enable(void)
{
	struct itimerval timer = {
		.it_interval = {0, 0},
		.it_value = {0, TIMESLICE * 1000}
	};

	setitimer(ITIMER_PROF, &timer, NULL);
};

static void disable(void)
{
	struct itimerval timer = {
		.it_interval = {0, 0},
		.it_value = {0, 0}
	};

	setitimer(ITIMER_PROF, &timer, NULL);
};

static void enqueue(tcb *thread, struct tcb_list **q)
{
	struct tcb_list **t = q;
	struct tcb_list *node;

	node = malloc(sizeof(*node));
	if (!node)
		err(-1, "%s: Error allocating %zu bytes.",
			__func__, sizeof(*node));
	node->thr = thread;
	node->n = node;
	node->p = node;

	if (!*t)
		*t = node;
	list_add(node, *t, (*t)->p);
};

static tcb *dequeue(struct tcb_list **q)
{
	struct tcb_list **c = q;
	struct tcb_list *h = *c;
	tcb *job;

	if (!*c)
		exit(-1);

	while ((*c)->thr->stat != READY) {
		if ((*c)->thr->stat == JOINED) {
			struct tcb_list *freeing = *c;

			list_del(freeing->p, freeing->n);
			*c = (*c)->n;

			free(freeing->thr->tcb_context.uc_stack.ss_sp);
			free(freeing->thr);
			free(freeing);

			if (freeing == *c) {
				*q = NULL;
				return NULL;
			}
			if (freeing == h) {
				h = (*c);
				continue;
			}

		} else {
			*c = (*c)->n;
		}

		if (h == *c)
			return NULL;
	}

	job = (*c)->thr;
	*c = (*c)->n;

	return job;
};

static void schedule()
{
// - every time a timer interrupt occurs, your worker thread library
// should be contexted switched from a thread context to this
// schedule() function

        for (;;) {
    #ifndef MLFQ
                sched_rr(sched->q);
    #else
                sched_mlfq();
    #endif
            }
};

static void sched_rr()
{
    // - your own implementation of RR
    // (feel free to modify arguments and return types)

    tcb *next_thr, *running = sched->run;

	if (running->stat == SCHEDULED) {
		running->stat = READY;
    }

	next_thr = dequeue(sched->q);

	if (!next_thr) {
		return;
    }

	next_thr->stat = SCHEDULED;
	sched->run = next_thr;
	time_elapsed = 0;

	enable();

	if (swapcontext(&sched->sched_context, &next_thr->tcb_context) < 0) {
		write(STDOUT_FILENO, "Error swapping context!\n", 24);
		exit(-1);
	}
  
};

static void sched_mlfq()
{
    // - your own implementation of MLFQ
    // (feel free to modify arguments and return types)

};

static void scheduler_preempt(int signum)
{
	disable();
	time_elapsed = 1;
	if (swapcontext(&sched->run->tcb_context, &sched->sched_context) < 0) {
		write(STDOUT_FILENO, "Preemption swap to scheduler failed.\n", 37);
		exit(-1);
	}
};

static void init_scheduler(void)
{
	tcb *init_thr;
	void *sched_stack;
	struct sigaction sig_act;
	int i;

	sched = malloc(sizeof(*sched));
	init_thr = malloc(sizeof(*init_thr));
	sched_stack = malloc(SIGSTKSZ);

	if (getcontext(&init_thr->tcb_context) < 0 ||
	    getcontext(&sched->sched_context) < 0)
		err(-1, "%s: Error", __func__);

	if (!init_thr || !sched || !sched_stack)
		err(-1, "%s: Error", __func__);


	sched->sched_context.uc_stack.ss_size = SIGSTKSZ;
	sched->sched_context.uc_stack.ss_sp = sched_stack;
	sched->sched_context.uc_link = NULL;
	sched->sched_context.uc_stack.ss_flags = 0;

	makecontext(&sched->sched_context, schedule, 0);

	memset(&sig_act, 0, sizeof(sig_act));

	sig_act.sa_handler = scheduler_preempt;
	if (sigaction(SIGPROF, &sig_act, NULL) < 0)
		err(-1, "%s: Error", __func__);

	init_thr->ID = open_tid++;
	init_thr->stat = SCHEDULED;
	init_thr->join = NULL;
	sched->run = init_thr;
	sched->prior = 0;
	for (i = 0; i < NUM_QS; i++)
		sched->q[i] = NULL;
	enqueue(init_thr, &sched->q[0]);
};


static void start_thread(void *(*function)(void *), void *arg)
{
    worker_exit(function(arg));
};
