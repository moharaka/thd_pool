#pragma once
#include <linux/list.h>
#include <linux/spinlock.h>

#define THP_NAME_MAX 256
#define THD_POLL_DEFAULT_MIN 3
#define THD_POLL_DEFAULT_MAX 9

typedef long thd_pool_func(void* data);

/* A thread pool service */
struct thd_pool_service {
	thd_pool_func *func;	/* function to execute */
	void *data;		/* data of the function */
};

/* A thread pool service */
struct thd_pool_srv_state {
	bool end;	/* signal the end of the service */
	long ret;	/* return value */
};

struct thd_pool_thread {
	long id;			 /* thread id */
	struct task_struct *tsk;	 /* thread task structure */
	struct thd_pool_service service; /* service (function) to execute */
	struct thd_pool_srv_state *state;/* state of the service */

	/* management fields */
	struct thd_pool *thd_pool;	/* pointer to the owner (thd_pool) */
	struct list_head list;		/* links availale threads to owner */
};

struct thd_pool {
	char name[THP_NAME_MAX];
	int node;
	long number;		/* total number of created threads */
	long in_list;		/* thread currently available in the list */
	long min, max;		/* min and max number of thread in the list */
	spinlock_t lock;	/* synchronise all fields */
	struct list_head all_threads;	/* root of all available threads */
};


/**************************** Exported Functions ************************************/

/* initiliase a thp_pool.					*
 * min, max: min and max number of threads (-1 default values)	*
 * node: pin all thread to a node, unless node egal -1		*
 * namefmt, ...: printf like arguments for name			*/
int thd_pool_init_raw(struct thd_pool *thp, int min, int max, int node, char *namefmt, ...);

/* same as thd_pool_init_raw but uses default values */
//int thd_pool_init(struct thd_pool *thp, char *namefmt, ...);

/* Destroy the pool. Should be called once there *
 * is no requests (No sychronisation is used) */
void thd_pool_destroy(struct thd_pool *thp);

/* requesting a service */
int thd_pool_rqst_raw(struct thd_pool *thp, struct thd_pool_service *srv, 
					struct thd_pool_srv_state *state);

/* service with no return value */
static inline int thd_pool_rqst(struct thd_pool *thp, thd_pool_func *func, void*data);



/************************************* Private **************************************/

#define thd_pool_init(thp, namefmt, ...) \
		thd_pool_init_raw(thp, -1,-1,-1, namefmt, ## __VA_ARGS__);

static inline int thd_pool_rqst(struct thd_pool *thp, thd_pool_func *func, void*data) 
{
	struct thd_pool_service srv = {func, data};
	return thd_pool_rqst_raw(thp, &srv, NULL);
}
