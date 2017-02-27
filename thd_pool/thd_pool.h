#pragma once
#include <linux/list.h>
#include <linux/spinlock.h>

typedef long thd_pool_func(void* data);

struct thd_pool_service {
	/* function, argument, and return value of the service */
	thd_pool_func *func;
	void *data;
	long ret;
};

struct thd_pool_thread {
	int id;
	struct task_struct *thd;
	struct thd_pool_service service;

	/* thread pool pointer, and list entry */
	struct thd_pool *thd_pool;
	struct list_head list;
};

struct thd_pool {
	long number;		/* total number of thread in the pool */
	long in_list;		/* thread currently available in the pool */
	long min, max;		/* min and max number of thread in the pool */
	spinlock_t lock;	
	struct list_head all_threads;	/* root of all available threads */
};

#define THD_POLL_DEFAULT_MIN 3
#define THD_POLL_DEFAULT_MAX 9

/**** exported functions ****/

/* if min or max = -1, use default values */
int thd_pool_init(struct thd_pool *thp, int min, int max);

int thd_pool_rqst(struct thd_pool *thp, thd_pool_func *func, void*data);

int thd_pool_rqst_srv(struct thd_pool *thp, struct thd_pool_service *srv);

/* should be called once there is no more request */
void thd_pool_destroy(struct thd_pool *thp);
