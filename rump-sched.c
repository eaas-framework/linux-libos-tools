/*
 * Scheduler proxy of NUSE for rumpuser backend
 * Copyright (c) 2015 Hajime Tazaki
 *
 * Author: Hajime Tazaki <tazaki@sfc.wide.ad.jp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <linux/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include <rump/rumpuser_port.h>
#include <rump/rumpuser.h>
#include <rump/rump.h>

#include "sim-init.h"
#include "sim-assert.h"
#include "sim.h"
#include "nuse.h"
#include "rump-sched.h"

#define NSEC_PER_SEC	1000000000L

static unsigned long rump_pid = 1000;
static LIST_HEAD(, rump_task) task_list = LIST_HEAD_INITIALIZER(task_list);

static struct rump_task *lwp0 = NULL;
static bool threads_are_go;
static struct rumpuser_mtx *thrmtx;
static struct rumpuser_cv *thrcv;

/* librumpuser/rumpuser_int.h */
int  rumpuser__errtrans(int);

#ifdef RUMPRUN_READY
int
rump_pub_lwproc_newlwp(pid_t arg1)
{
	int rv;

	rump_schedule();
	rv = nuse_new_task("lwp") ? -1 : 0;
	rump_unschedule();

	return rv;
}

void
rump_pub_lwproc_switch(struct lwp *arg1)
{

	rump_schedule();
	/* FIXME */
	rump_lwproc_switch(arg1);
	rump_unschedule();
}

void
rump_pub_lwproc_releaselwp(void)
{
	struct nuse_task *task = (struct nuse_task *)rumpuser_curlwp();

	rump_schedule();
	nuse_release_task(task);
	rump_unschedule();
}

struct lwp *
rump_pub_lwproc_curlwp(void)
{
	struct lwp *rv;

	rump_schedule();
	rv = nuse_task_current(NULL);
	rump_unschedule();

	return rv;
}
#endif /* RUMPRUN_READY */

static size_t strlen(const char *s)
{
	const char *sc;

	for (sc = s; *sc != '\0'; ++sc)
		/* nothing */;
	return sc - s;
}

static char *strncpy(char *dest, const char *src, size_t count)
{
	char *tmp = dest;

	while (count) {
		if ((*tmp = *src) != 0)
			src++;
		tmp++;
		count--;
	}
	return dest;
}

struct rump_task *rump_new_task(char *name)
{
	struct rump_task *task;

	task = lib_malloc(sizeof(struct rump_task));
	lib_memset(task, 0, sizeof(*task));
	task->s_task = g_exported->task_create(task, ++rump_pid);
	strncpy(task->name, name, strlen(name));
	rumpuser_cv_init(&task->cv);
	rumpuser_mutex_init(&task->mtx, RUMPUSER_MTX_SPIN);
	LIST_INSERT_HEAD(&task_list, task, entries);

	return task;
}

void rump_release_task(struct rump_task *task)
{
	LIST_REMOVE(task, entries);

	g_exported->task_destroy(task->s_task);
	rumpuser_cv_destroy(task->cv);
#ifdef notyet
	rumpuser_mutex_destroy(task->mtx);
#endif
	free(task->mtx);
	free(task);
	/* dont exit if this is a sysproxy task */
	if (!task->rump_client)
		rumpuser_thread_exit();
}

struct rump_task *rump_find_task(unsigned long pid)
{
	struct rump_task *task;

	LIST_FOREACH(task, &task_list, entries) {
		if (task->pid == pid)
			return task;
	}
	return NULL;
}

struct SimTask *rump_task_current(struct SimKernel *kernel)
{
	struct rump_task *task = (struct rump_task *)rumpuser_curlwp();

	if (!lwp0) {
		lwp0 = rump_new_task("init");
		rumpuser_curlwpop(RUMPUSER_LWP_CREATE, (struct lwp *)lwp0);
	}

	if (!task) {
		task = lwp0;
		rumpuser_curlwpop(RUMPUSER_LWP_SET, (struct lwp *)task);
	}

	return task->s_task;
}

struct thrdesc {
	void (*f)(void *);
	void *arg;
	struct rump_task *newlwp;
	int runnable;
	struct timespec timeout;
};

