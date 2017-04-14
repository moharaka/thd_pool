#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <asm/barrier.h>
#include <linux/thd_pool.h>

static void thd_pool_service_init(struct thd_pool_service *srv)
{
	srv->func = NULL;
	srv->data = NULL;
}

static void thd_pool_service_exec(struct thd_pool_service *srv, 
				struct thd_pool_srv_state* state)
{
	long ret;

	if(!srv->func)
		return;

	ret = srv->func(srv->data);
	if(state)
	{
		state->ret = ret;
		smp_mb(); 
		state->end = 1;
	}
}

/* returns -1 if the thread need to exit */
static int thd_pool_pause_or_exit(struct thd_pool_thread *tpt)
{
	struct thd_pool *thp;
	
	thp = tpt->thd_pool;

	spin_lock(&thp->lock);

	if(thp->number > thp->max) goto exit;
	if(kthread_should_stop()) goto exit;

	set_current_state(TASK_INTERRUPTIBLE);
	list_add(&tpt->list, &thp->all_threads);
	thp->in_list++;

	spin_unlock(&thp->lock);
	schedule();	

	return 0;

exit:	/* lock is held */
	thp->number--;
	kfree(tpt);
	pr_debug("%s: a thd (id=%ld) exited, in_list %ld, total %ld\n", 
			__func__, tpt->id, thp->in_list, thp->number);
	spin_unlock(&thp->lock);
	return -1;
}

static int thd_pool_main_loop(void *data)
{
	struct thd_pool_thread *tpt;

	tpt = (struct thd_pool_thread*) data;
	
	do{
		/* executes the given service */
		thd_pool_service_exec(&tpt->service, tpt->state);
		/* reinitialises the service structure */
		thd_pool_service_init(&tpt->service);
	}while(!thd_pool_pause_or_exit(tpt));

	return 0;
}

/* should be called once there is no more rqst */
void thd_pool_destroy(struct thd_pool *thp)
{
	struct thd_pool_thread *tpt;

	list_for_each_entry(tpt, &thp->all_threads, list)
	{
		kthread_stop(tpt->tsk);
		/* tpts are freeds by the thread */
		/* thp is freed by the creator of the pool */
	}
}
EXPORT_SYMBOL_GPL(thd_pool_destroy);

static struct thd_pool_thread* __thd_pool_create(struct thd_pool *thp, long id)
{
	struct thd_pool_thread *tpt;


	/* Allocate all necessary memory */
	tpt = kmalloc(sizeof(struct thd_pool_thread), GFP_KERNEL);
	if(!tpt)
		goto nomem;
	tpt->tsk = kthread_create_on_node(thd_pool_main_loop, (void*)tpt, 
				thp->node, "%s_%ld", thp->name, id);
	if (IS_ERR(tpt->tsk))
		goto freetpt;

	/* initialise the fields */
	tpt->id = id;
	tpt->thd_pool = thp;
	tpt->state = NULL;
	thd_pool_service_init(&tpt->service);

	return tpt;

freetpt:
	kfree(tpt);
nomem:
	return NULL;
}

int thd_pool_create(struct thd_pool *thp, long id)
{
	struct thd_pool_thread *tpt;
	
	tpt = __thd_pool_create(thp, id);
	if(!tpt)
		goto nomem;

	list_add(&tpt->list, &thp->all_threads);

	pr_debug("%s: a thd (id=%ld) created, in_list %ld, total %ld\n", 
			__func__, tpt->id, thp->in_list, id+1);

	return 0;
nomem:
	return ENOMEM;
}


int thd_pool_init_raw(struct thd_pool *thp, int min, int max, int node, char *namefmt, ...)
{
	int ret, i;
	va_list args;

	if(min <0)
		min = THD_POOL_DEFAULT_MIN;
	if(max <0)
		max = THD_POOL_DEFAULT_MAX;

	va_start(args, namefmt);
	vsnprintf(thp->name, sizeof(thp->name), namefmt, args);
	va_end(args);

	thp->min = min;
	thp->max = max;
	thp->node = node;
	thp->balance = 0;
	thp->number = min-1; 
	thp->in_list = min-1;

	spin_lock_init(&thp->lock);
	INIT_LIST_HEAD(&thp->all_threads);

	for(i=0; i<thp->min; i++)
	{
		ret = thd_pool_create(thp, i);
		if(ret) 
			goto err;
	}
	return 0;
err:
	thd_pool_destroy(thp);
	return ret;
}
EXPORT_SYMBOL_GPL(thd_pool_init_raw);

#define THD_POOL_ADJUST_FREQ 100
/* This function is called when a the of number in the list is positif.	*
 * I tries to decrementing max in a balanced way.			*
 * Note: lock should be held */
static void balance_max_down(struct thd_pool *thp)
{
	/* if max equal min then return. Else try to decrement max.	*
	 * This is because max cannot be smaller than min.		*/
	if(thp->max == thp->min)
	{
		thp->balance = 0;
		return;
	}

	/* if the number of element in the list is lower than min: don't *
	 * decrement max */
	if(thp->in_list <= thp->min)
	{
		thp->balance = 0;
		return;
	}

	/* decrement max only if we were at least THD_POOL_ADJUST_FREQ 
	 * times far from min */
	thp->balance--;
	if((thp->balance <= -THD_POOL_ADJUST_FREQ))
	{
		thp->balance = 0;
		thp->max--;
	}
}

/* lock should be held */
static void balance_max_up(struct thd_pool *thp)
{
	/* decreases the chances of decreasing max */
	thp->balance = thp->balance>0 ? thp->balance+1: 0;

	thp->max++;
}

/* get a thread from pool or create if absent */
static struct thd_pool_thread* thd_pool_get(struct thd_pool *thp)
{
	long num;
	struct thd_pool_thread *tpt;

	/* see if an allocatin is necessary */
	spin_lock(&thp->lock);
	if(thp->in_list > 0)
	{
		balance_max_down(thp);
		goto fromlist;
	}else{
		/* nothing in the list increase max */
		balance_max_up(thp);
	}
	/* increment here, to avoid overcreating threads */
	thp->in_list++;
	num = thp->number++;
	spin_unlock(&thp->lock);

	/* Allocate a tpt and return it */
	tpt = __thd_pool_create(thp, num);
	if(!tpt)
		goto nomem;
	return tpt;
nomem:
	spin_lock(&thp->lock);
	thp->in_list--;
	thp->number--;
fromlist:
	tpt = list_first_entry_or_null(&thp->all_threads, struct thd_pool_thread, list);
	if(!tpt) 
		goto unlock;
	list_del(&tpt->list);
	thp->in_list--;
unlock:
	spin_unlock(&thp->lock);
	return tpt;
}


int thd_pool_rqst_raw(struct thd_pool *thp, struct thd_pool_service *srv, 
					struct thd_pool_srv_state *state)
{
	struct thd_pool_thread *tpt;

	tpt = thd_pool_get(thp);
	if(tpt == NULL)
		goto notpt;
	tpt->service = *srv;
	tpt->state = state;
	/* make sure that the service has beed writeen before reading it */
	smp_mb(); 

	wake_up_process(tpt->tsk);

	return 0;
notpt:
	/* Retry until a thread is available */
	return EAGAIN;
}
EXPORT_SYMBOL_GPL(thd_pool_rqst_raw);
