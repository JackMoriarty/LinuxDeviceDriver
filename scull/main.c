/*
 * main.c -- 原始的scull字符设备
 * @Author: Bangduo Chen 
 * @Date: 2018-09-06 20:11:08 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-17 22:22:34
 */

#include <asm/semaphore.h>
#include <asm/uaccess.h>

#include <linux/errno.h>	// error codes
#include <linux/fs.h>		//everything...
#include <linux/init.h>
#include <linux/kernel.h>	// printk()
#include <linux/module.h>
#include <linux/moduleparam.h>	//module_param()
#include <linux/slab.h>		// kmalloc()...
#include <linux/stat.h>
#include <linux/types.h>    //dev_t,MKDEV ...
#include <linux/cdev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/capability.h>


#include "scull.h"

// 设置可在加载时设置的参数
int scull_major	=	SCULL_MAJOR;
int scull_minor	=	0;
int scull_nr_devs	=	SCULL_NR_DEVS;	//设备数量
int scull_quantum	=	SCULL_QUANTUM;
int scull_qset	=	SCULL_QSET;

module_param(scull_major, int ,S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);

MODULE_AUTHOR("Bangduo Chen");
MODULE_LICENSE("GPL");

struct scull_dev *scull_devices;    //在scull_init_module中申请

// 清理struct scull_dev结构
int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *next, *dptr;
    int qset = dev->qset;
    int i;

    // 所有项目
    for (dptr = dev->data; dptr; dptr = next) {
        if (dptr->data) {
            for (i=0; i < qset; i++)
                kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next;
        kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->data = NULL;
    return 0;
}

#ifdef SCULL_DEBUG // 打开调试以启用/proc文件

// /proc 文件读取函数
int scull_read_procmem(char *buf, char **start, off_t offset, int count,
                        int *eof, void *data)
{
    int i, j, len = 0;
    int limit = count - 80; // 不要获取超过这个值的数据

    for (i=0; i<scull_nr_devs && len <= limit; i++) {
        struct scull_dev *d = &scull_devices[i];
        struct scull_qset *qs = d->data;
        if (down_interruptible(&d->sem))
            return -ERESTARTSYS;
        len += sprintf(buf + len, "\nDevice %i: qset %i, q %i, sz %li\n",
                        i, d->qset, d->quantum, d->size);
        for (; qs && len <= limit; qs = qs->next) { // 扫描列表
            len += sprintf(buf + len, " item at %p, qset at %p\n",
                            qs, qs->data);
            if (qs->data && !qs->next) {
                //只打印最后一个项目
                for (j=0; j < d->qset; j++) {
                    if(qs->data[j]) {
                        len += sprintf(buf + len, " %4i: %8p\n", j, qs->data[j]);
                    }
                }
            }
        }
        up(&scull_devices[i].sem);
    }
    *eof = 1;
    return len;
}

// 采用seq_file接口实现
//以下是seq_file所需的迭代器方法
static void *scull_seq_start(struct seq_file *sfile, loff_t *pos)
{
    if (*pos >= scull_nr_devs)
        return NULL;
    return scull_devices + *pos;
}

static void *scull_seq_next(struct seq_file *sfile, void *v, loff_t *pos)
{
    (*pos)++;
    if (*pos >= scull_nr_devs)
        return NULL;
    return scull_devices + *pos;
}

static void scull_seq_stop(struct seq_file *sfile, void *v)
{
    //do nothing
}

static int scull_seq_show(struct seq_file *s, void *v)
{
    struct scull_dev *dev = (struct scull_dev *) v;
    struct scull_qset *d;
    int i;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    seq_printf(s, "\nDevice %i: qset %i, q %i, sz %li\n",
                (int)(dev - scull_devices), dev->qset, 
                dev->quantum, dev->size);
    for (d = dev->data; d; d = d->next) {
        // 遍历链表
        seq_printf(s, " item at %p, qset at %p\n",d, d->data);
        // 输出最后一项
        if (d->data && !d->next)
            for (i = 0; i < dev->qset; i++){
                if (d->data[i])
                    seq_printf(s, " %4i: %8p\n", i, d->data[i]);
            }
    }
    up(&dev->sem);
    return 0;
}

//打包迭代器
static struct seq_operations scull_seq_ops = {
    .start = scull_seq_start,
    .next = scull_seq_next,
    .stop = scull_seq_stop,
    .show = scull_seq_show
};

// 使用seq_file接口只需实现open方法
static int scull_proc_open(struct inode *inode, struct file *file)
{
    return seq_open(file, &scull_seq_ops);
}

//seq_file 文件操作结构
static struct file_operations scull_proc_ops = {
    .owner = THIS_MODULE,
    .open = scull_proc_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = seq_release
};


// 创建proc文件
static void scull_create_proc(void)
{
    struct proc_dir_entry *entry;
    create_proc_read_entry("scullmem", 0 /* 默认权限 */, NULL /* 父文件夹 */,
                            scull_read_procmem, NULL/* client */);
    entry = create_proc_entry("scullseq", 0, NULL);
    if (entry)
        entry->proc_fops = &scull_proc_ops;
}

// 移除proc文件
static void scull_remove_proc(void)
{
    // 如果proc文件没有注册,进行卸载不会导致问题
    remove_proc_entry("scullmem", NULL /* 父目录 */);
    remove_proc_entry("scullseq", NULL /* 父目录 */);
}

#endif // SCULL_DEBUG

// 打开设备
int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;

    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;

    // 如果以只写打开则将设备的长度设置为0
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
        if (down_interruptible(&dev->sem))
            return -ERESTARTSYS;
        scull_trim(dev);
        up(&dev->sem);
    }
    return 0;   // 打开成功
}

