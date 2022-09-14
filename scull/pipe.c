/*
 * scull_pipe设备
 * @Author: Bangduo Chen
 * @Date: 2018-09-10 19:36:14
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-10 23:08:43
 */

#include <asm/semaphore.h>  // struct semaphore 等
#include <asm/uaccess.h>

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include <linux/wait.h> //wait_queue_head_t
#include <linux/fs.h>
#include <linux/cdev.h> //struct cdev 等
#include <linux/proc_fs.h>
#include <linux/poll.h>


#include "scull.h"

struct scull_pipe {
    wait_queue_head_t inq, outq;  //读写等待队列
    char *buffer, *end;   //缓冲区(缓冲区头,缓冲区尾)
    int buffersize;       // 用于指针计算
    char *rp, *wp;        //读取和写入位置
    int nreaders, nwriters; //用于读写打开数量
    struct fasync_struct *async_queue;  // 异步读取者
    struct semaphore sem; //互斥信号量
    struct cdev cdev;     //字符设备结构
};

static int scull_p_nr_devs = SCULL_P_NR_DEVS; // scullpipe设备数量
int scull_p_buffer = SCULL_P_BUFFER;  //缓冲区大小
dev_t scull_p_devno;  //第一个设备编号

module_param(scull_p_nr_devs, int, S_IRUGO);
module_param(scull_p_buffer, int, S_IRUGO);

static struct scull_pipe *scull_p_devices;

static int scull_p_fasync(int fd, struct file *flip, int mode);
static int spacefree(struct scull_pipe *dev);


// open 函数

static int scull_p_open(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev;

    dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
    filp->private_data = dev;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if (!dev->buffer) {
        // 申请内存
        dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
        if (!dev->buffer) {
            up(&dev->sem);
            return -ENOMEM;
        }
    }
    dev->buffersize = scull_p_buffer;
    dev->end = dev->buffer + dev->buffersize;
    dev->rp = dev->wp = dev->buffer;    // 从缓冲区头部开始读写

    // 使用 f_mode 而不是 f_flags
    if (filp->f_mode & FMODE_READ)
        dev->nreaders++;
    if (filp->f_mode & FMODE_WRITE)
        dev->nwriters++;
    up(&dev->sem);

    return nonseekable_open(inode, filp);
}

// release函数
static int scull_p_release(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev = filp->private_data;

    // // 从异步通知中移除该文件指针
    scull_p_fasync(-1, filp, 0);
    down(&dev->sem);
    if (filp->f_mode & FMODE_READ)
        dev->nreaders--;
    if (filp->f_mode & FMODE_WRITE)
        dev->nwriters--;
    if (dev->nreaders + dev->nwriters == 0) {
        kfree(dev->buffer);
        dev->buffer = NULL;
    }
    up(&dev->sem);
    return 0;
}

// 读取函数
static ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count,
                            loff_t *f_ops)
{
    struct scull_pipe *dev = filp->private_data;

    // 互斥访问设备
    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    
    while (dev->rp == dev->wp) {    // 无数据可读
        up(&dev->sem);  // 释放信号量
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
        if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
            return -ERESTARTSYS;    // 信号导致停止等待, 通知fs层做相应处理
        // 否则先获取信号量, 再次测试
        if (down_interruptible(&dev->sem))
            return -ERESTARTSYS;
    }
    // 确定此时有数据
    if (dev->wp > dev->rp)
        count = min(count, (size_t)(dev->wp - dev->rp));
    else    // 写指针环回, 只返回读指针到缓冲区末尾的数据
        count = min(count, (size_t)(dev->end - dev->wp));
    // 进行复制
    if (copy_to_user(buf, dev->rp, count)) {
        up(&dev->sem);
        return -EFAULT;
    }

    // 更新
    dev->rp += count;
    if (dev->rp == dev->end) //读指针环回
        dev->rp = dev->buffer;
    up(&dev->sem);
    // 唤醒一个写进程
    wake_up_interruptible(&dev->outq);
    PDEBUG("\"%s\" did read %li bytes\n", current->comm, (long)count);
    return count;
}

// 判断是否空间, 返回空闲空间大小
static int spacefree(struct scull_pipe *dev)
{
    if (dev->rp == dev->wp)
        return dev->buffersize - 1;
    return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}

// 测试是否有空间
static int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
    while (spacefree(dev) == 0) {   // 已满
        // 准备等待
        DEFINE_WAIT(wait);

        // 释放信号量以使读者进程能够读取
        up(&dev->sem);
        PDEBUG("\"%s\" writing: going to sleep\n", current->comm);
        prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
        // 睡眠之前再次检查,以防止唤醒条件已经满足
        if (spacefree(dev) == 0)
            schedule();
        // 被唤醒, 清理
        finish_wait(&dev->outq, &wait);
        // 如果是信号唤醒, 则应通知 fs 层作相应处理
        if (signal_pending(current))
            return -ERESTARTSYS;
        
        // 再次测试, 检查其他进程是否已经又写满了缓冲区
        if (down_interruptible(&dev->sem))
            return -ERESTARTSYS;
    }
    return 0;
}

