// Tiny tty driver

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <asm/uaccess.h>

#define DRIVER_VERSION "v2.0"
#define DRIVER_AUTHOR "Bangduo Chen"
#define DRIVER_DESC "Tiny TTY driver"

// 模块信息
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define DELAY_TIME      HZ * 2 // 每秒两个字符
#define TINY_DATA_CHARACTER 't'

// 主次设备号
#define TINY_TTY_MAJOR      240 // 实验范围
#define TINY_TTY_MINORS     4   // 只有四个设备

// 设备结构体
struct tiny_serial {
    struct tty_struct *tty;        // 指向本设备tty的指针
    int open_count; // 端口打开计数
    struct semaphore sem;   // 本结构互斥访问
    struct timer_list *timer;

    // 供tiocmget 和 tiocmset 函数使用
    int msr;    // MSR副本
    int mcr;    // MCR副本

    // 供ioctl函数使用
    struct serial_struct serial;
    wait_queue_head_t wait;
    struct async_icount icount;
};

static struct tiny_serial *tiny_table[TINY_TTY_MINORS]; // 初始化为null

static void tiny_timer(unsigned long timer_data)
{
    struct tiny_serial *tiny = (struct tiny_serial *)timer_data;
    struct tty_struct *tty;
    int i;
    char data[1] = {TINY_DATA_CHARACTER};
    int data_size = 1;

    if(!tiny)
        return;
    
    tty = tiny->tty;

    // 发送数据给tty层, 从而让用户读取. 此处没有实际推送数据除非tty->low_latency 被设置
    for(i = 0; i < data_size; ++i) {
        if(tty->flip.count >= TTY_FLIPBUF_SIZE)
            tty_flip_buffer_push(tty);
        tty_insert_flip_char(tty, data[i], TTY_NORMAL);
    }
    tty_flip_buffer_push(tty);

    // 重新提交定时器
    tiny->timer->expires = jiffies + DELAY_TIME;
    add_timer(tiny->timer);
}

// 打开函数
static int tiny_open(struct tty_struct *tty, struct file *file)
{
    struct tiny_serial *tiny;
    struct timer_list *timer;
    int index;

    // 初始化指针, 以防发生错误
    tty->driver_data = NULL;

    // 获取与tty指针相关的串口对象
    index = tty->index;
    tiny = tiny_table[index];
    if(tiny == NULL) {
        // 第一次访问设备, 创建他
        tiny = kmalloc(sizeof(*tiny), GFP_KERNEL);
        if(!tiny)
            return -ENOMEM;
        init_MUTEX(&tiny->sem);
        tiny->open_count = 0;
        tiny->timer = NULL;

        tiny_table[index] = tiny;
    }

    // 互斥访问
    down(&tiny->sem);
    
    // 在tty结构中保存上述结构
    tty->driver_data = tiny;
    tiny->tty = tty;
    ++tiny->open_count;
    if(tiny->open_count == 1) {
        // 设备被第一次打开,做一切初始化工作
        if(!tiny->timer) {
            timer = kmalloc(sizeof(*timer), GFP_KERNEL);
            if(!timer) {
                up(&tiny->sem);
                return -ENOMEM;
            }
            tiny->timer = timer;
        }
        init_timer(tiny->timer);
        tiny->timer->data = (unsigned long) tiny;
        tiny->timer->expires = jiffies + DELAY_TIME;
        tiny->timer->function = tiny_timer;
        add_timer(tiny->timer);
    }

    up(&tiny->sem);
    return 0;
}

static void do_close(struct tiny_serial *tiny)
{
    down(&tiny->sem);

    if(!tiny->open_count) {
        // 端口未打开
        goto exit;
    }

    --tiny->open_count;
    if(tiny->open_count <= 0) {
        // 最后一个用户已经将端口关闭
        // 在这里做任何硬件相关操作
        
        // 关闭定时器
        del_timer(tiny->timer);
    }
exit:
    up(&tiny->sem);
}

// 关闭函数
static void tiny_close(struct tty_struct *tty, struct file *file)
{
    struct tiny_serial *tiny = tty->driver_data;

    if(tiny)
        do_close(tiny);
}

