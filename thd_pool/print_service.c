#include <thd_pool.h>
#include <linux/kernel.h>
#include <linux/smp.h>

int init;
struct thd_pool thp;

static long thd_printk(void *data)
{
	printk("%s: cpu %d, thread %p, arg %d\n", __func__, smp_processor_id(), current, (int)data);
	return 0;
}

asmlinkage long sys_thd_exec(void)
{
	int ret;

	if(!init){ 
		ret = thd_pool_init(&thp,-1,-1);
		if(ret)
		{
		        printk("initialising the pool failed\n");
			goto err;
		}
		init = 1;
	}

	thd_pool_rqst(&thp, thd_printk, (void*) 1);

        return 0;
err:
	return ret;
}