static void *rump_task_start_trampoline(void *arg)
{
	struct thrdesc *td = arg;
	struct rump_task *task = td->newlwp;
	void (*f)(void *);
	void *thrarg;
	int err;

	/* from src-netbsd/sys/rump/librump/rumpkern/thread.c */
	/* don't allow threads to run before all CPUs have fully attached */
	if (!threads_are_go) {
		rumpuser_mutex_enter_nowrap(thrmtx);
		while (!threads_are_go) {
			rumpuser_cv_wait_nowrap(thrcv, thrmtx);
		}
		rumpuser_mutex_exit(thrmtx);
	}

	rumpuser_curlwpop(RUMPUSER_LWP_SET, (struct lwp *)task);

	f = td->f;
	thrarg = td->arg;

	if (td->timeout.tv_sec != 0 || td->timeout.tv_nsec != 0) {
		rumpuser_mutex_enter(task->mtx);
		err = rumpuser_cv_timedwait(task->cv, task->mtx,
					    td->timeout.tv_sec,
					    td->timeout.tv_nsec);
		if (task->canceled) {
			if (!task->thrid) {
				rumpuser_curlwpop(RUMPUSER_LWP_DESTROY,
						  (struct lwp *)task);
				rump_release_task(task);
				free(td);
			}
			goto end;
		}
		rumpuser_mutex_exit(task->mtx);
		if (err && err != rumpuser__errtrans(ETIMEDOUT))
			goto end;
	}

#ifdef notyet
	rump_libos_schedule();
#endif
	lib_assert(f);
	f(thrarg);

	rumpuser_curlwpop(RUMPUSER_LWP_DESTROY, (struct lwp *)task);
	rump_release_task(task);
	free(td);
end:
	lib_update_jiffies();
	return arg;
}

struct SimTask *rump_task_start(struct SimKernel *kernel,
				void (*func)(void *), void *arg)
{
	int pri = 0;
	int rv;
	struct thrdesc *td;
	int joinable = 1;
	char *name = "task";

	td = lib_malloc(sizeof(*td));
	lib_memset(td, 0, sizeof(*td));
	td->newlwp = rump_new_task(name);
	rumpuser_curlwpop(RUMPUSER_LWP_CREATE, (struct lwp *)td->newlwp);

	td->f = func;
	td->arg = arg;

	rv = rumpuser_thread_create(rump_task_start_trampoline, td, name,
				    joinable, pri, -1, &td->newlwp->thrid);
	if (rv) {
		rumpuser_curlwpop(RUMPUSER_LWP_DESTROY,
				  (struct lwp *)td->newlwp);
		rump_release_task(td->newlwp);
		free(td);
		return NULL;
	}

	return td->newlwp->s_task;
}

void *rump_event_schedule_ns(struct SimKernel *kernel,
			     __u64 ns, void (*func) (void *arg), void *arg,
			     void (*dummy_fn)(void))
{
	int pri = 0;
	int rv;
	struct thrdesc *td;
	int joinable = 0;
	char *name = "timer";

	td = lib_malloc(sizeof(*td));
	lib_memset(td, 0, sizeof(*td));
	td->newlwp = rump_new_task(name);
	rumpuser_curlwpop(RUMPUSER_LWP_CREATE, (struct lwp *)td->newlwp);

	td->f = func;
	td->arg = arg;

	td->timeout = ((struct timespec) { .tv_sec = ns / NSEC_PER_SEC,
				.tv_nsec = ns % NSEC_PER_SEC });

	rv = rumpuser_thread_create(rump_task_start_trampoline, td, name,
				    joinable, pri, -1, &td->newlwp->thrid);
	if (rv) {
		rumpuser_curlwpop(RUMPUSER_LWP_DESTROY,
				  (struct lwp *)td->newlwp);
		rump_release_task(td->newlwp);
		free(td);
		return NULL;
	}

	return td;
}

