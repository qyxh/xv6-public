// Boot loader.
//
// Part of the boot block, along with bootasm.S, which calls bootmain().
// bootasm.S has put the processor into protected 32-bit mode.
// bootmain() loads an ELF kernel image from the disk starting at
// sector 1 and then jumps to the kernel entry routine.

#include "types.h"
#include "elf.h"
#include "x86.h"
#include "memlayout.h"

#define SECTSIZE  512


/* 
 * 函数功能：从磁盘读取指定长度的内容到物理内存
 * pa：目标物理内存地址
 * count：要读取的字节数
 * offset：磁盘中的偏移量（从该位置开始读）
 */
void readseg(uchar*, uint, uint);


/* 
 * bootmain：bootloader的核心函数（C语言入口）
 * 功能：加载ELF格式的xv6内核镜像到物理内存，验证合法性后跳转到内核入口
 */
void
bootmain(void)
{
  struct elfhdr *elf;  // ELF文件头结构体指针
  struct proghdr *ph, *eph; // ELF程序头表指针（eph是程序头表末尾）
  void (*entry)(void); // 内核入口函数指针
  uchar* pa;  // 物理内存地址临时变量

  elf = (struct elfhdr*)0x10000;    // 将ELF内核的头部加载到物理内存地址0x10000（xv6约定的位置）

  readseg((uchar*)elf, 4096, 0); // 从磁盘偏移量0的位置，读取ELF头的大小到内存0x10000

  // Is this an ELF executable?
  if(elf->magic != ELF_MAGIC)
    return;  // let bootasm.S handle error

  // Load each program segment (ignores ph flags).
  ph = (struct proghdr*)((uchar*)elf + elf->phoff);
  eph = ph + elf->phnum;
  for(; ph < eph; ph++){
    pa = (uchar*)ph->paddr;
    readseg(pa, ph->filesz, ph->off);
    if(ph->memsz > ph->filesz)
      stosb(pa + ph->filesz, 0, ph->memsz - ph->filesz);
  }

  // Call the entry point from the ELF header.
  // Does not return!
  entry = (void(*)(void))(elf->entry);
  entry();
}

void
waitdisk(void)
{
  // Wait for disk ready.
  while((inb(0x1F7) & 0xC0) != 0x40)
    ;
}

// Read a single sector at offset into dst.
void
readsect(void *dst, uint offset)
{
  waitdisk();  // 设置要读取的扇区数（1个扇区）
  outb(0x1F2, 1);   // 扇区数 = 1
  outb(0x1F3, offset); // 扇区偏移量低8位
  outb(0x1F4, offset >> 8);  // 扇区偏移量中8位
  outb(0x1F5, offset >> 16);  // 扇区偏移量高8位
  outb(0x1F6, (offset >> 24) | 0xE0);  // 磁盘号+偏移量最高位
  outb(0x1F7, 0x20);  // 发送读磁盘命令

  waitdisk(); // 读取数据到目标内存（512字节/扇区）
  insl(0x1F0, dst, SECTSIZE/4);
}

/* 
 * 函数功能：读取多个扇区到物理内存（封装readsect，处理跨扇区情况）
 * pa：目标物理地址
 * count：要读取的总字节数
 * offset：磁盘偏移量（字节）
 */
void
readseg(uchar* pa, uint count, uint offset)
{
  uchar* epa;  // 读取结束的物理地址

  epa = pa + count;

  pa -= offset % SECTSIZE; // 对齐到扇区边界（磁盘按扇区读写，512字节/扇区）

  offset = (offset / SECTSIZE) + 1; // 跳过MBR（主引导记录）所在的第一个扇区

  // If this is too slow, we could read lots of sectors at a time.
  // We'd write more to memory than asked, but it doesn't matter --
  // we load in increasing order.
  for(; pa < epa; pa += SECTSIZE, offset++)   // 逐扇区读取，直到读完所有字节
    readsect(pa, offset);
}
