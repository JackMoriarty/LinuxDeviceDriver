/*
 * USB Skeleton driver - 2.0
 * @Author: Bangduo Chen 
 * @Date: 2018-09-30 10:13:52 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-09-30 21:58:01
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/param.h>

#include <asm/uaccess.h>

// 定义以下常量来帮助识别设备
#define USB_SKEL_VENDOR_ID  0xfff0
#define USB_SKEL_PRODUCT_ID 0xfff0

// 本驱动程序支持的设备列表
static struct usb_device_id skel_table [] = {
    { USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
    {  }            //  终止入口项
};
// 允许用户空间工具判断该驱动程序可以控制什么设备
MODULE_DEVICE_TABLE(usb, skel_table);

// 从usb核心获取 minor 范围
#define USB_SKEL_MINOR_BASE     192

// 包含所有需要设备相关项的结构体
struct usb_skel {
    struct usb_device *udev;     // 本设备的usb设备结构体
    struct usb_interface *interface;    // 本设备的接口
    unsigned char *bulk_in_buffer;      // 接收数据的缓冲区
    size_t bulk_in_size;        // 接收缓冲区的大小
    __u8 bulk_in_endpointAddr;  // 输入端口地址
    __u8 bulk_out_endpointAddr; // 输出端口地址
    struct kref kref;
};

// 通过kref成员获取整个 usb_skel 结构体
#define to_skel_dev(d)  container_of(d, struct usb_skel, kref)

static struct usb_driver skel_driver;

// 设备相关项结构体删除函数
static void skel_delete(struct kref *kref)
{
    // 获取 usb_skel 结构体
    struct usb_skel *dev = to_skel_dev(kref);

    // 释放对应的 usb_device 结构体
    usb_put_dev(dev->udev);
    // 释放对应缓冲区
    kfree(dev->bulk_in_buffer);
    // 释放设备相关项结构体
    kfree(dev);
}

// 设备打开函数
static int skel_open(struct inode *inode, struct file *file)
{
    struct usb_skel *dev;
    struct usb_interface *interface;
    int subminor;
    int retval = 0;

    // 获取次设备号
    subminor = iminor(inode);

    interface = usb_find_interface(&skel_driver, subminor);
    if (!interface) {
        err ("%s - error, can't find device for minor %d", __FUNCTION__, subminor);
        retval = -ENODEV;
        goto exit;
    }

    dev = usb_get_intfdata(interface);
    if(!dev) {
        retval = -ENODEV;
        goto exit;
    }

    // 增加设备的引用计数
    kref_get(&dev->kref);

    // 将设备保存到file指针中
    file->private_data = dev;

exit:
    return retval;
}

// release 函数
static int skel_release(struct inode *inode, struct file *file)
{
    struct usb_skel *dev;
    
    dev = (struct usb_skel *)file->private_data;
    if (dev == NULL)
        return -ENODEV;
    
    // 减少引用计数
    kref_put(&dev->kref, skel_delete);
    return 0;
}

// read 函数
static ssize_t skel_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
    struct usb_skel *dev;
    int retval = 0;

    dev = (struct usb_skel *)file->private_data;

    // 进行批量阻塞读已从设备中获取数据
    retval = usb_bulk_msg(dev->udev,
                    usb_rcvbulkpipe(dev->udev, dev->bulk_in_endpointAddr),
                    dev->bulk_in_buffer,
                    min(dev->bulk_in_size, count),
                    &count,
                    HZ * 10);
    
    // 如果读取成功
    if (!retval) {
        if (copy_to_user(buffer, dev->bulk_in_buffer, count))
            retval = -EFAULT;
        else retval = count;
    }

    return retval;
}

// urb 写函数回调处理例程
static void skel_write_bulk_callback(struct urb *urb, struct pt_regs *regs)
{
    // 同步或异步解绑错误不是错误
    if (urb->status &&
        !(urb->status == -ENOENT ||
          urb->status == -ECONNRESET ||
          urb->status == -ESHUTDOWN)) {
              dbg("%s - nonzero write bulk status received: %d",
                    __FUNCTION__, urb->status);
          }
    
    // 释放申请的缓冲区
    usb_buffer_free(urb->dev, urb->transfer_buffer_length,
                urb->transfer_buffer, urb->transfer_dma);
}

// 写函数
static ssize_t skel_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
    struct usb_skel *dev;
    int retval = 0;
    struct urb *urb = NULL;
    char *buf = NULL;

    dev = (struct usb_skel *)file->private_data;

    // 保证确实有数据可写
    if (count == 0)
        goto exit;

    // 创建urb, 并为其创建缓冲区, 然后将数据拷贝给urb
    urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!urb) {
        retval = -ENOMEM;
        goto error;
    }
    
    buf = usb_buffer_alloc(dev->udev, count, GFP_KERNEL, &urb->transfer_dma);
    if (!buf) {
        retval =ENOMEM;
        goto error;
    }
    if (copy_from_user(buf, user_buffer, count)) {
        retval = -EFAULT;
        goto error;
    }
    
    // 正确地初始化urb
    usb_fill_bulk_urb(urb, dev->udev,
            usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
            buf, count, skel_write_bulk_callback, dev);
    urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    
    // 将urb提交给usb核心, 以发送数据
    retval = usb_submit_urb(urb, GFP_KERNEL);
    if (retval) {
        err("%s - failed submitting write urb, error %d",__FUNCTION__, retval);
        goto error;
    }
    
    // 释放对urb的引用, USB核心最终会释放该结构体 buffer 将会被回调函数释放
    usb_free_urb(urb);
exit:
    return count;

error:
    usb_buffer_free(dev->udev, count, buf, urb->transfer_dma);
    usb_free_urb(urb);
    kfree(buf);
    return retval;

}

// USB 相关操作结构体
static struct file_operations skel_fops = {
    .owner = THIS_MODULE,
    .read = skel_read,
    .write = skel_write,
    .open = skel_open,
    .release = skel_release,
};

// 定义不同的参数以供USB核心使用
static struct usb_class_driver skel_class = {
    .name = "usb/skel%d",
    .fops = &skel_fops,
    .mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH,
    .minor_base = USB_SKEL_MINOR_BASE,
};

// 探测函数
static int skel_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct usb_skel *dev = NULL;
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    size_t buffer_size;
    int i;
    int retval = -ENOMEM;

    // 申请设备结构体
    dev = kmalloc(sizeof(struct usb_skel), GFP_KERNEL);
    if (dev == NULL) {
        err("Out of memory");
        goto error;
    }
    memset(dev, 0x00, sizeof(*dev));
    kref_init(&dev->kref);

    dev->udev = usb_get_dev(interface_to_usbdev(interface));
    dev->interface = interface;

    // 设置端点信息
    // 仅使用第一个批量IN和批量OUT端点
    iface_desc = interface->cur_altsetting;
    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;

        if (!dev->bulk_in_endpointAddr &&
            (endpoint->bEndpointAddress & USB_DIR_IN) && 
            ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
                == USB_ENDPOINT_XFER_BULK)) {
                    // 找到一个批量IN类型的端点
                    buffer_size = endpoint->wMaxPacketSize;
                    dev->bulk_in_size = buffer_size;
                    dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
                    dev->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
                    if (!dev->bulk_in_buffer) {
                        err("Could not allocate bulk_in_buffer");
                        goto error;
                    }
                }
        
        if (!dev->bulk_out_endpointAddr &&
            !(endpoint->bEndpointAddress & USB_DIR_IN) &&
            ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
                    == USB_ENDPOINT_XFER_BULK)) {
                        // 找到第一个批量OUT类型端点
                        dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
                    }
    }
    if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
        err("Could not find both bulk-in and bulk-out endpoints");
        goto error;
    }

    // 将设备指针保存到interface中
    usb_set_intfdata(interface, dev);

    // 注册USB设备
    retval = usb_register_dev(interface, &skel_class);
    if (retval) {
        err("Not able to get a minor for this device");
        usb_set_intfdata(interface, NULL);
        goto error;
    }

    // 通知用户此设备对应的结点
    info("USB Skeleton device not attached to USBSkel-%d", interface->minor);
    return 0;

error:
    if (dev)
        kref_put(&dev->kref, skel_delete);
    return retval;
}

// 断开函数
static void skel_disconnect(struct usb_interface *interface)
{
    struct usb_skel *dev;
    int minor = interface->minor;

    // 防止skel_open 与 skel_disconnect 竞争
    lock_kernel();

    dev = usb_get_intfdata(interface);
    usb_set_intfdata(interface, NULL);

    // 注销
    usb_deregister_dev(interface, &skel_class);
    unlock_kernel();

    // 减小引用计数
    kref_put(&dev->kref, skel_delete);

    info("USB Skeleton #%d now disconnected", minor);
}

static struct usb_driver skel_driver = {
    .owner = THIS_MODULE,
    .name = "skeleton",
    .id_table = skel_table,
    .probe = skel_probe,
    .disconnect = skel_disconnect,
};

static int __init usb_skel_init(void)
{
    int result;
    // 注册设备驱动
    result = usb_register(&skel_driver);
    if (result)
        err("usb_register failed. Error number %d", result);
    
    return result;
}

static void __exit usb_skel_exit(void)
{
    // 注销usb驱动
    usb_deregister(&skel_driver);
}

module_init(usb_skel_init);
module_exit(usb_skel_exit);

MODULE_LICENSE("GPL");
