/*
 * jiq 工作队列
 * @Author: Bangduo Chen 
 * @Date: 2018-09-19 20:06:50 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-19 21:22:05
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/jiffies.h>

static long delay = 10;
module_param(delay, long, 0);

#define LIMIT   (PAGE_SIZE - 128)       // 不要打印查过这个数量的数据

static DECLARE_WAIT_QUEUE_HEAD(jiq_wait);

static struct work_struct jiq_work;

static struct clientdata {
    int len;
    char *buf;
    unsigned long jiffies;
    long delay;
} jiq_data;

static int jiq_print(void *ptr)
{
    struct clientdata *data = (struct clientdata *)ptr;
    int len = data->len;
    char *buf = data->buf;
    unsigned long j = jiffies;

    if (len > LIMIT) {
        wake_up_interruptible(&jiq_wait);
        return 0;
    }

    if (len == 0)
        len = sprintf(buf,"    time  delta preempt   pid cpu command\n");
    else len = 0;

    len += sprintf(buf+len, "%9li  %4li     %3i %5i %3i %s\n",
			j, j - data->jiffies,
			preempt_count(), current->pid, smp_processor_id(),
			current->comm);

	data->len += len;
	data->buf += len;
	data->jiffies = j;
	return 1;
}


static void jiq_print_wq(void *ptr)
{
    struct clientdata *data = (struct clientdata *) ptr;

    if (!jiq_print(ptr))
        return;
    
    if (data->delay)
        schedule_delayed_work(&jiq_work, data->delay);
    else   
        schedule_work(&jiq_work);
}

static int jiq_read_wq(char *buf, char **start, off_t offset,
                    int len, int *eof, void *data)
{
    DEFINE_WAIT(wait);

    jiq_data.len = 0;
    jiq_data.buf = buf;
    jiq_data.jiffies = jiffies;
    jiq_data.delay = 0;

    prepare_to_wait(&jiq_wait, &wait, TASK_INTERRUPTIBLE);
    schedule_work(&jiq_work);
    schedule();
    finish_wait(&jiq_wait, &wait);

    *eof = 1;
    return jiq_data.len;
}

static int jiq_read_wq_delayed(char *buf, char **start, off_t offset,
                    int len, int *eof, void *data)
{
    DEFINE_WAIT(wait);

    jiq_data.len = 0;
    jiq_data.buf = buf;
    jiq_data.jiffies = jiffies;
    jiq_data.delay = delay;

    prepare_to_wait(&jiq_wait, &wait, TASK_INTERRUPTIBLE);
    schedule_delayed_work(&jiq_work, delay);
    schedule();
    finish_wait(&jiq_wait, &wait);

    *eof = 1;
    return jiq_data.len;
}

// 初始化函数和退出函数
static int jiq_init(void)
{
    // 初始化共享队列
    INIT_WORK(&jiq_work, jiq_print_wq, &jiq_data);

    create_proc_read_entry("jiqwq", 0, NULL, jiq_read_wq, NULL);
    create_proc_read_entry("jiqwqdelay", 0, NULL, jiq_read_wq_delayed, NULL);
    return 0;
}

static void jiq_cleanup(void)
{
    remove_proc_entry("jiqwq", NULL);
    remove_proc_entry("jiqwqdelay", NULL);
}

module_init(jiq_init);
module_exit(jiq_cleanup);
