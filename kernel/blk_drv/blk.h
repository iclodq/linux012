#ifndef _BLK_H
#define _BLK_H


// 块设备类型数量
/*
	0	无	无	null
	1	块/字符	ram,内存设备（虚拟盘等）	do_rd_request()
	2	块	fd,软驱	do_rd_request()
	3	块	hd，硬盘	do_hd_request()
	4	字符	ttyx（虚拟/串行终端）	null
	5	字符	tty设备	null
	6	字符	lp打印机	null
*/
#define NR_BLK_DEV	7

/*
 * NR_REQUEST is the number of entries in the request-queue.
 * NOTE that writes may use only the low 2/3 of these: reads
 * take precedence.
 *
 * 32 seems to be a reasonable number: enough to get some benefit
 * from the elevator-mechanism, but not so much as to lock a lot of
 * buffers when they are in the queue. 64 seems to be too many (easily
 * long pauses in reading when heavy writing/syncing is going on)
 */
 // 请求项队列数量
#define NR_REQUEST	32

/*
 * Ok, this is an expanded form so that we can use the same
 * request for paging requests when that is implemented. In
 * paging, 'bh' is NULL, and 'waiting' is used to wait for
 * read/write completion.
 */
struct request {
	int dev;		/* -1 if no request */	// 发请求的设备号
	int cmd;		/* READ or WRITE */		// 读#0|写#1 指令  定义在include/linux/fs.h
	int errors;								// 操作时产生的错误次数
	unsigned long sector;					// 起始扇区  1块=2扇区
	unsigned long nr_sectors;				// 本次请求 读写扇区数
	char * buffer;							// 数据缓冲区
	struct task_struct * waiting;			// 任务等待请求完成操作的地方
	struct buffer_head * bh;				// 缓冲区头指针  定义include/linux/fs.h
	struct request * next;					// 下一个请求
};

/*
 * This is used in the elevator algorithm: Note that
 * reads always go before writes. This is natural: reads
 * are much more time-critical than writes.
 */

// 电梯算法，度操作总是在写操作之前进行

// s1 s2 为(struct request*)
// 根据s1/s2中的信息，判断 s1/s2的先后顺序，该顺序用于块设备的请求执行顺序
// 会在add_request() 被使用 blk_drv/ll_rw_blk.c
// 部分实现了I/O调度功能 还有个请求合并功能

#define IN_ORDER(s1,s2) \
((s1)->cmd<(s2)->cmd || (s1)->cmd==(s2)->cmd && \
((s1)->dev < (s2)->dev || ((s1)->dev == (s2)->dev && \
(s1)->sector < (s2)->sector)))

// 快设备处理结构
struct blk_dev_struct {
	void (*request_fn)(void);				// 请求处理函数指针
	struct request * current_request;		// 请求结构
};

// 块设备表	每种块设备占用一项，索引值为主设备号
extern struct blk_dev_struct blk_dev[NR_BLK_DEV];
// 请求项数组 
extern struct request request[NR_REQUEST];
// 等待空闲请求项的进程队列头指针
extern struct task_struct * wait_for_request;

// 一个块设备上数据块的总数指针数组
// 每个指针指向指定主设备号的总块数数组 hd_sizes[] blk_drv/hd.c
// 		hd_sizes[x] 对应一个子设备上所拥有的数据块总数 1块大小=1KB
extern int * blk_size[NR_BLK_DEV];

// 在包含此头文件的块设备驱动程序（hd.c）中，必须先定义程序使用的主设备号
#ifdef MAJOR_NR

/*
 * Add entries as needed. Currently the only block devices
 * supported are hard-disks and floppies.
 */

#if (MAJOR_NR == 1)
/* ram disk */
// 内存虚拟盘
#define DEVICE_NAME "ramdisk"					// 设备名称（内存虚拟盘）
#define DEVICE_REQUEST do_rd_request			// 设备请求项处理函数
#define DEVICE_NR(device) ((device) & 7)		// 子设备号(0-7)
#define DEVICE_ON(device) 						// 开启设备
#define DEVICE_OFF(device)						// 关闭设备

