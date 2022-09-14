/*
 * 虚拟 LDD总线
 * @Author: Bangduo Chen 
 * @Date: 2018-10-03 14:45:12 
 * @Last Modified by: Bangduo Chen
 * @Last Modified time: 2018-10-03 15:36:38
 */

// 外部定义的总线类型
extern struct bus_type ldd_bus_type;

// ldd总线驱动
struct ldd_driver {
    char *version;
    struct module *module;
    struct device_driver driver;
    struct driver_attribute version_attr;
};

#define to_ldd_driver(drv) container_of(drv, struct ldd_driver, driver);

// 总线上的设备
struct ldd_device {
    char *name;
    struct ldd_driver *driver;
    struct device dev;
};

#define to_ldd_device(dev) container_of(dev, struct ldd_device, dev);

extern int register_ldd_device(struct ldd_device *);
extern void unregister_ldd_device(struct ldd_device *);
extern int register_ldd_driver(struct ldd_driver *);
extern void unregister_ldd_driver(struct ldd_driver *);