void rump_event_cancel(struct SimKernel *kernel, void *event)
{
	struct thrdesc *td = event;
	struct rump_task *task = td->newlwp;

	if (task->canceled)
		return;

	task->canceled = 1;
	rumpuser_mutex_enter(task->mtx);
	rumpuser_cv_signal(task->cv);
	rumpuser_mutex_exit(task->mtx);

	if (task->thrid) {
		rumpuser_thread_join(task->thrid);

		rumpuser_curlwpop(RUMPUSER_LWP_DESTROY, (struct lwp *)task);
		rump_release_task(task);
		free(td);
	}
}

void rump_task_wait(struct SimKernel *kernel)
{
	struct SimTask *lib_task = rump_task_current(NULL);
	struct rump_task *task = g_exported->task_get_private(lib_task);

	lib_assert(task != NULL);

	rumpuser_mutex_enter(task->mtx);
	rumpuser_cv_wait(task->cv, task->mtx);
	rumpuser_mutex_exit(task->mtx);
	lib_update_jiffies();
}

int rump_task_wakeup(struct SimKernel *kernel, struct SimTask *lib_task)
{
	struct rump_task *task = g_exported->task_get_private(lib_task);
	int nwaiters;

	lib_assert(task != NULL);
	rumpuser_cv_has_waiters(task->cv, &nwaiters);
	if (nwaiters)
		rumpuser_cv_signal(task->cv);
	return nwaiters ? 1 : 0;
}

static inline struct timespec timespec_add(struct timespec lhs,
					   struct timespec rhs)
{
	struct timespec ts;

	ts.tv_sec = lhs.tv_sec + rhs.tv_sec;
	ts.tv_nsec = lhs.tv_nsec + rhs.tv_nsec;
	while (ts.tv_nsec >= NSEC_PER_SEC) {
		ts.tv_nsec -= NSEC_PER_SEC;
		ts.tv_sec++;
	}
	return ts;
}

/* clock thread routine */
static void *rump_clock_thread(void *noarg)
{
	struct timespec thetick, curclock;
	int64_t sec;
	long nsec;
	int error;

	/* from src-netbsd/sys/rump/librump/rumpkern/thread.c */
	/* don't allow threads to run before all CPUs have fully attached */
	if (!threads_are_go) {
		rumpuser_mutex_enter_nowrap(thrmtx);
		while (!threads_are_go) {
			rumpuser_cv_wait_nowrap(thrcv, thrmtx);
		}
		rumpuser_mutex_exit(thrmtx);
	}

	error = rumpuser_clock_gettime(RUMPUSER_CLOCK_ABSMONO, &sec, &nsec);
	if (error) {
		lib_printf("clock: cannot get monotonic time\n");
		lib_assert(0);
	}

	curclock.tv_sec = sec;
	curclock.tv_nsec = nsec;
	thetick.tv_sec = 0;
	thetick.tv_nsec = 1000000000/HZ;


	for (;;) {
		lib_update_jiffies();

		error = rumpuser_clock_sleep(RUMPUSER_CLOCK_ABSMONO,
					     curclock.tv_sec, curclock.tv_nsec);
		if (error) {
			lib_printf("clock sleep failure\n");
			lib_assert(0);
		}
		timespec_add(curclock, thetick);
	}

	/* should not reach */
	return NULL;
}

/* from src-netbsd/sys/rump/librump/rumpkern/thread.c */
void
rump_thread_allow(struct lwp *l)
{
#ifdef notyet
	struct thrdesc *td;
#endif

	rumpuser_mutex_enter(thrmtx);
	if (l == NULL) {
		threads_are_go = true;
	} else {
#ifdef notyet
		TAILQ_FOREACH(td, &newthr, entries) {
			if (td->newlwp == l) {
				td->runnable = 1;
				break;
			}
		}
#endif
	}
	rumpuser_cv_broadcast(thrcv);
	rumpuser_mutex_exit(thrmtx);
}

void rump_sched_init(void)
{
	int rv;
	void *thrid;

	rumpuser_mutex_init(&thrmtx, RUMPUSER_MTX_SPIN);
	rumpuser_cv_init(&thrcv);
	threads_are_go = false;

	rv = rumpuser_thread_create(rump_clock_thread, NULL, "clock",
				    0, 0, -1, &thrid);
	if (rv) {
		lib_printf("thread create failure\n");
		lib_assert(0);
	}
}