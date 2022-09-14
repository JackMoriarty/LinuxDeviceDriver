#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/aio.h>
#include <asm/uaccess.h>
#include "sculld.h"

int sculld_major    =   SCULLD_MAJOR;
int sculld_devs =   SCULLD_DEVS;
int sculld_qset =   SCULLD_QSET;
int sculld_order    =   SCULLD_ORDER;

module_param(sculld_major, int, 0);
module_param(sculld_devs, int, 0);
module_param(sculld_qset, int, 0);
module_param(sculld_order, int, 0);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");

struct sculld_dev *sculld_devices;

int sculld_trim(struct sculld_dev *dev);
void sculld_cleanup(void);

// ldd 驱动填充
static struct ldd_driver sculld_driver = {
    .version = "$Revision: 1.21 $",
    .module = THIS_MODULE,
    .driver = {
		.name = "sculld",
	},
};

// open 
int sculld_open(struct inode *inode, struct file *filp)
{
    struct sculld_dev *dev;

    dev = container_of(inode->i_cdev, struct sculld_dev, cdev);
    if((filp->f_flags & O_ACCMODE) == O_WRONLY) {
        if(down_interruptible(&dev->sem))
            return -ERESTARTSYS;
        sculld_trim(dev);
        up(&dev->sem);
    }

    filp->private_data = dev;
    
    return 0;
}

// close
int sculld_release(struct inode *inode, struct file *filp)
{
    return 0;
}

// 到达指定设备位置
struct sculld_dev *sculld_follow(struct sculld_dev *dev, int n)
{
    while(n--) {
        if(!dev->next) {
            dev->next = kmalloc(sizeof(struct sculld_dev), GFP_KERNEL);
            memset(dev->next, 0, sizeof(struct sculld_dev));
        }
        dev = dev->next;
    }
    return dev;
}