int scull_release(struct inode *inode, struct file *filp)
{
    return 0;
}

// 到达指定位置
struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;

    // 如果需要则申请一块内存
    if (!qs) {
        qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        if (qs == NULL)
            return NULL;
        memset(qs, 0, sizeof(struct scull_qset));
    }
    
    // 到达指定位置
    while (n--) {
        if(!qs->next) {
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (qs->next == NULL)
                return NULL;
            memset(qs->next, 0, sizeof(struct scull_qset));
        }
        qs = qs->next;
    }
    return qs;
}

// 读取数据
ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
                    loff_t *f_pos)
{
    PDEBUG("read some data\n");
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;    // 量子集列表的第一项
    int quantum = dev->quantum, qset = dev->qset;   // 总的数据量
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if (*f_pos + count >= dev->size)
        count = dev->size - *f_pos;

    // 在量子集中寻找链表项, qset索引以及偏移量
    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    // 到达指定的位置
    dptr = scull_follow(dev, item);

    if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
        goto out;

    // 只读到量子末尾
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;
out:
    up(&dev->sem);
    return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
                    loff_t *f_pos)
{
    PDEBUG("write some data\n");
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;    // 量子集列表的第一项
    int quantum = dev->quantum, qset = dev->qset;   // 总的数据量
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM; // 用于goto out 语句后的返回值

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
 

    // 在量子集中寻找链表项, qset索引以及偏移量
    item = (long)*f_pos / itemsize;
    rest = (long)*f_pos % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;
    
    // 到达指定位置
    dptr = scull_follow(dev, item);
    if (dptr == NULL)
        goto out;
    if(!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
        if (!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }
    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }

    // 只写到量子的末尾
    if (count > quantum - q_pos)
        count = quantum - q_pos;

    if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

    // 更新大小
    if (dev->size < *f_pos)
        dev->size = *f_pos;

out:
    up(&dev->sem);
    return retval;
}

// ioctl 函数
int scull_ioctl(struct inode *inode, struct file *filp,
                unsigned int cmd, unsigned long arg)
{
    PDEBUG("In ioctl function!\n");
    int err = 0, tmp;
    int retval = 0;
    // 获取类型和编号字段, 并拒绝错误的命令号
    // 在调用access_ok前返回ENOTTY(不恰当的ioctl)
    if (_IOC_TYPE(cmd) != SCULL_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > SCULL_IOC_MAXNR) return -ENOTTY;

    // 方向是一个位掩码,而VERIFY_WRITE用于R/W传输
    // "类型"是针对用户空间而言的,而access_ok是面向内核的
    // 因此,"读取" 和 "写入"的概念刚好相反
    // 当为从设备中读时,对于驱动程序来说是向用户空间写
    if (_IOC_DIR(cmd) & _IOC_READ)
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
        err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    if (err) return -EFAULT;
    
    switch(cmd) {
        case SCULL_IOCRESET:
            scull_quantum = SCULL_QUANTUM;
            scull_qset = SCULL_QSET;
            break;

        case SCULL_IOCSQUANTUM: // 通过指针设置
            if (!capable(CAP_SYS_ADMIN))
                return -EPERM;
            retval = __get_user(scull_quantum, (int __user *)arg);
            break;

        case SCULL_IOCTQUANTUM: // 通过参数直接设置
            if (!capable(CAP_SYS_ADMIN))
                return -EPERM;
            scull_quantum = arg;
            break;
        
        case SCULL_IOCGQUANTUM: // 通过指针返回
            retval = __put_user(scull_quantum, (int __user *)arg);
            break;

        case SCULL_IOCQQUANTUM: // 通过返回值返回
            retval = scull_quantum;
            break;
        
        case SCULL_IOCXQUANTUM: // 通过指针设置并返回
            if (!capable(CAP_SYS_ADMIN))
                return -EPERM;
            tmp = scull_quantum;
            retval = __get_user(scull_quantum, (int __user *)arg);
            if (retval == 0)
                retval = __put_user(tmp, (int __user *)arg);
            break;
        
        case SCULL_IOCHQUANTUM: // 通过参数设置, 返回值返回
            if (!capable(CAP_SYS_ADMIN))
                return -EPERM;
            tmp = scull_quantum;
            scull_quantum = arg;
            return tmp;

        case SCULL_IOCSQSET:
            if (! capable (CAP_SYS_ADMIN))
                return -EPERM;
            retval = __get_user(scull_qset, (int __user *)arg);
            break;

        case SCULL_IOCTQSET:
            if (! capable (CAP_SYS_ADMIN))
                return -EPERM;
            scull_qset = arg;
            break;

        case SCULL_IOCGQSET:
            retval = __put_user(scull_qset, (int __user *)arg);
            break;

        case SCULL_IOCQQSET:
            return scull_qset;

        case SCULL_IOCXQSET:
            if (! capable (CAP_SYS_ADMIN))
                return -EPERM;
            tmp = scull_qset;
            retval = __get_user(scull_qset, (int __user *)arg);
            if (retval == 0)
                retval = put_user(tmp, (int __user *)arg);
            break;

        case SCULL_IOCHQSET:
            if (! capable (CAP_SYS_ADMIN))
                return -EPERM;
            tmp = scull_qset;
            scull_qset = arg;
            return tmp;
        
        /**
         * 以下是scull_pipe的ioctl命令,只实现了一部分
         */

        case SCULL_P_IOCTSIZE:
            scull_p_buffer = arg;
            break;

        case SCULL_P_IOCQSIZE:
            return scull_p_buffer;

        default:  // 冗余, 因为cmd已根据MAXNR检查过了
            return -ENOTTY;
    }
    return retval;
}