// 写函数
static int tiny_write(struct tty_struct *tty,
                const unsigned char *buffer, int count)
{
    struct tiny_serial *tiny = tty->driver_data;
    int i;
    int retval = -EINVAL;

    if(!tiny)
        return -ENODEV;
    
    down(&tiny->sem);

    if(!tiny->open_count)
        // 端口还未打开
        goto exit;

    // 将数据写入内核调试日志, 来伪装将数据发送出硬件端口
    printk(KERN_DEBUG "%s - ", __FUNCTION__);
    for (i = 0; i < count; ++i)
		printk("%02x ", buffer[i]);
	printk("\n");

exit:
    up(&tiny->sem);
    return retval;
}

static int tiny_write_room(struct tty_struct *tty)
{
    struct tiny_serial *tiny = tty->driver_data;
    int room = -EINVAL;

    if(!tiny)
        return -ENODEV;
    
    down(&tiny->sem);
    if(!tiny->open_count) {
        // 端口未打开
        goto exit;
    }
    room = 225;
exit:
    up(&tiny->sem);
    return room;
}

#define RELEVANT_IFLAG(iflag) ((iflag) & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

// 改变端口设置
static void tiny_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
    unsigned int cflag;

    cflag = tty->termios->c_cflag;

    // 检查是否真的需要更改
    if(old_termios) {
        if((cflag == old_termios->c_cflag) && 
        (RELEVANT_IFLAG(tty->termios->c_iflag) == 
         RELEVANT_IFLAG(old_termios->c_iflag))) {
             printk(KERN_DEBUG " - nothing to change...\n");
             return;
         }
    }

    // 获取字节大小
    switch(cflag & CSIZE) {
        case CS5:
            printk(KERN_DEBUG " - data bits = 5\n");
            break;
        case CS6:
            printk(KERN_DEBUG " - data bits = 6\n");
            break;
        case CS7:
            printk(KERN_DEBUG " - data bits = 7\n");
            break;
        default:
        case CS8:
            printk(KERN_DEBUG " - data bits = 8\n");
            break;
    }

    // 判断奇偶
    if(cflag & PARENB)
        if(cflag & PARODD)
            printk(KERN_DEBUG " - parity = odd\n");
        else
            printk(KERN_DEBUG " - parity = even\n");
    else
        printk(KERN_DEBUG " - parity = none");
    
    // 确定需要的停止位
    if(cflag & CSTOPB)
        printk(KERN_DEBUG " - stop bits = 2\n");
    else
        printk(KERN_DEBUG " - stop bits = 1\n");

    // 确定硬件流控制设置
    if(cflag & CRTSCTS)
        printk(KERN_DEBUG " - RTS/CTS is enabled\n");
    else
        printk(KERN_DEBUG " - RTS/CTS is disabled\n");

    // 确定软件流控制
    // 如果实现了XON/XOFF, 设置设备中的开始和结束字符
    if(I_IXOFF(tty) || I_IXON(tty)) {
        unsigned char stop_char = STOP_CHAR(tty);
        unsigned char start_char = START_CHAR(tty);

        // 如果实现了INBOUND XON/XOFF
        if(I_IXOFF(tty))
            printk(KERN_DEBUG " - INBOUND XON?XOFF is enabled, "
                    "XON = %2x, XOFF = %2x", start_char, stop_char);
        else
            printk(KERN_DEBUG " - INBOUND XON/XOFF is disabled");
        
        // 如果实现了OUTBOUND XON/XOFF
        if(I_IXON(tty))
            printk(KERN_DEBUG " - OUTBOUND XON/XOFF is enabled, "
                "XON = %2x, XOFF = %2x", start_char, stop_char);
        else
            printk(KERN_DEBUG "- OUTBOUND XON/XOFF is disabled");
    }

    // 获取波特率
    printk(KERN_DEBUG " - baud rate = %d", tty_get_baud_rate(tty));
}

//虚假的UART值
#define MCR_DTR     0x01
#define MCR_RTS     0x02
#define MCR_LOOP    0x04
#define MSR_CTS     0x08
#define MSR_CD      0x10
#define MSR_RI      0x20
#define MSR_DSR     0x40

// 获取控制线路参数
static int tiny_tiocmget(struct tty_struct *tty, struct file *file)
{
    struct tiny_serial *tiny = tty->driver_data;

    unsigned int result = 0;
    unsigned int msr = tiny->msr;
    unsigned int mcr = tiny->mcr;

    result = ((mcr & MCR_DTR)   ? TIOCM_DTR   : 0) |    // 设置了DTR
             ((mcr & MCR_RTS)   ? TIOCM_RTS   : 0) |    // 设置了RTS
             ((mcr & MCR_LOOP)  ? TIOCM_LOOP  : 0) |    // 设置了LOOP
             ((msr & MSR_CTS)   ? TIOCM_CTS   : 0) |    // 设置了CTS
             ((msr & MSR_CD)    ? TIOCM_CAR   : 0) |    // 设置了Carrier detect
             ((msr & MSR_RI)    ? TIOCM_RI    : 0) |    // 设置了Ring Indicator
             ((msr & MSR_DSR)   ? TIOCM_DSR   : 0);
    return result;
}

