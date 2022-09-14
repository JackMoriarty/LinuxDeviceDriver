// #include <linux/config.h>
// #include <linux/module.h>

// #include <linux/mm.h>
// #include <linux/errno.h>
// #include <asm/pgtable.h>

// #include "sculld.h"

// // 打开/关闭函数: 仅跟踪设备被重新映射的次数,防止被释放
// void sculld_vma_open(struct vm_area_struct *vma)
// {
//     struct sculld_dev *dev = vma->private_data;
//     dev->vmas++;
// }

// void sculld_vma_close(struct vm_area_struct *vma)
// {
// 	struct sculld_dev *dev = vma->vm_private_data;

// 	dev->vmas--;
// }

// struct page *sculld_vma_nopage(struct vm_area_struct *vma,
//                                 unsigned long address, int *type)
// {
//     unsigned long offset;
//     struct sculld_dev *ptr, *dev = vma->vm_private_data;
//     struct page *page = NOPAGE_SIGBUS;
//     void *pageptr = NULL;   // 默认为missing

//     down(&dev->sem);
//     offset = (address - vma->vm_start) + (vma->pgoff << PAGE_SHIFT);
// }
