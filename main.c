#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void startothers(void);
static void mpmain(void)  __attribute__((noreturn));
extern pde_t *kpgdir;
extern char end[]; // 内核ELF文件加载完成后，第一个空闲的物理地址

// 引导处理器（BSP）从这里开始执行C语言代码
// 先分配真正的内核栈并切换到该栈，同时完成内存分配器所需的初始化工作
int
main(void)
{
  // ========== 启动可视化日志：进入内核main函数 ==========
  cprintf("[KERNEL] Enter kernel main function, start initializing core system components\n  ");

  // 初始化物理内存分配器（第一阶段）
  // 仅分配[end, 4MB)范围内的物理页框，用于早期内核初始化
  // end：内核镜像加载后的第一个空闲地址；P2V(4*1024*1024)：4MB物理地址转虚拟地址
  kinit1(end, P2V(4*1024*1024)); 
  kvmalloc();      // 初始化内核页表，建立虚拟地址到物理地址的映射
  mpinit();        // 检测系统中的其他处理器（AP），初始化多处理器环境
  lapicinit();     // 初始化本地高级可编程中断控制器（LAPIC），管理CPU中断
  seginit();       // 初始化段描述符表（GDT），xv6中分段仅为兼容x86架构，实际用分页
  picinit();       // 禁用传统8259 PIC中断控制器，避免与LAPIC/IOAPIC冲突
  ioapicinit();    // 初始化IO高级可编程中断控制器（IOAPIC），管理外设中断
  consoleinit();   // 初始化控制台硬件（串口+键盘），用于输出日志和交互
  uartinit();      // 初始化串口（UART），提供字符输出/输入能力
  pinit();         // 初始化进程表，管理系统中所有进程的状态和资源
  tvinit();        // 初始化陷阱向量表（IDT），设置中断/异常的处理入口
  binit();         // 初始化缓冲区缓存（buffer cache），减少磁盘IO次数
  fileinit();      // 初始化文件表，管理系统中打开的文件描述符
  ideinit();       // 初始化IDE磁盘驱动，提供磁盘读写能力
  startothers();   // 启动其他处理器（AP），完成多核初始化
  // 初始化物理内存分配器（第二阶段）
  // 分配[4MB, PHYSTOP)范围内的所有物理页框，必须在startothers后执行（避免AP内存冲突）
  kinit2(P2V(4*1024*1024), P2V(PHYSTOP)); 
  userinit();      // 创建第一个用户进程（init进程），是所有用户进程的祖先
  mpmain();        // 完成当前处理器（BSP）的最终初始化，进入进程调度器
}

// 其他处理器（AP）从entryother.S跳转到这里执行
static void
mpenter(void)
{
  switchkvm();     // 切换到内核页表（kpgdir），统一地址空间
  seginit();       // 初始化AP的段描述符表，与BSP保持一致
  lapicinit();     // 初始化AP的本地LAPIC
  mpmain();        // 执行处理器通用初始化逻辑
}

// 所有CPU（BSP/AP）的通用初始化代码
static void
mpmain(void)
{
  // 打印CPU启动日志，cpuid()获取当前CPU的编号，用于多核调试
  cprintf("cpu:%d:Initialization completed, start running\n", cpuid());
  idtinit();       // 加载中断描述符表（IDT）寄存器，启用中断处理
  // 原子操作：标记当前CPU已启动，通知startothers()无需等待
  xchg(&(mycpu()->started), 1); 
  scheduler();     // 启动进程调度器，循环选择就绪进程执行（永不返回）
}

pde_t entrypgdir[];  // 引导页表，供entry.S和entryother.S使用

// 启动非引导处理器（AP），实现多核CPU启动
static void
startothers(void)
{
  // _binary_entryother_start/end：entryother.S编译后的二进制镜像地址（链接器生成）
  extern uchar _binary_entryother_start[], _binary_entryother_size[];
  uchar *code;     // AP启动代码的加载地址
  struct cpu *c;   // 遍历CPU列表的指针
  char *stack;     // 为AP分配的内核栈

  // 将entryother.S的代码写入0x7000物理地址（AP的初始执行地址）
  // entryother.S是AP的启动汇编代码，负责从实模式切换到保护模式
  code = P2V(0x7000);
  memmove(code, _binary_entryother_start, (uint)_binary_entryother_size);

  // 遍历所有检测到的CPU，启动未初始化的AP
  for(c = cpus; c < cpus+ncpu; c++){
    if(c == mycpu())  // 跳过已启动的引导处理器（BSP）
      continue;

    // 为AP设置启动参数（栈、入口函数、页表）
    // AP初始运行在低内存，暂用entrypgdir而非kpgdir
    stack = kalloc();                          // 为AP分配内核栈
    *(void**)(code-4) = stack + KSTACKSIZE;    // 设置AP的栈顶地址
    *(void(**)(void))(code-8) = mpenter;       // 设置AP的C语言入口函数
    *(int**)(code-12) = (void *) V2P(entrypgdir);  // 设置AP的页表物理地址

    lapicstartap(c->apicid, V2P(code));        // 发送启动信号给AP的LAPIC

    // 等待AP完成mpmain()初始化（轮询started标志）
    while(c->started == 0)
      ;
  }
}

// 引导页表：供entry.S（BSP）和entryother.S（AP）使用的初始页表
// 页目录（及页表）必须按页（4KB）对齐，因此添加__aligned__(PGSIZE)属性
// PTE_PS标志启用4MB大页模式，减少页表层级，简化早期地址映射
__attribute__((__aligned__(PGSIZE)))
pde_t entrypgdir[NPDENTRIES] = {
  // 映射虚拟地址[0, 4MB)到物理地址[0, 4MB)：实模式到保护模式过渡用
  [0] = (0) | PTE_P | PTE_W | PTE_PS,
  // 映射虚拟地址[KERNBASE, KERNBASE+4MB)到物理地址[0, 4MB)：内核地址空间初始化
  [KERNBASE>>PDXSHIFT] = (0) | PTE_P | PTE_W | PTE_PS,
};

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.