// 写数据
static ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count,
                            loff_t *f_ops)
{
    struct scull_pipe *dev = filp->private_data;
    int result;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    
    // 确定有空间用来写入数据
    result = scull_getwritespace(dev, filp);
    if (result)
        return result;  // scull_getwritespace 调用了up
    
    // 已经确认缓冲区有空间
    count = min(count, (size_t)spacefree(dev));
    if (dev->wp >= dev->rp) // 如果写指针大于读指针, 最多写到缓冲区末尾
        count = min(count, (size_t)(dev->end - dev->wp));
    else    // 写指针环回, 最多写写指针到读指针之间的空间
        count = min(count, (size_t)(dev->rp - dev->wp -1));
    PDEBUG("Going to accept %li bytes to %p reom %p\n", (long)count, dev->wp, buf);
    if (copy_from_user(dev->wp, buf, count)) {
        up(&dev->sem);
        return -EFAULT;
    }
    
    // 更新相关数据
    dev->wp += count;
    if (dev->wp == dev->end)
        dev->wp = dev->buffer;  //环回
    up(&dev->sem);
    // 唤醒读者
    wake_up_interruptible(&dev->inq);   // 阻塞在read 和 select上的进程

    // 向异步读者发信号
    if (dev->async_queue)
        kill_fasync(&dev->async_queue, SIGIO, POLL_IN);
    PDEBUG("\"%s\" did write %li bytes\n", current->comm, (long)count);
    return count;
}

static unsigned int scull_p_poll(struct file *filp, poll_table *wait)
{
    struct scull_pipe *dev = filp->private_data;
    unsigned int mask = 0;

    // 缓冲区是环形的, 如果wp在rp后面,则缓冲区已满,如果相等,则表明是空的
    down(&dev->sem);
    poll_wait(filp, &dev->inq, wait);
    poll_wait(filp, &dev->outq, wait);
    if (dev->rp != dev->wp)
        mask |= POLLIN | POLLRDNORM;
    if (spacefree(dev))
        mask |= POLLOUT | POLLWRNORM;
    up(&dev->sem);
    return mask;
}

// 异步通知
static int scull_p_fasync(int fd, struct file *filp, int mode)
{
    struct scull_pipe *dev = filp->private_data;
    return fasync_helper(fd, filp, mode, &dev->async_queue);
}

#ifdef SCULL_DEBUG
// 设置/proc文件

static void scullp_proc_offset(char *buf, char **start, off_t *offset, int *len)
{
    if (*offset == 0)
        return;
    if (*offset >= *len) {      // 还没到达要读的数据的位置
        *offset -= *len;
        *len = 0;
    } else {                    // 到达要读取的位置
        *start = buf + *offset;
        *offset = 0;
    }
}

// proc文件读取
static int scull_read_p_mem(char *buf, char **start, off_t offset, int count,
                            int *eof, void *data)
{
    int i, len;
    struct scull_pipe *p;

#define LIMIT (PAGE_SIZE - 200)     // 不要输出超过这个数值大小的数据
    *start = buf;
    len = sprintf(buf, "Default buffersize is %i\n", scull_p_buffer);
    for (i = 0; i < scull_p_nr_devs && len <= LIMIT; i++) {
        p = &scull_p_devices[i];
        if (down_interruptible(&p->sem))
            return -ERESTARTSYS;
        len += sprintf(buf + len, "\nDevice %i: %p\n", i, p);
        len += sprintf(buf + len, " Buffer: %p to %p (%i bytes)\n", p->buffer, p->end, p->buffersize);
        len += sprintf(buf + len, " rp %p   wp %p\n", p->rp, p->wp);
        len += sprintf(buf + len, "readers %i   writers %i\n", p->nreaders, p->nwriters);
        up(&p->sem);
        scullp_proc_offset(buf, start, &offset, &len);
    }
    *eof = (len <= LIMIT);
    return len;
}

#endif

// scull pipe 设备文件操作
struct file_operations scull_pipe_fops = {
    .owner  =   THIS_MODULE,
    .llseek =   no_llseek,
    .read   =   scull_p_read,
    .write  =   scull_p_write,
    .poll   =   scull_p_poll,
    .ioctl  =   scull_ioctl,
    .open   =   scull_p_open,
    .release    =   scull_p_release,
    .fasync =   scull_p_fasync
};

// 设置字符结构,注册字符设备入口
static void scull_p_setup_cdev(struct scull_pipe *dev, int index)
{
    int err, devno = scull_p_devno + index;

    cdev_init(&dev->cdev, &scull_pipe_fops);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev, devno, 1);
    // 如果错误
    if (err)
        printk(KERN_NOTICE "Error %d adding scullpipe%d\n", err, index);
}

// 初始化scullpipe设备
int scull_p_init(dev_t firstdev)
{
    int i, result;

    result = register_chrdev_region(firstdev, scull_p_nr_devs, "scullp");
    if (result < 0) {
        printk(KERN_NOTICE "Unable to get scull region, error %d\n", result);
        return 0;
    }
    scull_p_devno = firstdev;
    scull_p_devices = kmalloc(scull_p_nr_devs * sizeof(struct scull_pipe), GFP_KERNEL);
    if (scull_p_devices == NULL) {
        unregister_chrdev_region(firstdev, scull_p_nr_devs);
        return 0;
    }
    memset(scull_p_devices, 0, scull_p_nr_devs * sizeof(struct scull_pipe));
    for (i = 0; i< scull_p_nr_devs; i++) {
        init_waitqueue_head(&(scull_p_devices[i].inq));
        init_waitqueue_head(&(scull_p_devices[i].outq));
        init_MUTEX(&(scull_p_devices[i].sem));
        scull_p_setup_cdev(scull_p_devices + i, i);
    }

#ifdef SCULL_DEBUG
        create_proc_read_entry("scullpipe", 0, NULL, scull_read_p_mem, NULL);
#endif
    return scull_p_nr_devs;
}

// 清理模块
void scull_p_cleanup(void)
{
    int i;
#ifdef SCULL_DEBUG
    remove_proc_entry("scullpipe", NULL);
#endif
    if(!scull_p_devices)
        return; //没有可释放的东西

    for (i = 0; i < scull_p_nr_devs; i++) {
        cdev_del(&scull_p_devices[i].cdev);
        kfree(scull_p_devices[i].buffer);
    }
    kfree(scull_p_devices);
    unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
    scull_p_devices = NULL;
}
