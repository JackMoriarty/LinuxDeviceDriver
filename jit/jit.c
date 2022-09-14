/*
 * 时间、延迟及延缓操作
 * @Author: Bangduo Chen 
 * @Date: 2018-09-18 14:37:00 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-18 22:56:04
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/interrupt.h>

#include <linux/hardirq.h>

int delay = HZ;
module_param(delay, int, 0);

MODULE_AUTHOR("Bangduo Chen");
MODULE_LICENSE("GPL");

enum jit_files {
    JIT_BUSY,
    JIT_SCHED,
    JIT_QUEUE,
    JIT_SCHEDTO
};

int jit_fn(char *buf, char **start, off_t offset,
            int len, int *eof, void *data)
{
    unsigned long j0, j1;
    wait_queue_head_t wait;

    init_waitqueue_head(&wait);
    j0 = jiffies;
    j1 = j0 + delay;
    
    switch((long)data) {
        case JIT_BUSY:
            while(time_before(jiffies, j1))
                cpu_relax();
            break;
        case JIT_SCHED:
            while(time_before(jiffies, j1))
                schedule();
            break;
        case JIT_QUEUE:
            wait_event_interruptible_timeout(wait, 0, delay);
            break;
        case JIT_SCHEDTO:
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(delay);
            break;
    }

    // 延迟后的jiffies真实值
    j1 = jiffies;
    
    // 输出
    len = sprintf(buf, "%9li %9li\n", j0, j1);
    *start = buf;
    return len;
}

// 获取当前时间
int jit_currenttime(char *buf, char **start, off_t offset,
                    int len, int *eof, void *data)
{
    struct timeval tv1;
    struct timespec tv2;
    unsigned long j1;
    u64 j2;

    // 获取这四项
    j1 = jiffies;
    j2 = get_jiffies_64();
    do_gettimeofday(&tv1);
    tv2 = current_kernel_time();

    // 打印
    len = 0;
    len += sprintf(buf, "0x%08lx 0x%016Lx %10i.%06i\n"
                        " %40i.%09i\n",
                        j1, j2,
                        (int)tv1.tv_sec, (int)tv1.tv_usec,
                        (int)tv2.tv_sec, (int)tv2.tv_nsec);
    *start = buf;
    return len;
}

// 内核定时器
int tdelay = 10;
module_param(tdelay, int, 0);

struct jit_data {
    struct timer_list timer;
    struct tasklet_struct tlet;
    int hi;
    wait_queue_head_t wait;
    unsigned long prevjiffies;
    unsigned char *buf;
    int loops;
};
#define JIT_ASYNC_LOOPS 5

void jit_timer_fn(unsigned long arg)
{
    struct jit_data *data = (struct jit_data *)arg;
    unsigned long j = jiffies;
    data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

    if (--data->loops) {
        data->timer.expires += tdelay;
        data->prevjiffies = j;
        add_timer(&data->timer);
    } else {
        wake_up_interruptible(&data->wait);
    }
}

int jit_timer(char *buf, char **start, off_t offset,
                int len, int *eof, void *unused_data)
{
    struct jit_data *data;
    char *buf2 = buf;
    unsigned long j = jiffies;
    
    data = kmalloc(sizeof(*data), GFP_KERNEL);
    if(!data)
        return -ENOMEM;
    
    // 初始化定时器
    init_timer(&data->timer);
    init_waitqueue_head(&data->wait);

    // 打印第一行
    buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
    buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
                    j, 0L, in_interrupt() ? 1 : 0,
                    current->pid, smp_processor_id(), current->comm);
    
    // 填充数据结构
    data->prevjiffies = j;
    data->buf = buf2;
    data->loops = JIT_ASYNC_LOOPS;

    // 注册定时器
    data->timer.data = (unsigned long)data;
    data->timer.function = jit_timer_fn;
    data->timer.expires = j + tdelay;
    add_timer(&data->timer);

    // 等待缓冲区填满
    wait_event_interruptible(data->wait, !data->loops);
    if (signal_pending(current))
        return -ERESTARTSYS;
    buf2 = data->buf;
    kfree(data);
    *eof = 1;
    return buf2 - buf;

}

// tasklet 处理函数
void jit_tasklet_fn(unsigned long arg)
{
    struct jit_data *data = (struct jit_data *)arg;
    unsigned long j = jiffies;
    data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
                j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
                current->pid, smp_processor_id(), current->comm);
    
    if (--data->loops) {
        data->prevjiffies = j;
        if (data->hi)
            tasklet_hi_schedule(&data->tlet);
        else
            tasklet_schedule(&data->tlet);
    } else {
        wake_up_interruptible(&data->wait);
    }
}

// tasklet
int jit_tasklet(char *buf, char **start, off_t offset,
                int len, int *eof, void *arg)
{
    struct jit_data *data;
    char *buf2 = buf;
    unsigned long j = jiffies;
    long hi = (long)arg;

    data = kmalloc(sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
    
    init_waitqueue_head(&data->wait);

    buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
    buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);
    
    // 填充数据结构
    data->prevjiffies = j;
    data->buf = buf2;
    data->loops = JIT_ASYNC_LOOPS;

    // 注册tasklet
    tasklet_init(&data->tlet, jit_tasklet_fn, (unsigned long)data);
    data->hi = hi;

    if (hi)
        tasklet_hi_schedule(&data->tlet);
    else
        tasklet_schedule(&data->tlet);
    
    // 等待填充缓冲区完毕
    wait_event_interruptible(data->wait, !data->loops);

    if (signal_pending(current))
        return -ERESTARTSYS;
    buf2 = data->buf;
    kfree(data);
    *eof = 1;
    return buf2 - buf;
}

static int __init jit_init(void)
{
    create_proc_read_entry("currenttime", 0, NULL, jit_currenttime, NULL);
    create_proc_read_entry("jitbusy", 0, NULL, jit_fn, (void *)JIT_BUSY);
    create_proc_read_entry("jitsched", 0, NULL, jit_fn, (void *)JIT_SCHED);
    create_proc_read_entry("jitqueue", 0, NULL, jit_fn, (void *)JIT_QUEUE);
    create_proc_read_entry("jitschedto", 0, NULL, jit_fn, (void *)JIT_SCHEDTO);
    
    create_proc_read_entry("jitimer", 0, NULL, jit_timer, NULL);
    create_proc_read_entry("jitasklet", 0, NULL, jit_tasklet, NULL);
    create_proc_read_entry("jitasklethi", 0, NULL, jit_tasklet, (void *)1);
    return 0;
}

static void __exit jit_exit(void)
{
    remove_proc_entry("currenttimie", NULL);
    remove_proc_entry("jitbusy", NULL);
    remove_proc_entry("jitsched", NULL);
    remove_proc_entry("jitqueue", NULL);
    remove_proc_entry("jitschedto", NULL);

    remove_proc_entry("jitimer", NULL);
    remove_proc_entry("jitasklet", NULL);
    remove_proc_entry("jitasklethi", NULL);
}

module_init(jit_init);
module_exit(jit_exit);
