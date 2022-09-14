#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h>   // printk
#include <linux/slab.h>     // kmalloc()
#include <linux/errno.h>    // error codes
#include <linux/types.h>    // size_t
#include <linux/interrupt.h>// mark_bh

#include <linux/in.h>
#include <linux/netdevice.h>// 设备结构, 和其他头部
#include <linux/etherdevice.h>// eth_type_trans ,确定包协议的ID
#include <linux/ip.h>   // struct iphdr
#include <linux/tcp.h>  // struct tcphdr

#include <linux/skbuff.h>
#include <linux/jiffies.h>

#include "snull.h"

#include <linux/in6.h>
#include <asm/checksum.h>

MODULE_AUTHOR("Bangduo Chen");
MODULE_LICENSE("GPL");

// 模拟发送数据包发生超时
static int lockup = 0;
module_param(lockup, int, 0);

// 设置超时时间
static int timeout = SNULL_TIMEOUT;

// 设置不使用中断, 采用轮询的方式
static int use_napi = 0;
module_param(use_napi, int, 0);

// 描述正在传输数据包的结构体
struct snull_packet {
    struct snull_packet *next;
    struct net_device *dev;
    int datalen;
    u8 data[ETH_DATA_LEN];
};

// 缓冲池大小
int pool_size = 8;
module_param(pool_size, int, 0);
// 设备结构体
struct net_device *snull_devs[2];

// 每个设备私有结构体, 用来传递数据包(发出和接收), 此处包含了数据包的空间
struct snull_priv {
    struct net_device_stats stats;
    int status;
    struct snull_packet *ppool;
    struct snull_packet *rx_queue;  // 传入数据包列表
    int rx_int_enabled;
    int tx_packetlen;
    u8 *tx_packetdata;
    struct sk_buff *skb;
    spinlock_t lock;
};

static void snull_tx_timeout(struct net_device *dev);
static void (*snull_interrupt)(int, void *, struct pt_regs *);

// 设置设备的数据包缓冲池
void snull_setup_pool(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    int i;
    struct snull_packet *pkt;

    priv->ppool = NULL;
    for (i = 0; i < pool_size; i++) {
        pkt = kmalloc(sizeof(struct snull_packet), GFP_KERNEL);
        if (pkt == NULL) {
            printk(KERN_NOTICE "Ran out of memory allocating packet pool\n");
            return ;
        }
        pkt->dev = dev;
        pkt->next = priv->ppool;
        priv->ppool = pkt;
    }
}

// 清除设备的数据包缓冲区
void snull_teardown_pool(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet *pkt;

    while ((pkt = priv->ppool)) {
        priv->ppool = pkt->next;
        kfree (pkt);
    }
}

// 缓冲区管理
struct snull_packet *snull_get_tx_buffer(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    unsigned long flags;
    struct snull_packet *pkt;

    PDEBUG("snull_get_tx_buffer: out of spin");

    spin_lock_irqsave(&priv->lock, flags);
    
    PDEBUG("snull_get_tx_buffer: in of spin");

    pkt = priv->ppool;
    priv->ppool = pkt->next;
    if (priv->ppool == NULL) {
        printk(KERN_INFO "Pool empty\n");
        netif_stop_queue(dev);
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    
    PDEBUG("snull_get_tx_buffer: out of spin");
    
    return pkt;
}

// 释放缓冲区

void snull_release_buffer(struct snull_packet * pkt)
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(pkt->dev);

    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->ppool;
    priv->ppool = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
    if (netif_queue_stopped(pkt->dev) && pkt->next == NULL)
        netif_wake_queue(pkt->dev);
}

void snull_enqueue_buf(struct net_device *dev, struct snull_packet *pkt)
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(dev);

    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->rx_queue;
    priv->rx_queue = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
}

struct snull_packet *snull_dequeue_buf(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet *pkt;
    unsigned long flags;

    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->rx_queue;
    if (pkt != NULL)
        priv->rx_queue = pkt->next;
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}

// 启用或者禁止中断
static void snull_rx_ints(struct net_device *dev, int enable)
{
    struct snull_priv *priv = netdev_priv(dev);
    priv->rx_int_enabled = enable;
}

// 打开和关闭
int snull_open(struct net_device *dev)
{
    // 对主板的硬件地址赋值, 使用"\0\0SNULx",
    // 其中x是0或1, 第一个字节是"\0"是为了避免成为
    // 组播地址(组播地址第一个字节是奇数)
    memcpy(dev->dev_addr, "\0SNUL0", ETH_ALEN);
    if (dev == snull_devs[1])
        dev->dev_addr[ETH_ALEN - 1]++; //"\0SNUL1"
    netif_start_queue(dev);
    return 0;
}

