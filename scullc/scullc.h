/*
 * scullc 头文件
 * @Author: Bangduo Chen 
 * @Date: 2018-09-20 09:15:36 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-20 09:41:43
 */

#include <linux/ioctl.h>
#include <linux/cdev.h>

// 帮助调试的宏

#undef PDEBUG       // 以防万一, 取消定义
#ifdef SCULLC_DEBUG
    #ifdef __KERNEL__
        // 内核空间调试
        #define PDEBUG(fmt, args...)    printk(KERN_DEBUG "scullc" fmt, ## args)
    #else
        // 用户空间调试
        #define PDEBUG(fmt, args...)    fprintf(stderr, fmt, ## args)
    #endif
#else
    // 不调试
    #define PDEBUG(fmt, args...)    // nothing
#endif  // SCULLC_DEBUG

#define SCULLC_MAJOR    0   // 默认动态申请主设备号
#define SCULLC_DEVS     4   // scullc0 --> scullc3

#define SCULLC_QUANTUM  4000
#define SCULLC_QSET     500

struct scullc_dev {
  void **data;
  struct scullc_dev *next;  // 下一项
  int vmas;                 // 有效的映射
  int quantum;              // 当前量子大小
  int qset;                 // 当前量子集大小
  size_t size;              // 32-bit 足够
  struct semaphore sem;     // 互斥信号量
  struct cdev cdev;
};

extern struct scullc_dev *scullc_devices;
extern struct file_operations   scullc_fops;

extern int scullc_major;
extern int scullc_devs;
extern int scullc_order;
extern int scullc_qset;

int scullc_trim(struct scullc_dev *dev);
struct scullc_dev *scullc_follow(struct scullc_dev *dev, int n);

#ifdef SCULLC_DEBUG
    #define SCULLC_USE_PROC
#endif

// Ioctl 定义
// 定义魔数
#define SCULLC_IOC_MAGIC    'x'

// ioctl 命令
/*
 * S means "Set" through a ptr,
 * T means "Tell" directly
 * G means "Get" (to a pointed var)
 * Q means "Query", response is on the return value
 * X means "eXchange": G and S atomically
 * H means "sHift": T and Q atomically
 */
#define SCULLC_IOCSQUANTUM _IOW(SCULLC_IOC_MAGIC,  1, int)
#define SCULLC_IOCTQUANTUM _IO(SCULLC_IOC_MAGIC,   2)
#define SCULLC_IOCGQUANTUM _IOR(SCULLC_IOC_MAGIC,  3, int)
#define SCULLC_IOCQQUANTUM _IO(SCULLC_IOC_MAGIC,   4)
#define SCULLC_IOCXQUANTUM _IOWR(SCULLC_IOC_MAGIC, 5, int)
#define SCULLC_IOCHQUANTUM _IO(SCULLC_IOC_MAGIC,   6)
#define SCULLC_IOCSQSET    _IOW(SCULLC_IOC_MAGIC,  7, int)
#define SCULLC_IOCTQSET    _IO(SCULLC_IOC_MAGIC,   8)
#define SCULLC_IOCGQSET    _IOR(SCULLC_IOC_MAGIC,  9, int)
#define SCULLC_IOCQQSET    _IO(SCULLC_IOC_MAGIC,  10)
#define SCULLC_IOCXQSET    _IOWR(SCULLC_IOC_MAGIC,11, int)
#define SCULLC_IOCHQSET    _IO(SCULLC_IOC_MAGIC,  12)

#define SCULLC_IOC_MAXNR 12