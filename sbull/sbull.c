#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>

MODULE_LICENSE("Dual BSD/GPL");

static int sbull_major = 0;
module_param(sbull_major, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int ,0);
static int nsectors = 1024;
module_param(nsectors, int, 0);
static int ndevices = 4;
module_param(ndevices, int, 0);

// 不同的请求模式
enum {
    RM_SIMPLE   =   0,  // 简易版
    RM_FULL     =   1,  // 完全版
    RM_NOQUEUE  =   2,  // 使用make_request, 不使用request 队列
};

static int request_mode = RM_SIMPLE;
module_param(request_mode, int, 0);

// 此设备号和分区管理
#define SBULL_MINORS    16
#define MINOR_SHIFT     4
#define DEVNUM(kdevnum) (MINOR(kdev_t_to_nr(kdevnum)) >> MINOR_SHIFT)

#define KERNEL_SECTOR_SIZE  512

#define INVALIDATE_DELAY    30 * HZ

// 设备结构体
struct sbull_dev {
    int size;       // 以扇区为单位, 设备的到校
    u8 *data;       // 数据数组
    short users;    // 用户数量
    short media_change; // 介质改变标志
    spinlock_t lock;    // 用于互斥
    struct request_queue *queue;   // 设备请求队列
    struct gendisk *gd; // gendisk 结构
    struct timer_list timer;    // 用来模拟介质改变
};

static struct sbull_dev *Devices = NULL;