int snull_release(struct net_device *dev)
{
    netif_stop_queue(dev);  // 停止传输
    return 0;
}

// 配置改变(ifconfig 命令传递)
int snull_config(struct net_device *dev, struct ifmap *map)
{
    // 不能在接口还在使用时进行配置
    if (dev->flags & IFF_UP)
        return -EBUSY;
    
    // 禁止改变网络接口I/O基地址
    if (map->base_addr != dev->base_addr) {
        printk(KERN_WARNING "snull: Can't change I/O address\n");
        return -EOPNOTSUPP;
    }

    // 允许修改中断号
    if (map->irq != dev->irq) {
        dev->irq = map->irq;
        // request_irq() 被推迟到打开时
    }

    // 忽略其他细节
    return 0;
}

// 接收数据包
void snull_rx(struct net_device *dev, struct snull_packet *pkt)
{
    struct sk_buff *skb;
    struct snull_priv *priv = netdev_priv(dev);

    // 从传输介质中获取了数据包, 建立封装它的skb, 使得上层可以处理它
    skb = dev_alloc_skb(pkt->datalen + 2);
    if (!skb) {
        if(printk_ratelimit())
            printk(KERN_NOTICE "snull rx: low on mem - packet dropped\n");
        priv->stats.rx_dropped++;
        goto out;
    }

    // 保留报文头空间
    skb_reserve(skb, 2);
    // 向套接字缓冲区写入数据包
    memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);

    // 写入其他数据, 然后传递给接收层
    skb->dev = dev;
    skb->protocol = eth_type_trans(skb, dev);
    skb->ip_summed = CHECKSUM_UNNECESSARY; // 不必检查它
    priv->stats.rx_packets++;
    priv->stats.rx_bytes += pkt->datalen;
    netif_rx(skb);
out:
    return;
}

// poll 方法实现(不使用中断)
static int snull_poll(struct net_device *dev, int *budget)
{
    int npackets = 0, quota = min(dev->quota, *budget);
    struct sk_buff *skb;
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet * pkt;

    while(npackets < quota && priv->rx_queue) {
        pkt = snull_dequeue_buf(dev);
        skb = dev_alloc_skb(pkt->datalen + 2);
        if (!skb) {
            if(printk_ratelimit())
                printk(KERN_NOTICE "snull: packet dropped\n");
            priv->stats.rx_dropped++;
            snull_release_buffer(pkt);
            continue;
        }
        // 保留报文头部空间
        skb_reserve(skb, 2);
        memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);
        skb->dev = dev;
        skb->protocol = eth_type_trans(skb, dev);
        skb->ip_summed = CHECKSUM_UNNECESSARY;
        netif_receive_skb(skb);

        // 更新状态
        npackets++;
        priv->stats.rx_packets++;
        priv->stats.rx_bytes += pkt->datalen;
        snull_release_buffer(pkt);
    }
    // 此时, 如果已经处理完所有数据包, 向内核报告并重新启用中断
    *budget -= npackets;
    dev->quota -= npackets;
    if (!priv->rx_queue) {
        netif_rx_complete(dev);
        snull_rx_ints(dev, 1);
        return 0;
    }
    // 没能处理完
    return 1;
}

// 典型中断处理函数
static void snull_regular_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    int statusword;
    struct snull_priv *priv;
    struct snull_packet *pkt = NULL;

    // 同往常一样, 检查device指针以确保他指向了中断, 然后为struct device *dev 赋值
    // 超出正常范围
    struct net_device *dev = (struct net_device *) dev_id;
    if (!dev)
        return ;
    
    // 锁定设备
    priv = netdev_priv(dev);
    spin_lock(&priv->lock);

    // 获取状态字, 真实的网络设备使用I/O指令
    statusword = priv->status;
    priv->status = 0;
    if (statusword & SNULL_RX_INTR) {
        // 将其发送给snull_rx 处理
        pkt = priv->rx_queue;
        if (pkt) {
            priv->rx_queue = pkt->next;
            snull_rx(dev, pkt);
        }
    }
    if (statusword & SNULL_TX_INTR) {
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
        dev_kfree_skb(priv->skb);
    }
    // 解锁,结束处理
    spin_unlock(&priv->lock);
    if (pkt) snull_release_buffer(pkt);
    return;
}

