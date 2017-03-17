#include <linux/thd_pool.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cpumask.h>

struct thd_pool thp;

static long all_core_print_exec(void *data)
{
	int cpu;
	cpumask_var_t tmpmask;

	if (!alloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	for_each_online_cpu(cpu)
	{
		cpumask_clear(tmpmask);
		cpumask_set_cpu(cpu, tmpmask);
		set_cpus_allowed_ptr(current, tmpmask);
		printk("%s: cpu %d, thread %p, arg %d\n", __func__, 
				smp_processor_id(), current, (int)data);
	}

	free_cpumask_var(tmpmask);

	return 0;
}

int __init all_core_print_init(void)
{
	int ret;
	ret = thd_pool_init(&thp, "First");
	if(ret)
		printk("Failed to allocate thd_pool %d\n", ret);
	printk("Success to allocate thd_pool %d\n", ret);
	return ret;
}
pure_initcall(all_core_print_init);

//TODO: modify the name
asmlinkage long sys_thd_exec(int data)
{
	thd_pool_rqst(&thp, all_core_print_exec, (void*) data);

        return 0;
}
