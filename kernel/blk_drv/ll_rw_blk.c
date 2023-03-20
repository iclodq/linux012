/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
 // 请求队列  32
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
 // 块设备列表
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

/*
 * blk_size contains the size of all block-devices:
 *
 * blk_size[MAJOR][MINOR]
 *
 * if (!blk_size[MAJOR]) then no minor size checking is done.
 */
 // 每个指定设备的块数量
int * blk_size[NR_BLK_DEV] = { NULL, NULL, };

///
/// 锁缓冲区
//	如果缓冲区已被所住，则休眠
static inline void lock_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	bh->b_lock=1;
	sti();
}

///	解锁缓冲区
//	唤醒等待区任务
static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 *
 * Note that swapping requests always go before other requests,
 * and are done in the order they appear.
 */

 /// 向链表中加入一项请求  会关闭中断
 //	如果设备dev中请求项列表为空，则直接执行req请求
 //	否则，把req请求插入到dev的请求列表中
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli();
	if (req->bh)
		req->bh->b_dirt = 0;	// 缓冲区脏标记 清理
	/// 设备请求链表为空，直接执行请求
	if (!(tmp = dev->current_request)) {
		dev->current_request = req;	// 赋值到请求列表中
		sti();
		(dev->request_fn)();
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next) {
		if (!req->bh)  // 请求没有设置缓冲区
			// 后续请求节点有缓冲区，退出循环
			if (tmp->next->bh)
				break;
			else
				continue;

		// 找到插入请求的位置
		/*
			判断式等价于
			(tmp < req || tmp >= tmp->next) && (req < tmp->next)
			又等价于
			(tmp < req && req < tmp->next)  ||   // 表示当前的请求顺序是 从小到大 req 刚好可以插入到中间
			(tmp >= tmp->next && req < tmp->next)  // 表示当前的请求顺序是 从大到小，
			                                         实际应该插入到tmp->next 但还是插入到tmp后，表示让req当转向节点
		*/
		if ((IN_ORDER(tmp,req) || !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	}
	// 插入到temp后面
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

// 创建请求
// 参数：major 主设备号  ; rw 读写命令  ;  bh 缓冲区
static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	// 预读/预写
	if (rw_ahead = (rw == READA || rw == WRITEA)) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	// 不是读写指令，直接停机
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	// 锁缓冲区
	lock_buffer(bh);
	// 写指令，缓冲区没有脏标记  直接解锁
	// 读指令，缓冲区已经更到最新，直接解锁
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	if (rw == READ)
		req = request+NR_REQUEST;	// 队尾
	else
		req = request+((NR_REQUEST*2)/3);	// 后三分一是读请求
/* find an empty request */
	// 往前找到一个空闲的请求项
	while (--req >= request)
		if (req->dev<0)
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	/// 如果没找到，先判断预读 有预读就解锁缓冲区返回
	//	否则，就睡眠等待请求结构的释放
	if (req < request) {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	// 填充请求结构
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;		// 起始扇区位置
	req->nr_sectors = 2;				// 第一个块，2个扇区
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	add_request(major+blk_dev,req);
}

/// low level read-write page 低级页面读写
//	一次性读写一个页面(4KB=4个块，8个扇区)
//	设置当前任务为不可中断睡眠，并调度
void ll_rw_page(int rw, 		// 读写命令
				int dev, 		// 设备号全称 包含主子
				int page, 		// 页面号（把扇区换成页面号）
				char * buffer)	// buff
{
	struct request * req;
	unsigned int major = MAJOR(dev);

	// 设备号不对 相关检测
	if (major >= NR_BLK_DEV || !(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	// 读写指令检测
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W");
repeat:
	req = request+NR_REQUEST;
	while (--req >= request)
		if (req->dev<0)
			break;
	if (req < request) {
		sleep_on(&wait_for_request);
		goto repeat;
	}
/* fill up the request-info, and add it to the queue */
	req->dev = dev;
	req->cmd = rw;
	req->errors = 0;
	req->sector = page<<3;
	req->nr_sectors = 8;
	req->buffer = buffer;
	req->waiting = current;
	req->bh = NULL;
	req->next = NULL;
	// 因为要读8个扇区，花费时间长，睡眠当前进程
	current->state = TASK_UNINTERRUPTIBLE;
	add_request(major+blk_dev,req);
	schedule();
}	

/// 低级读写块
void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	make_request(major,rw,bh);
}

/// 块设备初始化
void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