// 处理实际的IO操作
static void sbull_transfer(struct sbull_dev *dev, unsigned long sector,
        unsigned long nsect, char *buffer, int write)
{
    unsigned long offset = sector * KERNEL_SECTOR_SIZE;
    unsigned long nbytes = nsect * KERNEL_SECTOR_SIZE;

    if ((offset + nbytes) > dev->size) {
        printk(KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
        return;
    }
    if(write)
        memcpy(dev->data + offset, buffer, nbytes);
    else 
        memcpy(buffer, dev->data + offset, nbytes);
}

// 请求函数简易版
static void sbull_request(request_queue_t *q)
{
    struct request *req;

    while ((req = elv_next_request(q)) != NULL) {
        struct sbull_dev *dev = req->rq_disk->private_data;
        if(!blk_fs_request(req)) {
            printk(KERN_NOTICE"Skip non-fs request\n");
            end_request(req, 0);
            continue;
        }

        sbull_transfer(dev, req->sector, req->current_nr_sectors, req->buffer, rq_data_dir(req));
        end_request(req, 1);
    }
}

// 传输一个bio结构对应的IO
static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
{
    int i;
    struct bio_vec *bvec;
    sector_t sector = bio->bi_sector;

    // 对每个段单独操作
    bio_for_each_segment(bvec, bio, i) {
        char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
        sbull_transfer(dev, sector, bio_cur_sectors(bio), buffer, bio_data_dir(bio) == WRITE);
        sector += bio_cur_sectors(bio);
        __bio_kunmap_atomic(bio, KM_USER0);
    }
    return 0;
}

// 传输整个请求对应的IO
static int sbull_xfer_request(struct sbull_dev *dev, struct request * req)
{
    struct bio *bio;
    int nsect = 0;
    rq_for_each_bio(bio, req) {
        sbull_xfer_bio(dev, bio);
        nsect += bio->bi_size/KERNEL_SECTOR_SIZE;
    }
    return nsect;
}

// 处理整个请求队列
static void sbull_full_request(request_queue_t *q)
{
    struct request *req;
    int sectors_xferred;
    struct sbull_dev *dev = q->queuedata;

    while ((req = elv_next_request(q)) != NULL) {
        if (!blk_fs_request(req)) {
            printk(KERN_NOTICE"Skip non-fs requests\n");
            end_request(req, 0);
            continue;
        }
        sectors_xferred = sbull_xfer_request(dev, req);
        if (!end_that_request_first(req, 1, sectors_xferred)) {
            blkdev_dequeue_request(req);
            end_that_request_last(req);
        }
    }
}

// 不使用请求队列
static int sbull_make_request(request_queue_t *q, struct bio *bio)
{
    struct sbull_dev *dev = q->queuedata;
    int status;

    status = sbull_xfer_bio(dev, bio);
    bio_endio(bio, bio->bi_size, status);
    return 0;
}

// 打开和关闭函数
static int sbull_open(struct inode *inode, struct file *filp)
{
    struct sbull_dev *dev = inode->i_bdev->bd_disk->private_data;

    del_timer_sync(&dev->timer);
    filp->private_data = dev;
    spin_lock(&dev->lock);
    if(!dev->users)
        check_disk_change(inode->i_bdev);
    dev->users++;
    spin_unlock(&dev->lock);
    return 0;
}

static int sbull_release(struct inode *inode, struct file *filp)
{
    struct sbull_dev *dev = inode->i_bdev->bd_disk->private_data;

    spin_lock(&dev->lock);
    dev->users--;

    if(!dev->users) {
        dev->timer.expires = jiffies + INVALIDATE_DELAY;
        add_timer(&dev->timer);
    }
    spin_unlock(&dev->lock);
    return 0;
}

// 检查介质是否改变
int sbull_media_changed(struct gendisk *gd)
{
    struct sbull_dev *dev = gd->private_data;
    return dev->media_change;
}

// 介质改变后相应的操作
int sbull_revalidate(struct gendisk *gd)
{
    struct sbull_dev *dev = gd->private_data;

    if(dev->media_change) {
        dev->media_change = 0;
        memset(dev->data, 0, dev->size);
    }
    return 0;
}

// 定时器超时处理函数, 用以模拟介质取出
void sbull_invalidate(unsigned long ldev)
{
    struct sbull_dev *dev = (struct sbull_dev *) ldev;

    spin_lock(&dev->lock);
    if(dev->users || !dev->data)
        printk(KERN_WARNING"sbull: timer sanity check failed\n");
    else
        dev->media_change = 1;
    spin_unlock(&dev->lock);
}

// ioctl 函数
int sbull_ioctl(struct inode *inode, struct file *filp,
                unsigned int cmd, unsigned long arg)
{
    long size;
    struct hd_geometry geo;
    struct sbull_dev *dev = filp->private_data;

    switch(cmd) {
        case HDIO_GETGEO:
        size = dev->size * (hardsect_size/KERNEL_SECTOR_SIZE);
        geo.cylinders = (size & ~0x3f) >> 6;
        geo.heads = 4;
        geo.sectors = 16;
        geo.start = 4;
        if(copy_to_user((void __user *)arg, &geo, sizeof(geo)))
            return -EFAULT;
        return 0;
    }

    return -ENOTTY;
}

// 设备操作结构
static struct block_device_operations sbull_ops = {
    .owner      =   THIS_MODULE,
    .open       =   sbull_open,
    .release    =   sbull_release,
    .media_changed  =   sbull_media_changed,
    .revalidate_disk    =   sbull_revalidate,
    .ioctl      =   sbull_ioctl
};

// 配置设备
static void setup_device(struct sbull_dev *dev, int which)
{
    memset(dev, 0, sizeof(struct sbull_dev));
    dev->size = nsectors * hardsect_size;
    dev->data = vmalloc(dev->size);
    if(dev->data == NULL) {
        printk(KERN_NOTICE "vmalloc failure.\n");
        return;
    }
    spin_lock_init(&dev->lock);

    init_timer(&dev->timer);
    dev->timer.data = (unsigned long) dev;
    dev->timer.function = sbull_invalidate;

    // 使用不同的请求方式
    switch(request_mode) {
        case RM_NOQUEUE:
        dev->queue = blk_alloc_queue(GFP_KERNEL);
        if (dev->queue == NULL)
            goto out_vfree;
        blk_queue_make_request(dev->queue, sbull_make_request);
        break;

        case RM_FULL:
        dev->queue = blk_init_queue(sbull_full_request, &dev->lock);
        if(dev->queue == NULL)
            goto out_vfree;
        break;

        default:
        printk(KERN_NOTICE "Bad request mode %d, using simple\n", request_mode);

        case RM_SIMPLE:
        dev->queue = blk_init_queue(sbull_request, &dev->lock);
        if(dev->queue == NULL)
            goto out_vfree;
        break;
    }
    blk_queue_hardsect_size(dev->queue, hardsect_size);
    dev->queue->queuedata = dev;

    dev->gd = alloc_disk(SBULL_MINORS);
    if(!dev->gd) {
        printk(KERN_NOTICE "alloc_disk failure\n");
        goto out_vfree;
    }
    dev->gd->major = sbull_major;
    dev->gd->first_minor = which * SBULL_MINORS;
    dev->gd->fops = &sbull_ops;
    dev->gd->queue = dev->queue;
    dev->gd->private_data = dev;
    snprintf (dev->gd->disk_name, 32, "sbull%c", which + 'a');
    set_capacity(dev->gd, nsectors*(hardsect_size/KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;
out_vfree:
    if(dev->data)
        vfree(dev->data);
}

static int __init sbull_init(void)
{
    int i;

    sbull_major = register_blkdev(sbull_major, "sbull");
    if(sbull_major <= 0) {
        printk(KERN_WARNING "sbull: unable to get major number\n");
        return -EBUSY;
    }

    Devices = kmalloc(ndevices * sizeof(struct sbull_dev), GFP_KERNEL);
    if (Devices == NULL)
        goto out_unregister;
    for(i = 0; i < ndevices; i++)
        setup_device(Devices + i, i);
    
    return 0;

out_unregister:
    unregister_blkdev(sbull_major, "sbull");
    return -ENOMEM;
}

static void sbull_exit(void)
{
    int i;
    
    for(i = 0; i < ndevices; i++) {
        struct sbull_dev *dev = Devices + i;
        
        del_timer_sync(&dev->timer);
        if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
        if (dev->queue) {
			if (request_mode == RM_NOQUEUE)
				blk_put_queue(dev->queue);
			else
				blk_cleanup_queue(dev->queue);
		}
        if (dev->data)
			vfree(dev->data);
    }
    unregister_blkdev(sbull_major, "sbull");
	kfree(Devices);
}

module_init(sbull_init);
module_exit(sbull_exit);
