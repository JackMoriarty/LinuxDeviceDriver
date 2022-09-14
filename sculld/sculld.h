#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include "lddbus.h"

// 帮助debug的宏
#undef PDEBUG
#ifdef SCULLD_DEBUG
    #ifdef __KERNEL__
        #define PDEBUG(fmt, args...) printk( KERN_DEBUG "sculld: " fmt, ## args)
    #else
        define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
    #endif
#else
    #define PDEBUG(fmt, args...)
#endif // SCULLD_DEBUG

#define SCULLD_MAJOR 0
#define SCULLD_DEVS 4

#define SCULLD_ORDER    0
#define SCULLD_QSET 500

struct sculld_dev {
    void **data;
    struct sculld_dev *next;    // 下一个链表项
    int vmas;                   // 活动的映射
    int order;                  // 当前内存页申请的order
    int qset;                   // 当前qset大小
    size_t size;                // 32位能够满足使用
    struct semaphore sem;       // 互斥访问
    struct cdev cdev;           // 字符设备
    char devname[20];           // 设备名
    struct ldd_device ldev;     // ldd 总线设备
};

extern struct sculld_dev *sculld_devices;
extern struct file_operations sculld_fops;

extern int sculld_major;
extern int sculld_devs;
extern int sculld_order;
extern int sculld_qset;

int sculld_trim(struct sculld_dev *dev);
struct sculld_dev *sculld_follow(struct sculld_dev *dev, int n);

#ifdef SCULLD_DEBUG
    #define SCULLD_USE_PROC
#endif
