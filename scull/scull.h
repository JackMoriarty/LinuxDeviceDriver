/**
 * scull.h -- 字符模块相关定义
 * @Author: Bangduo Chen 
 * @Date: 2018-09-06 16:47:45 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-17 22:34:32
 */

#ifndef _SCULL_H_
#define _SCULL_H_

#include <linux/ioctl.h> // _IOW等宏需要的头文件

// 与调试输出相关的宏

#undef PDEBUG // 取消PDEBUG的生命(防止以前定义过)
#ifdef SCULL_DEBUG
	#ifdef __KERNEL__
        // 此时调试打开,并且在内核空间
        #define PDEBUG(fmt, args...) printk(KERN_DEBUG"scull: " fmt, ## args)
    #else
		// 用于用户空间
		#define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
	#endif
#else
	// 调试关闭
	#define PDEBUG(fmt, args...) // 不打印任何信息
#endif

// 定义主设备号
#ifndef SCULL_MAJOR
#define SCULL_MAJOR 0 // 当主设备号为0时,默认使用动态主设备号(在init函数中实现)
#endif

// 定义设备数量
#ifndef SCULL_NR_DEVS
#define SCULL_NR_DEVS 4 // scull[0-3]
#endif

#ifndef SCULL_P_NR_DEVS
#define SCULL_P_NR_DEVS 4 // scullpipe[0-3]
#endif

/**
 *  定义量子集个数和量子大小
 * scull_dev->data 指向了一个指针数组
 * 该数组有SCULL_QSET项,每个指针执行的内存大小为SCULL_QUANTUM字节
 */
#ifndef SCULL_QUANTUM
#define SCULL_QUANTUM 4000
#endif

#ifndef SCULL_QSET
#define SCULL_QSET	1000
#endif

/**
 * pipe设备的存储区域是一个简单的唤醒缓冲区
 * 此处设置大小
 */
#ifndef SCULL_P_BUFFER
#define SCULL_P_BUFFER 4000
#endif

// 量子集数组单项标识
struct scull_qset {
	void **data;
	struct scull_qset *next;
};

// scull字符设备结构
struct scull_dev {
	struct scull_qset *data;	// 指向第一个量子集的指针
	int quantum;				// 当前量子大小
	int qset;					// 当前量子集数组大小
	unsigned long size;			// 存储的数据大小
	unsigned int access_key;	// 供sculluid 和 scullpriv使用
	struct semaphore sem;		// 信号量
	struct cdev cdev;			// 字符设备结构(内核使用)
};

// 将 minors 分为两部分
#define TYPE(minor)	((minor) >> 4) & 0xf)	// 高4位
#define NUM(minor)	((minor) & 0xf)			// 底4位

/**
 *  不同设备使用的配置参数
 */
// main.c
extern int scull_major;
extern int scull_nr_devs;
extern int scull_quantum;
extern int scull_qset;
// pipe.c
extern int scull_p_buffer;

// 函数原型
int scull_p_init(dev_t dev);
void scull_p_cleanup(void);
int scull_access_init(dev_t dev);
void scull_access_cleanup(void);

int scull_strim(struct scull_dev *dev);

ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
					loff_t *f_pos);
ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
					loff_t *fops);
loff_t scull_llseek(struct file *filp, loff_t off, int whence);
int scull_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
				unsigned long arg);
int scull_trim(struct scull_dev *dev);

/**
 *  ioctl 相关定义
 */
// 使用x作为幻数
#define SCULL_IOC_MAGIC 'x'	//此处数值一定是一字节

// 重置命令
#define SCULL_IOCRESET	_IO(SCULL_IOC_MAGIC, 0)

/**
 * S 代表通过指针Set
 * T 代表通过参数Tell
 * G 代表通过指针Get
 * Q 代表通过返回值Query
 * X 代表原子地交换(eXchange) G和S 
 * H 代表切换(sHift) T和Q
 */
#define SCULL_IOCSQUANTUM	_IOW(SCULL_IOC_MAGIC, 1, int)
#define SCULL_IOCSQSET		_IOW(SCULL_IOC_MAGIC, 2, int)
#define SCULL_IOCTQUANTUM	_IO(SCULL_IOC_MAGIC, 3)
#define SCULL_IOCTQSET		_IO(SCULL_IOC_MAGIC, 4)
#define SCULL_IOCGQUANTUM	_IOR(SCULL_IOC_MAGIC, 5, int)
#define SCULL_IOCGQSET		_IOR(SCULL_IOC_MAGIC, 6, int)
#define SCULL_IOCQQUANTUM	_IO(SCULL_IOC_MAGIC, 7)
#define SCULL_IOCQQSET		_IO(SCULL_IOC_MAGIC, 8)
#define SCULL_IOCXQUANTUM	_IOWR(SCULL_IOC_MAGIC, 9, int)
#define SCULL_IOCXQSET		_IOWR(SCULL_IOC_MAGIC, 10, int)
#define SCULL_IOCHQUANTUM	_IO(SCULL_IOC_MAGIC, 11)
#define SCULL_IOCHQSET		_IO(SCULL_IOC_MAGIC, 12)

/**
 * pipe设备的ioctl命令
 * 由于书中未提到,所以只提供两个命令
 */
#define SCULL_P_IOCTSIZE	_IO(SCULL_IOC_MAGIC, 13)
#define SCULL_P_IOCQSIZE	_IO(SCULL_IOC_MAGIC, 14)
/* 更多命令略 */

// 最大顺序标号
#define SCULL_IOC_MAXNR	14

#endif /* _SCULL_H_ */