// llseek 函数
loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
    struct scull_dev *dev = filp->private_data;
    loff_t newpos;

    switch(whence) {
        case 0: // SEEK_SET
            newpos = off;
            break;
        case 1: // SEEK_CUR
            newpos = filp->f_pos + off;
            break;
        case 2: // SEEK_END
            newpos = dev->size + off;
            break;
        default:    //不应发生
            return -EINVAL;
    }
    
    if (newpos < 0) return -EINVAL;
    filp->f_pos = newpos;
    return newpos;
}

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
 //   .llseek = scull_llseek,
    .read = scull_read,
    .write = scull_write,
   .ioctl = scull_ioctl,
    .open = scull_open,
    .release = scull_release,
};

// 清理相关数据结构,同时也作为初始化失败时的处理函数
// 因此,也需要小心清理,因为有些数据结构可能还未初始化
static void scull_cleanup_module(void)
{
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor);

    PDEBUG("scull exit\n");
    // 防止因初始化失败导致的未申请字符设备结构
    if (scull_devices) {
        for (i = 0; i< scull_nr_devs; i++){
            scull_trim(scull_devices + i);
            cdev_del(&scull_devices[i].cdev);
        }
        kfree(scull_devices);
    }
#ifdef SCULL_DEBUG
   scull_remove_proc();
#endif
    // 注销设备号
    unregister_chrdev_region(devno, scull_nr_devs);
    // 清理其他设备
   scull_p_cleanup();    
   scull_access_cleanup()  ;
}

// 设置字符设备结构
static void scull_setup_cdev(struct scull_dev *dev, int index)
{
    int err, devno = MKDEV(scull_major, scull_minor + index);
    cdev_init(&dev->cdev, &scull_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scull_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    // 如果出错,打印错误信息
    if (err)
        printk(KERN_NOTICE "Error %d adding scull%d\n", err, index);
}

static int scull_init_module(void)
{
    PDEBUG("scull init\n");
    int result, i;
    dev_t dev = 0;

    // 申请设备号
    if (scull_major) {
        dev = MKDEV(scull_major, scull_minor);
        result = register_chrdev_region(dev, scull_nr_devs, "scull");
    } else {
        result = alloc_chrdev_region(&dev, 0, scull_nr_devs, "scull");
        scull_major = MAJOR(dev);
    }
    if (result < 0) {
        printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
        return result;
    }

    /*
     * 申请设备结构
     */
    scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if(!scull_devices) {
        result = -ENOMEM;
        goto fail;
    }
    memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

    /* 
     * 初始化设备
     */
    for (i = 0; i< scull_nr_devs; i++) {
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
        init_MUTEX(&scull_devices[i].sem);
        scull_setup_cdev(&scull_devices[i], i);
    }

    // 初始化其他设备,pipe和access
   dev = MKDEV(scull_major, scull_minor + scull_nr_devs);
   dev += scull_p_init(dev);
   dev += scull_access_init(dev);
#ifdef SCULL_DEBUG  //调试时启用
   scull_create_proc();
#endif
    return 0;   //初始化成功
fail:
    scull_cleanup_module();
    return result;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);