// 读/写
ssize_t sculld_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct sculld_dev *dev = filp->private_data;
    struct sculld_dev *dptr;
    int quantum = PAGE_SIZE << dev->order;
    int qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if(down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if(*f_pos + count > dev->size)
        count = dev->size - *f_pos;

    item = ((long) *f_pos) / itemsize;
    rest = ((long) *f_pos) % itemsize;
    s_pos = rest / quantum;
    q_pos = rest % quantum;

    dptr = sculld_follow(dev, item);

    if(!dptr->data)
        goto nothing;
    if(!dptr->data[s_pos])
        goto nothing;
    if(count > quantum - q_pos)
        count = quantum - q_pos;
    
    if(copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
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

ssize_t sculld_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct sculld_dev *dev = filp->private_data;
	struct sculld_dev *dptr;
	int quantum = PAGE_SIZE << dev->order;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;

    if(down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    
    item = ((long) *f_pos) / itemsize;
	rest = ((long) *f_pos) % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;
    
    dptr = sculld_follow(dev, item);
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto nomem;
		memset(dptr->data, 0, qset * sizeof(char *));
	}

    if(!dptr->data[s_pos]) {
        dptr->data[s_pos] = (void *) __get_free_pages(GFP_KERNEL, dptr->order);
        if (!dptr->data[s_pos])
            goto nomem;
        memset(dptr->data[s_pos], 0, PAGE_SIZE << dptr->order);
    }

    if (count > quantum - q_pos)
		count = quantum - q_pos;
	if (copy_from_user (dptr->data[s_pos]+q_pos, buf, count)) {
		retval = -EFAULT;
		goto nomem;
	}
	*f_pos += count;

	if (dev->size < *f_pos)
		dev->size = *f_pos;
	up (&dev->sem);
	return count;
  nomem:
	up (&dev->sem);
	return retval;
}

loff_t sculld_llseek (struct file *filp, loff_t off, int whence)
{
	struct sculld_dev *dev = filp->private_data;
	long newpos;

	switch(whence) {
	case 0: /* SEEK_SET */
		newpos = off;
		break;

	case 1: /* SEEK_CUR */
		newpos = filp->f_pos + off;
		break;

	case 2: /* SEEK_END */
		newpos = dev->size + off;
		break;

	default: /* can't happen */
		return -EINVAL;
	}
	if (newpos<0) return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}

struct file_operations sculld_fops = {
	.owner =     THIS_MODULE,
	.llseek =    sculld_llseek,
	.read =	     sculld_read,
	.write =     sculld_write,
	// .ioctl =     sculld_ioctl,
	// .mmap =	     sculld_mmap,
	.open =	     sculld_open,
	.release =   sculld_release,
	// .aio_read =  sculld_aio_read,
	// .aio_write = sculld_aio_write,
};

int sculld_trim(struct sculld_dev *dev)
{
	struct sculld_dev *next, *dptr;
	int qset = dev->qset;   /* "dev" is not-null */
	int i;

	if (dev->vmas) /* don't trim: there are active mappings */
		return -EBUSY;

	for (dptr = dev; dptr; dptr = next) { /* all the list items */
		if (dptr->data) {
			/* This code frees a whole quantum-set */
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					free_pages((unsigned long)(dptr->data[i]),
							dptr->order);

			kfree(dptr->data);
			dptr->data=NULL;
		}
		next=dptr->next;
		if (dptr != dev) kfree(dptr); /* all of them but the first */
	}
	dev->size = 0;
	dev->qset = sculld_qset;
	dev->order = sculld_order;
	dev->next = NULL;
	return 0;
}

static void sculld_setup_cdev(struct sculld_dev *dev, int index)
{
	int err, devno = MKDEV(sculld_major, index);
    
	cdev_init(&dev->cdev, &sculld_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &sculld_fops;
	err = cdev_add (&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

static ssize_t sculld_show_dev(struct device *ddev, char *buf)
{
	struct sculld_dev *dev = ddev->driver_data;

	return print_dev_t(buf, dev->cdev.dev);
}

static DEVICE_ATTR(dev, S_IRUGO, sculld_show_dev, NULL);

static void sculld_register_dev(struct sculld_dev *dev, int index)
{
	sprintf(dev->devname, "sculld%d", index);
	dev->ldev.name = dev->devname;
	dev->ldev.driver = &sculld_driver;
	dev->ldev.dev.driver_data = dev;
	register_ldd_device(&dev->ldev);
	device_create_file(&dev->ldev.dev, &dev_attr_dev);
}


int sculld_init(void)
{
    int result, i;
	dev_t dev = MKDEV(sculld_major, 0);
    if (sculld_major)
		result = register_chrdev_region(dev, sculld_devs, "sculld");
	else {
		result = alloc_chrdev_region(&dev, 0, sculld_devs, "sculld");
		sculld_major = MAJOR(dev);
	}
	if (result < 0)
		return result;
    
    register_ldd_driver(&sculld_driver);

    sculld_devices = kmalloc(sculld_devs*sizeof (struct sculld_dev), GFP_KERNEL);
	if (!sculld_devices) {
		result = -ENOMEM;
		goto fail_malloc;
	}
	memset(sculld_devices, 0, sculld_devs*sizeof (struct sculld_dev));
	for (i = 0; i < sculld_devs; i++) {
		sculld_devices[i].order = sculld_order;
		sculld_devices[i].qset = sculld_qset;
		sema_init (&sculld_devices[i].sem, 1);
		sculld_setup_cdev(sculld_devices + i, i);
		sculld_register_dev(sculld_devices + i, i);
	}
	return 0;
 fail_malloc:
	unregister_chrdev_region(dev, sculld_devs);
	return result;
}

void sculld_cleanup(void)
{
	int i;

	for (i = 0; i < sculld_devs; i++) {
		unregister_ldd_device(&sculld_devices[i].ldev);
		cdev_del(&sculld_devices[i].cdev);
		sculld_trim(sculld_devices + i);
	}
	kfree(sculld_devices);
	unregister_ldd_driver(&sculld_driver);
	unregister_chrdev_region(MKDEV (sculld_major, 0), sculld_devs);
}


module_init(sculld_init);
module_exit(sculld_cleanup);