#elif (MAJOR_NR == 2)
/* floppy */
#define DEVICE_NAME "floppy"
#define DEVICE_INTR do_floppy
#define DEVICE_REQUEST do_fd_request
#define DEVICE_NR(device) ((device) & 3)		// 子设备号（0-3）
#define DEVICE_ON(device) floppy_on(DEVICE_NR(device))
#define DEVICE_OFF(device) floppy_off(DEVICE_NR(device))


#elif (MAJOR_NR == 3)
/* harddisk */
//	硬盘
#define DEVICE_NAME "harddisk"						// 设备名称（硬盘）
#define DEVICE_INTR do_hd							// 设备中断处理函数
#define DEVICE_TIMEOUT hd_timeout					// 设备超时值
#define DEVICE_REQUEST do_hd_request				// 设备请求项处理函数
#define DEVICE_NR(device) (MINOR(device)/5)			// 硬盘设备号(0-1)
#define DEVICE_ON(device)							// 一开机硬盘就是运转着
#define DEVICE_OFF(device)							// 同上

#elif
/* unknown blk device */
#error "unknown blk device"

#endif

// 是指定设备号的当前请求结构的的指针
#define CURRENT (blk_dev[MAJOR_NR].current_request)
// 当前请求项设备的设备号 硬盘为0/1
#define CURRENT_DEV DEVICE_NR(CURRENT->dev)

// 如果定义了DEVICE_INTR ,就把它定义为一个函数指针，并置空
#ifdef DEVICE_INTR
void (*DEVICE_INTR)(void) = NULL;
#endif

// 设备超时
#ifdef DEVICE_TIMEOUT
int DEVICE_TIMEOUT = 0;
// 设置中断函数，并同时设置超时
#define SET_INTR(x) (DEVICE_INTR = (x),DEVICE_TIMEOUT = 200)
#else
#define SET_INTR(x) (DEVICE_INTR = (x))
#endif
// 声明请求函数
static void (DEVICE_REQUEST)(void);

// 解锁缓冲块 并唤醒等待该缓冲区的进程
extern inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk(DEVICE_NAME ": free buffer being unlocked\n");
	bh->b_lock=0;
	wake_up(&bh->b_wait);
}

// 结束请求处理宏
// uptodate # 更新标记，应该时间相关数据 | =0#表示更新失败
extern inline void end_request(int uptodate)
{
	DEVICE_OFF(CURRENT->dev);		// 关闭设备
	if (CURRENT->bh) {
		// 更新缓冲头，并解锁
		CURRENT->bh->b_uptodate = uptodate;
		unlock_buffer(CURRENT->bh);
	}
	// 更新标失败
	if (!uptodate) {
		printk(DEVICE_NAME " I/O error\n\r");
		printk("dev %04x, block %d\n\r",CURRENT->dev,
			CURRENT->bh->b_blocknr);
	}
	wake_up(&CURRENT->waiting);		// 唤醒等待该请求的进程    让那些进程不要等待了
	wake_up(&wait_for_request);		// 唤醒等待空闲请求项的进程
	CURRENT->dev = -1;				// 释放该请求项
	CURRENT = CURRENT->next;		// 指向下一个请求项
}

#ifdef DEVICE_TIMEOUT
// 清理设备超时
#define CLEAR_DEVICE_TIMEOUT DEVICE_TIMEOUT = 0;
#else
#define CLEAR_DEVICE_TIMEOUT
#endif

#ifdef DEVICE_INTR
// 清理中断函数指针
#define CLEAR_DEVICE_INTR DEVICE_INTR = 0;
#else
#define CLEAR_DEVICE_INTR
#endif

// 定义初始化请求项
#define INIT_REQUEST \
repeat: \
	if (!CURRENT) {\
		CLEAR_DEVICE_INTR \
		CLEAR_DEVICE_TIMEOUT \
		return; \
	} \
	if (MAJOR(CURRENT->dev) != MAJOR_NR) \
		panic(DEVICE_NAME ": request list destroyed"); \
	if (CURRENT->bh) { \
		if (!CURRENT->bh->b_lock) \
			panic(DEVICE_NAME ": block not locked"); \
	}

#endif

#endif