// 使用轮询方式的相关中断处理函数
static void snull_napi_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    int statusword;
    struct snull_priv *priv;
    // 检查设备指针, 然后为其赋值
    struct net_device *dev = (struct net_device *)dev_id;

    // 超出正常范围
    if (!dev)
        return;

    // 锁住设备
    priv = netdev_priv(dev);
    spin_lock(&priv->lock);

    statusword = priv->status;
    priv->status = 0;
    if (statusword & SNULL_RX_INTR) {
        snull_rx_ints(dev, 0); // 禁用中断
        netif_rx_schedule(dev);
    }
    if (statusword & SNULL_TX_INTR) {
        priv->stats.tx_packets++;
        priv->stats.tx_bytes += priv->tx_packetlen;
        dev_kfree_skb(priv->skb);
    }

    // 解锁
    spin_unlock(&priv->lock);
    return;
}

// 发送数据包(底层接口)
static void snull_hw_tx(char *buf, int len, struct net_device *dev)
{
    // 此函数处理硬件细节.此接口将数据包回环到另一个snull接口.
    // 换言之, 此函数实现了snull的行为, 而其他程序是独立于设备的
    struct iphdr *ih;
    struct net_device *dest;
    struct snull_priv *priv;
    u32 *saddr, *daddr;
    struct snull_packet *tx_buffer;

    if (len < sizeof(struct ethhdr) + sizeof(struct iphdr)) {
        printk("snull: Hmm... packet too short (%i octets)\n", len);
        return;
    }

    if (0) { // 打开以查看数据
        int i;
        PDEBUG("len is %i\n" KERN_DEBUG "data:", len);
        for ( i = 14; i < len; i++)
            printk(" %02X", buf[i] & 0xff);
        printk("\n");
    }

    // 以太网头14个字节, 但是内核把他设置为对齐的(以太网不是对齐的)
    ih = (struct iphdr *)(buf + sizeof(struct ethhdr));
    saddr = &ih->saddr;
    daddr = &ih->daddr;

    ((u8 *)saddr)[2] ^= 1; // 改变第三个字节
    ((u8 *)daddr)[2] ^= 1;

    // 重新计算校验和
    ih->check = 0;
    ih->check = ip_fast_csum((unsigned char *)ih, ih->ihl);

    	if (dev == snull_devs[0])
		PDEBUG("%08x:%05i --> %08x:%05i\n",
				ntohl(ih->saddr),ntohs(((struct tcphdr *)(ih+1))->source),
				ntohl(ih->daddr),ntohs(((struct tcphdr *)(ih+1))->dest));
	else
		PDEBUG("%08x:%05i <-- %08x:%05i\n",
				ntohl(ih->daddr),ntohs(((struct tcphdr *)(ih+1))->dest),
				ntohl(ih->saddr),ntohs(((struct tcphdr *)(ih+1))->source));
    
    // 好的, 现在包已经准备好, 等待发送: 首先在接收方模拟一个接收中断, 然后在发送方模拟一个
    // 发送完毕中断
    dest = snull_devs[dev == snull_devs[0] ? 1 : 0];
    priv = netdev_priv(dest);
    tx_buffer = snull_get_tx_buffer(dev);
    tx_buffer->datalen = len;
    memcpy(tx_buffer->data, buf, len);
    snull_enqueue_buf(dest, tx_buffer);
    if (priv->rx_int_enabled) {
        priv->status |= SNULL_RX_INTR;
        snull_interrupt(0, dest, NULL);
    }
    
    priv = netdev_priv(dev);
    priv->tx_packetlen = len;
    priv->tx_packetdata = buf;
    priv->status |= SNULL_TX_INTR;
    if (lockup && ((priv->stats.tx_packets + 1) % lockup) == 0) {
        // 模拟取消发送中断
        netif_stop_queue(dev);
        PDEBUG("Simulate lockup at %ld, txp %ld\n", jiffies,
				(unsigned long) priv->stats.tx_packets);
    } else {
        snull_interrupt(0, dev, NULL);
    }
}

// 发送数据包, 有内核调用
int snull_tx(struct sk_buff *skb, struct net_device *dev)
{
    int len;
    char *data, shortpkt[ETH_ZLEN];
    struct snull_priv *priv = netdev_priv(dev);

    data = skb->data;
    len = skb->len;
    if (len <ETH_ZLEN) {
        memset(shortpkt, 0, ETH_ZLEN);
        memcpy(shortpkt, skb->data, skb->len);
        len = ETH_ZLEN;
        data = shortpkt;
    }
    dev->trans_start = jiffies;    // 保存时间戳
    // 记住skb, 这样在发送中断处理函数中可以对齐进行释放
    priv->skb = skb;
    // 实际发送数据
    snull_hw_tx(data, len, dev);

    // 永不失败
    return 0;
}

