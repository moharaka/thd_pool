#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <thd_pool.h>

static void thd_pool_service_init(struct thd_pool_service *srv)
{
	srv->func = NULL;
	srv->data = NULL;
	srv->ret = 0;
}

static void thd_pool_service_exec(struct thd_pool_service *srv)
{
	if(!srv->func)
		return;

	srv->ret = srv->func(srv->data);
	thd_pool_service_init(srv);
}

static void thd_pool_pause(struct thd_pool_thread *tpt)
{
	struct thd_pool *thp;
	
	thp = tpt->thd_pool;

	spin_lock(&thp->lock);
	if(thp->id > thp->max) goto exit;
	set_current_state(TASK_INTERRUPTIBLE);
	list_add(&tpt->list, &thp->all_threads);
	spin_unlock(&thp->lock);

	schedule();	
	__set_current_state(TASK_RUNNING);//?

	return;

exit:	/* exit if id > max */
	spin_unlock(&thp->lock);
	do_exit();
}

static int thd_pool_main_loop(void *data)
{
	struct thd_pool_thread *tpt;

	tpt = (struct thd_pool_thread*) data;

	while(!kthread_should_stop())
	{
		thd_pool_service_exec(&tpt->service);
		thd_pool_pause(tpt);
	}

	kfree(tpt);
	return 0;
}

/* should be called once there is no more rqst */
void thd_pool_destroy(struct thd_pool *thp)
{
	struct thd_pool_thread *tpt;

	list_for_each_entry(tpt, &thp->all_threads, list)
	{
		kthread_stop(tpt->thd);
		/* tpts are freeds by the thread */
	}
}


static int thd_pool_create(struct thd_pool *thp, int num)
{
	struct thd_pool_thread *tpt;

	tpt = kmalloc(sizeof(struct thd_pool_thread), GFP_KERNEL);
	
	if(!tpt)
		goto nomem;

	tpt->thd_pool = thp;
	thd_pool_service_init(&tpt->service);
	tpt->thd = kthread_run_on_cpu(thd_pool_main_loop, (void*)tpt, 
				thp->cpu, "%s_%d", thp->name, num);
	if(!tpt->thd)
	{
		kfree(tpt);
		goto nomem;
	}

	tpt->id = num;

	return 0;
nomem:
	return ENOMEM;
}

int thd_pool_init_raw(struct thd_pool *thp, int min, int max, int cpu, char *namefmt, ...)
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
	thp->number=min+1;
	thp->in_list=thp->number;
	spin_lock_init(&thp->lock);
	INIT_LIST_HEAD(&thp->all_threads);

	for(i=0; i<thp->number; i++)
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

static void thd_pool_refill(struct thd_pool *thp)
{
	spin_lock(&thp->lock);
	if(thp->in_list >= thp->min || thp->in_list > thp->max)
		goto unlock;
	thd_pool_create(thp, thp->number++);
unlock:
	spin_unlock(&thp->lock);
	return;
}

static struct thd_pool_thread* thd_pool_get(struct thd_pool *thp)
{
	struct thd_pool_thread *tpt;

	thd_pool_refill();

	spin_lock(&thp->lock);
	tpt = list_first_entry_or_null(&thp->all_threads, struct thd_pool_thread, list);
	if(!tpt) 
		goto nomem;
	list_del(&tpt->list);
	thp->in_list--;
nomem:
	spin_unlock(&thp->lock);

	return tpt;

}


int thd_pool_rqst_srv(struct thd_pool *thp, struct thd_pool_service *srv)
{
	struct thd_pool_thread *tpt;

	tpt = get_thread(thp);
	if(tpt == NULL)
		goto nomem;
	tpt->service = *srv;
	/* set a memory barrier ? */
	wake_up_process(tpt->thd);

	return 0;
nomem:
	return ENOMEM;
}

int thd_pool_rqst(struct thd_pool *thp, thd_pool_func *func, void*data)
{
	int ret;
	struct thd_pool_service srv = {func,data,0};

	ret = thd_pool_rqst_srv(thp, &srv);

	return ret;
}
