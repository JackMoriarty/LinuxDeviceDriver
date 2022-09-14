/*
 * 其他的一些字符设备
 * @Author: Bangduo Chen 
 * @Date: 2018-09-17 16:34:46 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-17 22:47:23
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/tty.h>
#include <asm/atomic.h>

#include "scull.h"

static dev_t scull_a_firstdev;  // 此类设备起始设备号

/*********************single 设备***********************/
static struct scull_dev scull_s_device;
static atomic_t scull_s_available = ATOMIC_INIT(1);

// single 设备的打开方法
static int scull_s_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev = &scull_s_device;

    if (!atomic_dec_and_test(&scull_s_available)) {
        atomic_inc(&scull_s_available);
        return -EBUSY;  // 早已打开
    }

    // 判断打开权限
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);
    filp->private_data = dev;
    return 0;   // success
}

// release 方法
static int scull_s_release(struct inode *inode, struct file *filp)
{
    atomic_inc(&scull_s_available);
    return 0;
}
// scullsingle设备的操作
struct file_operations scull_sngl_fops = {
	.owner =	THIS_MODULE,
	.llseek =     	scull_llseek,
	.read =       	scull_read,
	.write =      	scull_write,
	.ioctl =      	scull_ioctl,
	.open =       	scull_s_open,
	.release =    	scull_s_release,
};

/***********************sculluid 设备************************/
static struct scull_dev scull_u_device;
static int scull_u_count;   // 默认初始化为0
static uid_t scull_u_owner; // 默认初始化为0
static spinlock_t scull_u_lock = SPIN_LOCK_UNLOCKED;

// open 方法
static int scull_u_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev = &scull_s_device;
    spin_lock(&scull_u_lock);
    if (scull_u_count &&
            (scull_u_owner != current->uid) &&  // 允许的用户
            (scull_u_owner != current->euid) && // 允许使用了su的用户
            !capable(CAP_DAC_OVERRIDE)) {       // 也允许root用户
        spin_unlock(&scull_u_lock);
        return -EBUSY;
    }

    if (scull_u_count == 0)
        scull_u_owner = current->uid;   // 授权
    
    scull_u_count++;
    spin_unlock(&scull_u_lock);

    if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
		scull_trim(dev);
	filp->private_data = dev;
	return 0;          // 成功
}

// release 方法
static int scull_u_release(struct inode *inode, struct file *filp)
{
    spin_lock(&scull_u_lock);
    scull_u_count--;
    spin_unlock(&scull_u_lock);
    return 0;
}

// 文件操作方法
struct file_operations scull_user_fops = {
	.owner =      THIS_MODULE,
	.llseek =     scull_llseek,
	.read =       scull_read,
	.write =      scull_write,
	.ioctl =      scull_ioctl,
	.open =       scull_u_open,
	.release =    scull_u_release,
};
/***************替代EBUSY的阻塞型OPEN, 基于uid设备********************/
static struct scull_dev scull_w_device;
static int scull_w_count;   // 默认初始化为0
static uid_t scull_w_owner; // 默认初始化为0
static DECLARE_WAIT_QUEUE_HEAD(scull_w_wait);
static spinlock_t scull_w_lock = SPIN_LOCK_UNLOCKED;

// 判断该设备是否可用
static inline int scull_w_available(void)
{
    return scull_w_count == 0 ||
        scull_w_owner == current->uid ||
        scull_w_owner == current->euid ||
        capable(CAP_DAC_OVERRIDE);
}

// open 方法
static int scull_w_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev = &scull_w_device;

    spin_lock(&scull_w_lock);
    while(!scull_w_available()) {
        spin_unlock(&scull_w_lock);
        if (filp->f_flags & O_NONBLOCK) return -EAGAIN;
        if (wait_event_interruptible(scull_w_wait, scull_w_available()))
            return -ERESTARTSYS;
        spin_lock(&scull_w_lock);
    }

    if (scull_w_count == 0)
        scull_w_owner = current->uid;   // 授权
    scull_w_count++;
    spin_unlock(&scull_w_lock);
    
    if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
		scull_trim(dev);
	filp->private_data = dev;
	return 0;          // 成功
}

static int scull_w_release(struct inode *inode, struct file *filp)
{
    int temp;

    spin_lock(&scull_w_lock);
    scull_w_count--;
    temp = scull_w_count;
    spin_unlock(&scull_w_lock);

    if (temp == 0)
        wake_up_interruptible_sync(&scull_w_wait);  // 通知其他用户的等待进程
    return 0;
}

struct file_operations scull_wusr_fops = {
	.owner =      THIS_MODULE,
	.llseek =     scull_llseek,
	.read =       scull_read,
	.write =      scull_write,
	.ioctl =      scull_ioctl,
	.open =       scull_w_open,
	.release =    scull_w_release,    
};

