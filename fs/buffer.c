/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end;	// 由链接器生成，指明内核执行模块的末端位置
struct buffer_head * start_buffer = (struct buffer_head *) &end;		// 缓冲区开始地址
struct buffer_head * hash_table[NR_HASH];								// 缓冲区Hash表
static struct buffer_head * free_list;									// 空闲列表
static struct task_struct * buffer_wait = NULL;							// 等待空闲缓冲区的任务队列
int NR_BUFFERS = 0;														// 系统含有缓冲块个数

/// 等待指定缓冲区解锁 如果被锁住，就睡眠
//	关中断只会影响调用进程，其他进程不影响。是通过TSS.flags保存每个任务的这些标记
static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli();
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
	sti();
}

/// 设备数据同步
//	同步设备和内存高速缓冲区中数据
//	同步会把所有修改过的i节点写入高速缓冲
//	然后把高速缓冲区写入设备中，这里是产生设备块写请求
int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	/// 便利缓冲块，有脏标记就写入设备中
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);		// 等待缓冲区可用
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

///	对指定设备进行高速缓冲区的同步操作 
//	首先把缓冲区中dev的块写盘
//	然后同步inode，在把设定设备的缓冲区写盘
int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		// 下面需要再判断一次设备，可能是因为再等待后，该缓冲块被其他进程使用
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	/// 再次同步
	sync_inodes();
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

/// 使指定设备的高速缓冲区数据无效
//	扫描所有缓冲区，是属于设备的缓冲区的更新标记和脏标记置0
//	这个是什么时候用呢？卸载设备的时候？
void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
 /// 检查设备是否更换，如果更换则清理对应的缓冲区
void check_disk_change(int dev)
{
	int i;

	if (MAJOR(dev) != 2)	// 检查是不是软盘
		return;
	if (!floppy_change(dev & 0x03))
		return;
	// 释放对应设备的i节点位图 逻辑块位图所占的高速缓冲块
	// 同时使该设备锁占用的i节点和数据块的高速缓冲块无效
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

// hash函数计算
#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH)
#define hash(dev,block) hash_table[_hashfn(dev,block)]

/// 从hash队列和空闲缓冲队列中移走缓冲区
static inline void remove_from_queues(struct buffer_head * bh)
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	// 维护hash头
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next;

/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh)
		free_list = bh->b_next_free;
}

/// 插入到hash链表体系中
static inline void insert_into_queues(struct buffer_head * bh)
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

/// 在hash表中找到指定设备+块号的高速缓冲区
static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block)
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 */
 /// 从hash表中获取指定设备+块号的高速缓冲区
 //	会加上引用计数
 //	找不到返回null
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++;	// 这里为啥要维护引用呢
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
 // 定义搜索时需要的比较值  同时也规定了优先级，dirt要高
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock)


/// 根据设备号和逻辑块号，获取一个缓冲区块，且引用计数会加一.
//  如果hash表中没有指向该key的缓冲区，则找一个空闲的，把key设置成(dev,block)（会先从hash表中移除，再添加）
//	并返回。所以此方法应该不返回空的？？
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;
   
repeat:
	// 先从hash表中获取，可能为空
	if (bh = get_hash_table(dev,block))
		return bh;
	tmp = free_list;
	do {
		if (tmp->b_count)	// 引用计数不等于0，有进程在使用
			continue;
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp;
			if (!BADNESS(tmp))
				// 从到这里表示 tmp既没有被锁住，也不是脏的，更没有被其他进程所拿住
				// 就可以跳出返回
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list);
	if (!bh) {
		// 没找到，等待
		sleep_on(&buffer_wait);
		goto repeat;
	}
	wait_on_buffer(bh); // 保证没被锁住
	if (bh->b_count)	// 再次保证没有引用
		goto repeat;
	// 又有脏数据
	while (bh->b_dirt) {
		sync_dev(bh->b_dev); // 同步到设备中
		wait_on_buffer(bh);	// 保证没锁 没引用
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	// 可能被其他进程添加到hash表中
	if (find_buffer(dev,block))
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	// 终于找到一个干净的缓冲区，没有被使用，没有被锁住，没有脏数据
	bh->b_count=1;					// 添加引用计数
	bh->b_dirt=0;					// 清零脏标记
	bh->b_uptodate=0;				// 清理更新标记
	remove_from_queues(bh);			// 从hash表中移除
	bh->b_dev=dev;					// 设置设备
	bh->b_blocknr=block;			// 设置逻辑块好
	insert_into_queues(bh);			// 再使用插入到hash表中
	return bh;
}

/// 释放高速缓冲区（引用计数减一），并唤醒等待空闲缓冲区的进程
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))	// 引用减一，如果引用为0 就说明维护出错了
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);	// 唤醒等待高速缓冲区的进程
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 */

/// 读取指定设备+块号的数据块到缓冲区
struct buffer_head * bread(int dev,int block)
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block)))	// 找个空闲的缓冲区块
		panic("bread: getblk returned NULL\n");
	if (bh->b_uptodate)	// 如果已经读取了数据，则世界返回
		return bh;
	ll_rw_block(READ,bh);	// 从设备中读取数据到缓冲区块中
	wait_on_buffer(bh);	// 等待从设备中读取数据到缓冲区，依据b_lockk标记
	if (bh->b_uptodate)	// 如果更到最新，最返回，没有则释放
		return bh;
	// 设备读失败，释放缓冲区块 返null
	brelse(bh);
	return NULL;
}

// 复制内存块从from到to
#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
 /// 一次性读取四个块的数据到指定地址
 //	b[4]四个指定块号
 //	在其中会使用高速区块，使用完马上释放
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			if (bh[i] = getblk(dev,b[i]))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
 /// heread一样读取块，但同时支持多个块，以负数结尾
 //	返回第一个块号的缓冲区，如果最后还有
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh);
			tmp->b_count--;
		}
	}
	va_end(args);
	wait_on_buffer(bh);		// 等解锁
	if (bh->b_uptodate)		// 如果读出的数据还有效则返回，否则释放缓冲块，返回nul
		return bh;
	brelse(bh);
	return (NULL);
}

/// 缓冲区块头初始化
//	buffer_end是缓冲区末尾地址
//	
void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer;
	void * b;
	int i;

	if (buffer_end == 1<<20) 		// 地址是1MB，则改成640KB，因为640KB-1MB是 BIOS和显示内存使用
		b = (void *) (640*1024);
	else
		b = (void *) buffer_end;
	// 初始化每个缓冲块头
	while ( (b -= BLOCK_SIZE) >= ((void *) (h+1)) ) {
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL;
		h->b_prev = NULL;
		h->b_data = (char *) b;	// 缓冲块头指向的实际数据地址
		h->b_prev_free = h-1;
		h->b_next_free = h+1;
		h++;
		NR_BUFFERS++;
		if (b == (void *) 0x100000)		// 当指向1MB时，跳过394KB，指向640KB
			b = (void *) 0xA0000;
	}
	h--;// 回调到最后一个缓冲块头
	free_list = start_buffer;
	free_list->b_prev_free = h;
	h->b_next_free = free_list;		// 完成双向链表闭环
	// 整理hash表
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL;
}	