// 设置控制线路参数
static int tiny_tiocmset(struct tty_struct *tty, struct file *file,
                         unsigned int set, unsigned int clear)
{
    struct tiny_serial *tiny = tty->driver_data;
    unsigned int mcr = tiny->mcr;

    if(set & TIOCM_RTS)
        mcr |= MCR_RTS;
    if(set & TIOCM_DTR)
        mcr |= MCR_RTS;
    
    if(clear & TIOCM_RTS)
        mcr &= ~MCR_RTS;
    if(clear & TIOCM_DTR)
        mcr &= ~MCR_RTS;

    // 设置设备中新的MCT值
    tiny->mcr = mcr;
    return 0;
}

// proc 文件读取函数
static int tiny_read_proc(char *page, char **start, off_t off, int count,
                        int *eof, void *data)
{
    struct tiny_serial *tiny;
    off_t begin = 0;
    int length = 0;
    int i;

    length += sprintf(page, "tinyserinfo:1.0 driver:%s\n", DRIVER_VERSION);
    for (i = 0; i < TINY_TTY_MINORS && length < PAGE_SIZE; ++i) {
        tiny = tiny_table[i];
        if(tiny == NULL)
            continue;
        length += sprintf(page + length, "%d\n", i);
        if((length + begin) > (off + count))
            goto done;
        if((length + begin) < off) {
            begin += length;
            length = 0;
        }
    }
    *eof = 1;
done:
    if(off >= (length + begin))
        return 0;
    *start = page + (off - begin);
    return (count < begin+length-off) ? count : begin + length-off;
}

// 获取串行线路信息
#define tiny_ioctl tiny_ioctl_tiocgserial
static int tiny_ioctl(struct tty_struct *tty, struct file *file,
                    unsigned int cmd, unsigned long arg)
{
    struct tiny_serial *tiny = tty->driver_data;

    if(cmd == TIOCGSERIAL) {
        struct serial_struct tmp;

        if(!arg)
            return -EFAULT;
        
        memset(&tmp, 0, sizeof(tmp));

        tmp.type    =   tiny->serial.type;
        tmp.line    =   tiny->serial.line;
        tmp.port    =   tiny->serial.port;
        tmp.irq     =   tiny->serial.irq;
        tmp.flags		= ASYNC_SKIP_TEST | ASYNC_AUTO_IRQ;
		tmp.xmit_fifo_size	= tiny->serial.xmit_fifo_size;
		tmp.baud_base		= tiny->serial.baud_base;
		tmp.close_delay		= 5*HZ;
		tmp.closing_wait	= 30*HZ;
		tmp.custom_divisor	= tiny->serial.custom_divisor;
		tmp.hub6		= tiny->serial.hub6;
		tmp.io_type		= tiny->serial.io_type;

        if(copy_to_user((void __user *)arg, &tmp, sizeof(struct serial_struct)))
            return -EFAULT;
        return 0;
    }
    return -ENOIOCTLCMD;
}
#undef tiny_ioctl