/*************************打开时复制设备**********************/
struct scull_listitem {
    struct scull_dev device;
    dev_t key;
    struct list_head list;
};

// 设备的链表, 以及保护它的锁
static LIST_HEAD(scull_c_list);
static spinlock_t scull_c_lock = SPIN_LOCK_UNLOCKED;

static struct scull_dev scull_c_device;

// 搜索设备, 如果没有则创建
static struct scull_dev *scull_c_lookfor_device(dev_t key)
{
    struct scull_listitem *lptr;

    list_for_each_entry(lptr, &scull_c_list, list) {
        if (lptr->key == key)
            return &(lptr->device);
    }
    
    // 没找到
    lptr = kmalloc(sizeof(struct scull_listitem), GFP_KERNEL);
    if (!lptr)
        return NULL;
    
    // 初始化设备
    memset(lptr, 0, sizeof(struct scull_listitem));
    lptr->key = key;
    scull_trim(&(lptr->device));
    init_MUTEX(&(lptr->device.sem));

    // 添加到列表中
    list_add(&lptr->list, &scull_c_list);
    return &(lptr->device);
}

// open 方法
static int scull_c_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;
    dev_t key;

    if (!current->signal->tty) {
        PDEBUG("Process \"%s\" has no ctl tty\n", current->comm);
        return -EINVAL;
    }
    key = tty_devnum(current->signal->tty);
    
    // 从列表中查找scullc设备
    spin_lock(&scull_c_lock);
    dev = scull_c_lookfor_device(key);
    spin_unlock(&scull_c_lock);

    if (!dev)
        return -ENOMEM;
    if ( (filp->f_flags & O_ACCMODE) == O_WRONLY)
		scull_trim(dev);
	filp->private_data = dev;
	return 0;          // 成功

}

// release 
static int scull_c_release(struct inode *inode, struct file *filp)
{
    // 此设备是永久设备, release 方法为空
    return 0;
}

// 方法
struct file_operations scull_priv_fops = {
	.owner =    THIS_MODULE,
	.llseek =   scull_llseek,
	.read =     scull_read,
	.write =    scull_write,
	.ioctl =    scull_ioctl,
	.open =     scull_c_open,
	.release =  scull_c_release,
};

/***********************初始化等其他方法****************************************/
// 用于初始化和清除函数调用的设备信息
static struct scull_adev_info {
    char *name;
    struct scull_dev *sculldev;
    struct file_operations *fops;
} scull_access_devs[] = {
    { "scullsingle", &scull_s_device, &scull_sngl_fops },
    { "sculluid", &scull_u_device, &scull_user_fops },
    { "scullwuid", &scull_w_device, &scull_wusr_fops },
    { "scullpriv", &scull_c_device, &scull_priv_fops }
};
#define SCULL_N_ADEVS 4

// 设置设备
static void scull_access_setup(dev_t devno, struct scull_adev_info *devinfo)
{
    struct scull_dev *dev = devinfo->sculldev;
    int err;

    // 初始化设备结构
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    init_MUTEX(&dev->sem);

    // cdev 填充
    cdev_init(&dev->cdev, devinfo->fops);
    kobject_set_name(&dev->cdev.kobj, devinfo->name);
    dev->cdev.owner = THIS_MODULE;
    err = cdev_add(&dev->cdev, devno, 1);
    // 优雅的报错
    if (err) {
        printk(KERN_NOTICE "Error %d adding %s\n", err, devinfo->name);
        kobject_put(&dev->cdev.kobj);
    } else {
        printk(KERN_NOTICE "%s registered at %x\n", devinfo->name, devno);
    }
}
// 初始化方法
int scull_access_init(dev_t firstdev)
{
    int result, i;
    // 注册设备号
    result = register_chrdev_region(firstdev, SCULL_N_ADEVS, "sculla");
    if (result < 0) {
        printk(KERN_WARNING "sculla: device number registration failed\n");
        return 0;
    }
    scull_a_firstdev = firstdev;

    // 设置设备
    for (i = 0; i < SCULL_N_ADEVS; i++)
        scull_access_setup(firstdev + i, scull_access_devs + i);
    return SCULL_N_ADEVS;
}

// 清理方法
void scull_access_cleanup(void)
{
    struct scull_listitem *lptr, *next;
    int i;

    // 清理所有静态设备
    for (i = 0; i< SCULL_N_ADEVS; i++) {
        struct scull_dev *dev = scull_access_devs[i].sculldev;
        cdev_del(&dev->cdev);
        scull_trim(scull_access_devs[i].sculldev);
    }

    // 清理所有复制的设备
    list_for_each_entry_safe(lptr, next, &scull_c_list, list) {
        list_del(&lptr->list);
        scull_trim(&(lptr->device));
        kfree(lptr);
    }

    // 释放设备号
    unregister_chrdev_region(scull_a_firstdev, SCULL_N_ADEVS);
    return;
}