// 处理超时
void snull_tx_timeout(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);

    PDEBUG("Transmit timout at %ld, latency %ld\n", jiffies, jiffies - dev->trans_start);

    // 模拟一个传输中断
    priv->status = SNULL_TX_INTR;
    snull_interrupt(0, dev, NULL);
    priv->stats.tx_errors++;
    netif_wake_queue(dev);
    return;
}

// ioctl 函数
int snull_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    PDEBUG("ioctl\n");
    return 0;
}

// 返回统计结果给调用者
struct net_device_stats *snull_stats(struct net_device *dev)
{
    struct snull_priv *priv = netdev_priv(dev);
    return &priv->stats;
}

// 此函数用来填充一个以太网头, 因为在此接口上arp不可用
int snull_rebuild_header(struct sk_buff *skb)
{
    struct ethhdr *eth = (struct ethhdr *) skb->data;
    struct net_device *dev = skb->dev;
    memcpy(eth->h_source, dev->dev_addr, dev->addr_len);
    memcpy(eth->h_dest, dev->dev_addr, dev->addr_len);
    eth->h_dest[ETH_ALEN - 1] ^= 0x01;
    return 0;
}

int snull_header(struct sk_buff *skb, struct net_device *dev,
                unsigned short type, void *daddr, void *saddr,
                unsigned int len)
{
    struct ethhdr *eth = (struct ethhdr *)skb_push(skb, ETH_HLEN);

    eth->h_proto = htons(type);
    memcpy(eth->h_source, saddr ? saddr : dev->dev_addr, dev->addr_len);
    memcpy(eth->h_dest, daddr ? daddr : dev->dev_addr, dev->addr_len);
    eth->h_dest[ETH_ALEN - 1] ^= 0x01;
    return (dev->hard_header_len);
}

// 改变mtu通常不被使用
int snull_change_mtu(struct net_device *dev, int new_mtu)
{
    unsigned long flags;
    struct snull_priv *priv = netdev_priv(dev);
    spinlock_t *lock = &priv->lock;

    // 检查新的mtu值
    if ((new_mtu < 68) || (new_mtu > 1500))
        return -EINVAL;
    
    spin_lock_irqsave(lock, flags);
    dev->mtu = new_mtu;
    spin_unlock_irqrestore(lock, flags);
    return 0;
}

// 初始化函数(有时也称探测函数), 会被register_netdev()调用
void snull_init(struct net_device *dev)
{
    struct snull_priv *priv;

    ether_setup(dev);   // 初始化dev中的一些成员
    dev->open            = snull_open;
	dev->stop            = snull_release;
	dev->set_config      = snull_config;
	dev->hard_start_xmit = snull_tx;
	dev->do_ioctl        = snull_ioctl;
	dev->get_stats       = snull_stats;
	dev->change_mtu      = snull_change_mtu;  
	dev->rebuild_header  = snull_rebuild_header;
	dev->hard_header     = snull_header;
	dev->tx_timeout      = snull_tx_timeout;
	dev->watchdog_timeo  = timeout;
    if (use_napi) {
		dev->poll        = snull_poll;
		dev->weight      = 2;
	}

    // 初始化priv域, 此项关系到统计和一些其他私有域
    priv = netdev_priv(dev);
    memset(priv, 0, sizeof(struct snull_priv));
    spin_lock_init(&priv->lock);
    snull_rx_ints(dev, 1);  //允许接收中断
    snull_setup_pool(dev);
}

// 模块卸载函数
void snull_cleanup(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (snull_devs[i]) {
            unregister_netdev(snull_devs[i]);
            snull_teardown_pool(snull_devs[i]);
            free_netdev(snull_devs[i]);
        }
    }
    return ;
}

int snull_init_module(void)
{
    int result, i, ret = -ENOMEM;

    snull_interrupt = use_napi ? snull_napi_interrupt : snull_regular_interrupt;

    snull_devs[0] = alloc_netdev(sizeof(struct snull_priv), "sn%d", snull_init);
    snull_devs[1] = alloc_netdev(sizeof(struct snull_priv), "sn%d", snull_init);

    if (snull_devs[0] == NULL || snull_devs[1] == NULL)
        goto out;
    
    ret = -ENODEV;
    for (i = 0; i < 2; i++)
        if ((result = register_netdev(snull_devs[i])))
            printk("snull: error %i registering device \"%s\"\n",
                    result, snull_devs[i]->name);
        else
            ret = 0;
    out:
        if (ret)
            snull_cleanup();
        return ret;
}

module_init(snull_init_module);
module_exit(snull_cleanup);
