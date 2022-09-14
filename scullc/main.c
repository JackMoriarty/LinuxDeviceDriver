/*
 * main.c -- 裸设备, 使用slab高速缓冲
 * @Author: Bangduo Chen 
 * @Date: 2018-09-20 09:08:09 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-20 11:30:47
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/proc_fs.h>

#include "scullc.h"

int scullc_major    =   SCULLC_MAJOR;
int scullc_devs     =   SCULLC_DEVS;
int scullc_qset     =   SCULLC_QSET;
int scullc_quantum  =   SCULLC_QUANTUM;

module_param(scullc_major, int, 0);
module_param(scullc_devs, int, 0);
module_param(scullc_qset, int, 0);
module_param(scullc_quantum, int, 0);
MODULE_AUTHOR("Bangduo Chen");
MODULE_LICENSE("GPL");

// 声明高速缓存指针
kmem_cache_t *scullc_cache;

#ifdef SCULLC_USE_PROC

void scullc_proc_offset(char *buf, char **start, off_t *offset, int *len)
{
    if (*offset == 0)
        return;
    if (*offset >= *len) {
        // 还没到达
        *offset -= *len;
        *len = 0;
    } else {
        // 到达需要的范围
        *start = buf + *offset;
        *offset = 0;
    }
}

int scullc_read_procmem(char *buf, char **start, off_t offset,
                    int count, int *eof, void *data)
{
    int i, j, quantum, qset, len = 0;
    int limit = count - 80;     // 不要打印超过这个数量的数据
    struct scullc_dev *d;

    *start = buf;
    for (i = 0; i < scullc_devices; i++) {
        d = &scullc_devices[i];
        if (down_interruptible(&d->sem))
            return -ERESTARTSYS;
        qset = d->qset;
        quantum = d->quantum;
        len += sprintf(buf + len,"\nDevice %i: qset %i, quantum %i, sz %li\n",
                    i, qset, quantum, (long)(d->size));
        for (; d; d = d->next) {
            len += sprintf(buf + len,"  item at %p, qset at %p\n", d, d->data);
            scullc_proc_offset(buf, start, &offset, &len);
            if (len > limit)
                goto out;
            if (d->data && !d->next)    // 打印最后一项
                for (j = 0; j < qset; j++) {
                    if (d->data[j])
                        len += sprintf(buf + len, "     %4i:%8p\n", j, d->data[j]);
                    scullc_proc_offset(buf, start, &offset, &len);
                    if (len > limit)
                        goto out;
                }
        }
        out:
            up(&scullc_devices[i].sem);
            if (len > limit)
                break;
    }
    *eof = 1;
    return len;
}

#endif  // SCULLC_USE_PROC

// open 和 close
int scullc_open(struct inode *inode ,struct file *filp)
{
    struct scullc_dev *dev;
    
    // 获得设备结构体
    dev = container_of(inode->i_cdev, struct scullc_dev, cdev);

    // 如果以只写的方式打开则设置设备长度为0
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
        if (down_interruptible(&dev->sem))
            return -ERESTARTSYS;
        scullc_trim(dev);
        up (&dev->sem);
    }

    filp->private_data = dev;
    
    return 0;
}

int scullc_release (struct inode *inode, struct file *filp)
{
    return 0;
}

// 到达指定位置
struct scullc_dev *scullc_follow(struct scullc_dev *dev, int n)
{
    while (n--) {
        if (!dev->next) {
            dev->next = kmalloc(sizeof(struct scullc_dev), GFP_KERNEL);
            memset(dev->next, 0, sizeof(struct scullc_dev));
        }
        dev = dev->next;
    }
    return dev;
}

// 读取
ssize_t scullc_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct scullc_dev *dev = filp->private_data;
    struct scullc_dev *dptr;
    int quantum = dev->quantum;
    int qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, reset;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if (*f_pos > dev->size)
        goto nothing;
    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    // 定位
    item = ((long) *f_pos) / itemsize;
    rest = ((long) *f_pos) % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    // 返回定位位置指针
    dptr = scullc_follow(dev, item);

    if (!dptr->data)
        goto nothing;
    if (!dptr->data[s_pos])
        goto nothing;
    if (count > quantum - q_pos)
        count = quantum - q_pos;    // 只读到末尾

    if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto nothing;
    }
    up(&dev->sem);

    *f_pos += count;
    return count;
nothing:
    up(&dev->sem);
    return retval;
}

// 写函数
ssize_t scullc_write(struct file *filp, const char __user *buf, size_t count,
                    loff_t *f_pos)
{
    struct scullc_dev *dev = filp->private_data;
    struct scullc_dev *dptr;
    int quantum = dev->quantum;
    int qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;       // 此函数中最常见的错误

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    
    item = ((long) *f_pos) / itemsize;
    rest = ((long) *f_pos) % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % itemsize;

    // 移动到指定位置
    dptr = scullc_follow(dev, item);
    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
        if (!dptr->data)
            goto nomem;
        memset(dptr->data, 0, qset * sizeof(char *));
    }

    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmem_cache_alloc(scullc_cache, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto nomem;
            memset(dptr->data[s_pos], 0, scullc_quantum);
    }
    if (count > quantum - q_pos)
        count = quantum - q_pos;    // 只读到量子的末尾
    
    if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)){
        retval = -EFAULT;
        goto nomem;
    }

    *f_pos += count;

    // 更新
    if (dev->size < *f_pos)
        dev->size = *f_pos;
    up(&dev->sem);
    return count;
nomem:
    up(&dev->sem);
    return retval;
}

// ioctl 函数
int scullc_ioctl(struct inode *inode, struct file *filp,
                unsigned int cmd, unsigned long arg)
{
    int err = 0, ret = 0, tmp;
    // 检查命令
    if (_IOC_TYPE(cmd) != SCULLC_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > SCULLC_IOC_MAXNR) return -ENOTTY;

    // 检查用户提供的缓冲区是否具有相应读写权限
    if (_IOC_DIR(cmd) & _IOC_READ)
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
        err = !access_ok(VERIFY, (void __user *)arg, _IOC_SIZE(cmd));
    if (err)
        return -EFAULT;

    switch(cmd) {

        case SCULLC_IOCRESET:
            scullc_qset = SCULLC_QSET;
            scullc_quantum = SCULLC_QUANTUM;
            break;
        
        case SCULLC_IOCSQUANTUM:
            ret = __get_user(scullc_quantum, (int __user *) arg);
            break;
        
        case SCULLC_IOCTQUANTUM:
            scullc_quantum = arg;
            break;

        case SCULLC_IOCGQUANTUM:
            ret = __put_user(scullc_quantum, (int __user *)arg);
            break;
        
        case SCULLC_IOCQQUANTUM:
            return scullc_quantum;
        
        case SCULLC_IOCXQUANTUM:
            tmp = scullc_quantum;
            ret = __get_user(scullc_quantum, (int __user *)arg);
            if (ret == 0)
                ret = __put_user(tmp, (int __user *) arg);
            break;
        
        case SCULLC_IOCHQUANTUM:
            tmp = scullc_quantum;
            scullc_quantum = arg;
            return tmp;

        case SCULLC_IOCSQSET:
            ret = __get_user(scullc_qset, (int __user *) arg);
            break;

        case SCULLC_IOCTQSET:
            scullc_qset = arg;
            break;

        case SCULLC_IOCGQSET:
            ret = __put_user(scullc_qset, (int __user *)arg);
            break;

        case SCULLC_IOCQQSET:
            return scullc_qset;

        case SCULLC_IOCXQSET:
            tmp = scullc_qset;
            ret = __get_user(scullc_qset, (int __user *)arg);
            if (ret == 0)
                ret = __put_user(tmp, (int __user *)arg);
            break;

        case SCULLC_IOCHQSET:
            tmp = scullc_qset;
            scullc_qset = arg;
            return tmp;

        default:    // 冗余
            return -ENOTTY;
    }

    return ret;
}

// llseek 函数
loff_t scullc_llseek(struct file *filp, loff_t off, int whence)
{
    struct scullc_dev *dev = filp->private_data;
    long newpos;

    switch(whence) {
        case 0: // 相对于起始位置移动
            newpos = off;
            break;
        
        case 1: // 相对于当前位置移动
            newpos = filp->f_pos + off;
        
        case 2: // 相对于末尾位置移动
            newpos = dev->size + off;
            break;
        
        default: // 冗余
            return -EINVAL;
    }
    if (newpos < 0) return -INVAL;
    filp->f_pos = newpos;
    return newpos;
}

// 设备操作结构体
struct file_operations scullc_fops = {
    .owner  =   THIS_MODULE,
    .llseek =   scullc_llseek,
    .read   =   scullc_read,
    .write  =   scullc_write,
    .ioctl  =   scullc_ioctl,
    .open   =   scullc_open,
    .release    =   scullc_release,
    // .aio_read   =   scullc_aio_read,
    // .aio_write  =   scullc_aio_write,  
};

// 清理设备结构体内容
int sculc_trim(struct scullc_dev *dev)
{
    struct scullc_dev *next, *dptr;
    int qset = dev->qset;
    int i;

    // if (dev->vmas)      // 不清理, 仍存在活动的映射
    //     return -EBUSY;

    for (dptr = dev; dptr; dptr = next) {
        if (dptr->data) {
            for (i = 0; i < qset; i++) {
                if (dptr->data[i])
                    kmem_cache_free(scullc_cache, dptr->data[i]);
            }
        }
        next = dptr->next;
        if (dptr != dev) kfree(dptr);
    }
    dev->size = 0;
    dev->qset = scullc_qset;
    dev->quantum = scullc_quantum;
    dev->next = NULL;
    return 0;
}

static void scullc_setup_cdev(struct scullc_dev *dev, int index)
{
    int err, devno = MKDEV(scullc_major, index);

    cdev_init(&dev->cdev, &scullc_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scullc_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

int scullc_init(void)
{
    int result, i;
    dev_t dev = MKDEV(scullc_major, 0);

    // 注册住设备号, 或动态获取
    if (scullc_major)
    {
        result = register_chrdev_region(dev, scullc_devs, "scullc");
    } else {
        result = alloc_chrdev_region(&dev, 0, scullc_devs, "scullc");
        scullc_major = MAJOR(dev);
    }
    if (result < 0)
        return result;
    
    scullc_devices = kmalloc(scullc_devs * sizeof(struct scullc_dev));
    scullc_devices = kmalloc(scullc_devs * sizeof(struct scullc_dev));
    if (!scullc_devices) {
        result = -ENOMEM;
        goto fail_malloc;
    }
    memset(scullc_devices, 0, scullc_devs * sizeof(struct scullc_dev));

    for (i = 0; i < scullc_devs; i++) {
        scullc_devices[i].quantum = scullc_quantum;
        scullc_devices[i].qset = scullc_qset;
        sema_init(&scullc_devices[i].sem, 1);
        scullc_setup_cdev(scullc_devices + i, i);
    }

    scullc_cache = kmem_cache_create("scullc", scullc_quantum,
            0, SLAB_HWCACHE_ALIGN, NULL, NULL);
    if (!scullc_cache) {
        scullc_cleanup();
        return -ENOMEM;
    }

#ifdef SCULLC_USE_PROC
    create_proc_read_entry("scullcmem", 0, NULL, scullc_read_procmem, NULL);
#endif

    return 0;

    fail_malloc:
        unregister_chrdev_region(dev, scullc_devs);
        return result;
}

void scullc_cleanup(void)
{
    int i;

#ifdef SCULLC_USE_PROC
    remove_proc_entry("scullcmem", NULL);
#endif

    for (i = 0; i < scullc_devs; i++) {
		cdev_del(&scullc_devices[i].cdev);
		scullc_trim(scullc_devices + i);
	}
	kfree(scullc_devices);

	if (scullc_cache)
		kmem_cache_destroy(scullc_cache);
	unregister_chrdev_region(MKDEV (scullc_major, 0), scullc_devs);
}

module_init(scullc_init);
module_exit(scullc_cleanup);