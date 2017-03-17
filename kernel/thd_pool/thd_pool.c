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
	printk("%s: a thd (id=%ld) exited, in_list %ld, total %ld\n", 
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


/* create a thread if necessary */
static int thd_pool_create(struct thd_pool *thp)
{
	int ret;
	long num;
	struct thd_pool_thread *tpt;

	ret = 0;

	/* see if an allocatin is necessary */
	spin_lock(&thp->lock);
	if(thp->in_list >= thp->min || thp->in_list > thp->max)
		goto unlock;
	/* increment here, to avoid overcreating threads */
	thp->in_list++;
	num = thp->number++;
	spin_unlock(&thp->lock);

	/* Allocate all necessary memory */
	tpt = kmalloc(sizeof(struct thd_pool_thread), GFP_KERNEL);
	if(!tpt)
		goto nomem;
	tpt->tsk = kthread_create_on_node(thd_pool_main_loop, (void*)tpt, 
				thp->node, "%s_%ld", thp->name, num);
	if (IS_ERR(tpt->tsk))
		goto freetpt;

	/* initialise the fields */
	tpt->id = num;
	tpt->thd_pool = thp;
	tpt->state = NULL;
	thd_pool_service_init(&tpt->service);

	/* register the new thread */
	spin_lock(&thp->lock);
	list_add(&tpt->list, &thp->all_threads);
	printk("%s: a thd (id=%ld) created, in_list %ld, total %ld\n", 
			__func__, tpt->id, thp->in_list, thp->number);
	spin_unlock(&thp->lock);

	return 0;

freetpt:
	kfree(tpt);
nomem:
	ret = ENOMEM;
	spin_lock(&thp->lock);
	thp->in_list--;
	thp->number--;
unlock:
	spin_unlock(&thp->lock);
	return ret;
}

int thd_pool_init_raw(struct thd_pool *thp, int min, int max, int node, char *namefmt, ...)
{
	int ret, i;
	va_list args;

	if(min <0)
		min = THD_POLL_DEFAULT_MIN;
	if(max <0)
		max = THD_POLL_DEFAULT_MAX;

	va_start(args, namefmt);
	vsnprintf(thp->name, sizeof(thp->name), namefmt, args);
	va_end(args);

	thp->min = min;
	thp->max = max;
	thp->node = node;

	thp->number=0; /* incremented by thd_pool_create */
	thp->in_list=0; /* incremented by thd_pool_pause*/

	spin_lock_init(&thp->lock);
	INIT_LIST_HEAD(&thp->all_threads);

	for(i=0; i<thp->min; i++)
	{
		ret = thd_pool_create(thp);
		if(ret) 
			goto err;
	}

	return 0;
err:
	thd_pool_destroy(thp);
	return ret;
}
EXPORT_SYMBOL_GPL(thd_pool_init_raw);

static void thd_pool_refill(struct thd_pool *thp)
{
	thd_pool_create(thp);
}

static struct thd_pool_thread* thd_pool_get(struct thd_pool *thp)
{
	struct thd_pool_thread *tpt;

	thd_pool_refill(thp);

	spin_lock(&thp->lock);
	tpt = list_first_entry_or_null(&thp->all_threads, struct thd_pool_thread, list);
	if(!tpt) 
		goto notpt;
	list_del(&tpt->list);
	thp->in_list--;
notpt:
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
