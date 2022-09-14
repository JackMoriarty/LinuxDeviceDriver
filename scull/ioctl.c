#include <stdio.h>
#include <stropts.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/ioctl.h>


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


int main()
{
    int fd;
    fd = open("/dev/scull", O_RDWR);

    int quantum = 1;
    ioctl(fd, SCULL_IOCSQUANTUM, &quantum);
    quantum = 2;
    ioctl(fd, SCULL_IOCGQUANTUM, &quantum);
    // 应该输出1
    printf("%d\n", quantum);

    quantum = 2;
    ioctl(fd, SCULL_IOCTQUANTUM, quantum);
    quantum = 3;
    quantum = ioctl(fd, SCULL_IOCQQUANTUM);
    // 应该为2
    printf("%d\n", quantum);

    quantum = 3;
    ioctl(fd, SCULL_IOCQQUANTUM, &quantum);
    // 应该为2
    printf("%d\n", quantum);

    quantum = 4;
    ioctl(fd, SCULL_IOCHQUANTUM, quantum);
    // 应该为3
    printf("%d\n", quantum);

    quantum = ioctl(fd, SCULL_IOCQQUANTUM);
    // 应该为4
    printf("%d\n", quantum);
    return 0;
}
