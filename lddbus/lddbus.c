/*
 * 虚拟总线
 * @Author: Bangduo Chen 
 * @Date: 2018-10-03 15:38:35 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-10-03 15:40:44
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include "lddbus.h"

MODULE_AUTHOR("Bangduo Chen");
MODULE_LICENSE("GPL");
static char *Version = "$Revision: 1.9 $";

// 总线match函数, 用来匹配设备与驱动程序
static int ldd_match(struct device *dev, struct device_driver *driver)
{
    return !strncmp(dev->bus_id, driver->name, strlen(driver->name));
}

// 总线响应热插拔事件, 设置环境变量
static int ldd_hotplug(struct device *dev, char **envp, int num_envp,
            char *buffer, int buffer_size)
{
    envp[0] = buffer;
    if (snprintf(buffer, buffer_size, "LDDBUS_VERSION=%s", Version) >= buffer_size)
        return -ENOMEM;
    envp[1] = NULL;
    return 0;
}

// 总线释放函数
static void ldd_bus_release(struct device *dev)
{
    printk(KERN_DEBUG "lddbus release\n");
}

// 总线设备结构体
struct device ldd_bus = {
    .bus_id = "ldd0",
    .release = ldd_bus_release
};

// 总线类型
struct bus_type ldd_bus_type = {
    .name = "ldd",
    .match = ldd_match,
    .hotplug = ldd_hotplug
};

// 导出一个简单的属性
static ssize_t show_bus_version(struct bus_type *bus, char *buf)
{
    return snprintf(buf, PAGE_SIZE, "%s\n", Version);
}

// 编译时创建bus参数结构体
static BUS_ATTR(version, S_IRUGO, show_bus_version, NULL);

// LDD 设备

// 设备释放函数
static void ldd_dev_release(struct device *dev)
{ }

// 设备注册函数
int register_ldd_device(struct ldd_device *ldddev)
{
    ldddev->dev.bus = &ldd_bus_type;
    ldddev->dev.parent = &ldd_bus;
    ldddev->dev.release = ldd_dev_release;
    strncpy(ldddev->dev.bus_id, ldddev->name, BUS_ID_SIZE);
    return device_register(&ldddev->dev);
}
EXPORT_SYMBOL(register_ldd_device);

// 设备注销函数
void unregister_ldd_device(struct ldd_device *ldddev)
{
    device_unregister(&ldddev->dev);
}
EXPORT_SYMBOL(unregister_ldd_device);

// 驱动程序

static ssize_t show_version(struct device_driver *driver, char *buf)
{
    struct ldd_driver *ldriver = to_ldd_driver(driver);

    sprintf(buf, "%s\n", ldriver->version);
    return strlen(buf);
}

int register_ldd_driver(struct ldd_driver *driver)
{
    int ret;

    driver->driver.bus = &ldd_bus_type;
    ret = driver_register(&driver->driver);
    if(ret)
        return ret;
    driver->version_attr.attr.name = "version";
    driver->version_attr.attr.owner = driver->module;
    driver->version_attr.attr.mode = S_IRUGO;
    driver->version_attr.show = show_version;
    driver->version_attr.store = NULL;
    return driver_create_file(&driver->driver, &driver->version_attr);
}

void unregister_ldd_driver(struct ldd_driver *driver)
{
    driver_unregister(&driver->driver);
}
EXPORT_SYMBOL(register_ldd_driver);
EXPORT_SYMBOL(unregister_ldd_driver);

static int __init ldd_bus_init(void)
{
    int ret;
    ret = bus_register(&ldd_bus_type);
    if (ret)
        return ret;
    if (bus_create_file(&ldd_bus_type, &bus_attr_version))
        printk(KERN_NOTICE "Unable to create version attribute\n");
    ret = device_register(&ldd_bus);
    if (ret)
        return ret;
    return 0;
}

static void ldd_bus_exit(void)
{
    device_unregister(&ldd_bus);
    bus_unregister(&ldd_bus_type);
}

module_init(ldd_bus_init);
module_exit(ldd_bus_exit);