// 等待MSR的变化
#define tiny_ioctl tiny_ioctl_tiocmiwait
static int tiny_ioctl(struct tty_struct *tty, struct file *file,
                    unsigned int cmd, unsigned long arg)
{
    struct tiny_serial *tiny = tty->driver_data;

    if(cmd == TIOCMIWAIT) {
        DECLARE_WAITQUEUE(wait, current);
        struct async_icount cnow;
        struct async_icount cprev;

        cprev = tiny->icount;
        while(1) {
            add_wait_queue(&tiny->wait, &wait);
            set_current_state(TASK_INTERRUPTIBLE);
            schedule();
            remove_wait_queue(&tiny->wait, &wait);

            // 查看是否是信号唤醒了我们
            if (signal_pending(current))
                return -ERESTARTSYS;
            
            cnow = tiny->icount;
            if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
			    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
                return -EIO;    // 没有改变,返回错误
            if (((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
			    ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
			    ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
			    ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
				return 0;
			}
            cprev = cnow;
        }
    }
    return -ENOIOCTLCMD;
}
#undef tiny_ioctl

// 获得中断计数
#define tiny_ioctl tiny_ioctl_tiocgicount
static int tiny_ioctl(struct tty_struct *tty, struct file *file,
                        unsigned int cmd, unsigned long arg)
{
    struct tiny_serial *tiny = tty->driver_data;

    if (cmd == TIOCGICOUNT) {
		struct async_icount cnow = tiny->icount;
		struct serial_icounter_struct icount;

		icount.cts	= cnow.cts;
		icount.dsr	= cnow.dsr;
		icount.rng	= cnow.rng;
		icount.dcd	= cnow.dcd;
		icount.rx	= cnow.rx;
		icount.tx	= cnow.tx;
		icount.frame	= cnow.frame;
		icount.overrun	= cnow.overrun;
		icount.parity	= cnow.parity;
		icount.brk	= cnow.brk;
		icount.buf_overrun = cnow.buf_overrun;

		if (copy_to_user((void __user *)arg, &icount, sizeof(icount)))
			return -EFAULT;
		return 0;
	}
	return -ENOIOCTLCMD;
}
#undef tiny_ioctl

// 真实的ioctl函数
static int tiny_ioctl(struct tty_struct *tty, struct file *file,
                      unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case TIOCGSERIAL:
		return tiny_ioctl_tiocgserial(tty, file, cmd, arg);
	case TIOCMIWAIT:
		return tiny_ioctl_tiocmiwait(tty, file, cmd, arg);
	case TIOCGICOUNT:
		return tiny_ioctl_tiocgicount(tty, file, cmd, arg);
	}

	return -ENOIOCTLCMD;
}

static struct tty_operations serial_ops = {
	.open = tiny_open,
	.close = tiny_close,
	.write = tiny_write,
	.write_room = tiny_write_room,
	.set_termios = tiny_set_termios,
};

static struct tty_driver *tiny_tty_driver;

static int __init tiny_init(void)
{
    int retval;
    int i;

    // 分配tty驱动程序
    tiny_tty_driver = alloc_tty_driver(TINY_TTY_MINORS);
    if (!tiny_tty_driver)
        return -ENOMEM;
    
    // 初始化驱动
    tiny_tty_driver->owner = THIS_MODULE;
	tiny_tty_driver->driver_name = "tiny_tty";
	tiny_tty_driver->name = "ttty";
	tiny_tty_driver->devfs_name = "tts/ttty%d";
	tiny_tty_driver->major = TINY_TTY_MAJOR,
	tiny_tty_driver->type = TTY_DRIVER_TYPE_SERIAL,
	tiny_tty_driver->subtype = SERIAL_TYPE_NORMAL,
	tiny_tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_NO_DEVFS,
	tiny_tty_driver->init_termios = tty_std_termios;
	tiny_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(tiny_tty_driver, &serial_ops);

    // 另一种方式(但应采用tty_set_operations的方式)
    tiny_tty_driver->read_proc = tiny_read_proc;
	tiny_tty_driver->tiocmget = tiny_tiocmget;
	tiny_tty_driver->tiocmset = tiny_tiocmset;
	tiny_tty_driver->ioctl = tiny_ioctl;

    // 注册tty驱动程序
    retval = tty_register_driver(tiny_tty_driver);
    if (retval) {
		printk(KERN_ERR "failed to register tiny tty driver");
		put_tty_driver(tiny_tty_driver);
		return retval;
	}
    for (i = 0; i < TINY_TTY_MINORS; ++i)
		tty_register_device(tiny_tty_driver, i, NULL);

	printk(KERN_INFO DRIVER_DESC " " DRIVER_VERSION);
	return retval;
}

static void __exit tiny_exit(void)
{
	struct tiny_serial *tiny;
	int i;

	for (i = 0; i < TINY_TTY_MINORS; ++i)
		tty_unregister_device(tiny_tty_driver, i);
	tty_unregister_driver(tiny_tty_driver);

	//关闭所有定时器并清理内存
	for (i = 0; i < TINY_TTY_MINORS; ++i) {
		tiny = tiny_table[i];
		if (tiny) {
			// 关闭端口
			while (tiny->open_count)
				do_close(tiny);

			// 删除定时器
			del_timer(tiny->timer);
			kfree(tiny->timer);
			kfree(tiny);
			tiny_table[i] = NULL;
		}
	}
}

module_init(tiny_init);
module_exit(tiny_exit);
