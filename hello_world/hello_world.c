/**
 * @Author: Bangduo Chen <chenbangduo>
 * @Date:   2018-02-23T17:04:40+08:00
 * @Email:  1413563527@qq.com
 * @Filename: hello_world.c
 * @Last modified by:   chenbangduo
 * @Last modified time: 2018-02-24T08:59:26+08:00
 */


#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("Dual BSD/GPL");

/**
 * 初始化函数
 * @return  状态值，此处无明确意义，返回0即可
 */
static int hello_init(void)
{
    printk(KERN_ALERT "Hello, world\n");
    return 0;
}

/**
 * 清理函数，当卸载驱动时将调用
 */
static void hello_exit(void)
{
    printk(KERN_ALERT "Goodbye, cruel world\n");
}

module_init(hello_init);
module_exit(hello_exit);
