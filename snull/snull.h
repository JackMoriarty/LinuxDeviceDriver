#undef PDEBUG
#ifdef SNULL_DEBUG
    #ifdef __KERNEL
        #define PDEBUG(fmt, args...) printk(KERNEL_DEBUG "snull:" fmt, ##args)
    #else
        #define PDEUBG(fmt, args...) fprintf(stderr, fmt, ##args)
    #endif
#else
    #define PDEBUG(fmt, args...)
#endif  // SNULL_DEBUG

// 定义一些状态码
#define SNULL_RX_INTR 0x0001
#define SNULL_TX_INTR 0x0002

// 默认超时时间
#define SNULL_TIMEOUT 5

extern struct net_device *snull_devs[];
