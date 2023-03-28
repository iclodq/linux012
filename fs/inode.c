/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <asm/system.h>

/**
 * @brief 
 * i节点号所在的逻辑块号=启动块（引动块）+ 超级块 + i节点位图占用块数量 + 逻辑块位图占用块数量 + （i节点块号-1）/每块含有的i节点数量
 * 虽然i节点块号从0还是编号，但由于0号块不使用，且也不实际占用块，所以存放i节点数据的第一个块所保存的块号是（1-32）
 * 
 */


/// 设备数据块总数指针数组，
//	blk_size[0] = hd_sizes[]    指定主设备号的总块数数组  hd_sizes[0]  就是对应子设备号的总块数组
extern int *blk_size[];

// i节点表
struct m_inode inode_table[NR_INODE]={{0,},};

static void read_inode(struct m_inode * inode);
static void write_inode(struct m_inode * inode);


/**
 * @brief 等待i节点无锁
 * 如果已锁住，则把当前任务设置为不可中断的等待
 * 并把当前任务放到节点的等待队列中？
 *
 * @param inode 
 */
static inline void wait_on_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	sti();
}

/// 对i节点上锁
//	如果已锁住，则把当前任务设置为不可中断的等待
//	并把当前任务放到节点的等待队列中？
//	然后上锁
static inline void lock_inode(struct m_inode * inode)
{
	cli();
	while (inode->i_lock)
		sleep_on(&inode->i_wait);
	inode->i_lock=1;
	sti();
}

/// 对i节点解锁
//	唤醒等待进程的节点
static inline void unlock_inode(struct m_inode * inode)
{
	inode->i_lock=0;
	wake_up(&inode->i_wait);
}

/// 释放设备dev再内存i节点表中所有i节点
void invalidate_inodes(int dev)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		if (inode->i_dev == dev) {
			if (inode->i_count)		// 还有引用  
				printk("inode in use on removed disk\n\r");
			inode->i_dev = inode->i_dirt = 0;	// 释放i节点，对设备号，脏数据值0
		}
	}
}

/// 同步所有i节点数据
//	把i节点表中所有i节点与设备上i节点作同步操作
void sync_inodes(void)
{
	int i;
	struct m_inode * inode;

	inode = 0+inode_table;
	for(i=0 ; i<NR_INODE ; i++,inode++) {
		wait_on_inode(inode);
		// i节点是脏的且不是管道类型
		if (inode->i_dirt && !inode->i_pipe)
			write_inode(inode);		// 写盘，实际写入缓冲区
	}
}



 /**
  * @brief 文件数据块映射到盘块的处理操作 （block位图处理函数，bmap - block map）
  * 
  * @param inode             文件的i节点指针
  * @param block             文件中的数据块号
  * @param create            是否创建块标志 = 0 #不创建 | 1 #创建
  * @return int 创建的块号
  */
static int _bmap(struct m_inode * inode,int block,int create)
{
	struct buffer_head * bh;
	int i;

	if (block<0)
		panic("_bmap: block<0");
	if (block >= 7+512+512*512)
		panic("_bmap: block>big");
	// 直接块的处理
	if (block<7) {
		if (create && !inode->i_zone[block])
			if (inode->i_zone[block]=new_block(inode->i_dev)) {
				inode->i_ctime=CURRENT_TIME;
				inode->i_dirt=1;
			}
		return inode->i_zone[block];
	}
	// 一级间接块的处理
	block -= 7;
	if (block<512) {
		if (create && !inode->i_zone[7])
			if (inode->i_zone[7]=new_block(inode->i_dev)) {
				inode->i_dirt=1;
				inode->i_ctime=CURRENT_TIME;
			}
		if (!inode->i_zone[7])
			return 0;
		if (!(bh = bread(inode->i_dev,inode->i_zone[7])))
			return 0;
		i = ((unsigned short *) (bh->b_data))[block];
		if (create && !i)
			if (i=new_block(inode->i_dev)) {
				((unsigned short *) (bh->b_data))[block]=i;
				bh->b_dirt=1;
			}
		brelse(bh);
		return i;
	}
	// 二级间接块的处理
	block -= 512;
	if (create && !inode->i_zone[8])
		if (inode->i_zone[8]=new_block(inode->i_dev)) {
			inode->i_dirt=1;
			inode->i_ctime=CURRENT_TIME;
		}
	if (!inode->i_zone[8])
		return 0;
	if (!(bh=bread(inode->i_dev,inode->i_zone[8])))
		return 0;
	i = ((unsigned short *)bh->b_data)[block>>9];
	// 块号为0且有创建标志，创建一级块
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block>>9]=i;
			bh->b_dirt=1;
		}
	brelse(bh);
	if (!i)
		return 0;
	if (!(bh=bread(inode->i_dev,i)))
		return 0;
	i = ((unsigned short *)bh->b_data)[block&511];
	if (create && !i)
		if (i=new_block(inode->i_dev)) {
			((unsigned short *) (bh->b_data))[block&511]=i;	// 块号<511
			bh->b_dirt=1;
		}
	brelse(bh);
	return i;
}

/**
 * @brief 取文件数据块block在设备上对应的逻辑块号
 * 
 * @param inode 文件的内存i节点指针
 * @param block 文件中的数据块号
 * @return int >0 #成功，逻辑块号 | 0 #失败
 */
int bmap(struct m_inode * inode,int block)
{
	return _bmap(inode,block,0);
}

/**
 * @brief 取文件数据块block在设备上对应的逻辑块号，如果对应逻辑块不存在，则创建
 * 
 * @param inode 文件的内存i节点指针
 * @param block 文件中的数据块号
 * @return int int >0 #成功，逻辑块号 | 0 #失败
 */
int create_block(struct m_inode * inode, int block)
{
	return _bmap(inode,block,1);
}
		
		// @read again
/**
 * @brief 回收i节点资源
 * 
 * @param inode 
 */
void iput(struct m_inode * inode)
{
	if (!inode)
		return;
	wait_on_inode(inode);
	if (!inode->i_count)
		panic("iput: trying to free free inode");
	
	
	// 管道类型的处理
	//	唤醒等待，如果还有引用，则退出，没有引用则释放处理
	if (inode->i_pipe) {
		wake_up(&inode->i_wait);
		wake_up(&inode->i_wait2);
		if (--inode->i_count)
			return;
		free_page(inode->i_size);
		inode->i_count=0;
		inode->i_dirt=0;
		inode->i_pipe=0;
		return;
	}
	// 设备号已重置
	if (!inode->i_dev) {
		inode->i_count--;  
		return;
	}
	// 如果是块设备，设备号放i_zone[0]中，同步刷新该设备
	if (S_ISBLK(inode->i_mode)) {
		sync_dev(inode->i_zone[0]);
		wait_on_inode(inode);
	}
repeat:
	// 还有其他任务使用
	if (inode->i_count>1) {
		inode->i_count--;
		return;
	}
	// 链接数为0
	if (!inode->i_nlinks) {
		truncate(inode);
		free_inode(inode);
		return;
	}
	// 有脏数据，回写，并等待解锁
	if (inode->i_dirt) {
		write_inode(inode);	/* we can sleep - so do again */
		wait_on_inode(inode);
		goto repeat;
	}
	// 引用=1，链接数!=0，内容没被修改  扣除引用
	inode->i_count--;
	return;
}

/**
 * @brief 从i节点表中获取一个空节点 （无锁/不脏/引用计数为1） 
 * 会加引用计数
 * @return struct m_inode* i节点指针 | NULL
 */
struct m_inode * get_empty_inode(void)
{
	struct m_inode * inode;
	static struct m_inode * last_inode = inode_table;
	int i;

	do {
		inode = NULL;
		// 找到一个没引用的节点，如果这个节点是不脏无锁的，直接返回
		
		for (i = NR_INODE; i ; i--) {
			if (++last_inode >= inode_table + NR_INODE)
				last_inode = inode_table;
			if (!last_inode->i_count) {
				inode = last_inode;
				if (!inode->i_dirt && !inode->i_lock)
					break;
			}
		}
		// 说明在这里必须要找到一个无引用的节点
		if (!inode) {
			for (i=0 ; i<NR_INODE ; i++)
				printk("%04x: %6d\t",inode_table[i].i_dev,
					inode_table[i].i_num);
			panic("No free inodes in mem");
		}
		// 执行在这里表示inode是无引用的，但可能是脏的或者被锁了
		// 所以需要处理脏标记和锁标记
		wait_on_inode(inode);
		while (inode->i_dirt) {
			write_inode(inode);
			wait_on_inode(inode);
		}
		// 在清楚脏（写数据）或者解锁后，因为有可能会睡眠，从而导致又有其他任务引用，再次循环处理
	} while (inode->i_count);
	// 找到空i节点，清除其所有数据，引用开始计数
	memset(inode,0,sizeof(*inode));	
	inode->i_count = 1;
	return inode;
}

/**
 * @brief 获取管道节点。引用计数直接变2，读写端，初始化管道头尾 i_zone[0] i_zone[1]
 *
 * @return struct m_inode* = NULL # 失败 | pointer # 成功
 */
struct m_inode * get_pipe_inode(void)
{
	struct m_inode * inode;

	if (!(inode = get_empty_inode()))
		return NULL;
	if (!(inode->i_size=get_free_page())) {
		inode->i_count = 0;
		return NULL;
	}
	inode->i_count = 2;	/* sum of readers/writers */
	PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
	inode->i_pipe = 1;
	return inode;
}

/**
 * @brief 
 *  @read again
 * @param dev 
 * @param nr 
 * @return struct m_inode* i节点指针  | NULL
 */
struct m_inode * iget(int dev,int nr)
{
	struct m_inode * inode, * empty;

	if (!dev)
		panic("iget with dev==0");
	empty = get_empty_inode();
	inode = inode_table;
	while (inode < NR_INODE+inode_table) {
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode++;
			continue;
		}
		wait_on_inode(inode);
		if (inode->i_dev != dev || inode->i_num != nr) {
			inode = inode_table;
			continue;
		}
		// 找到设备和块号相等的i节点
		inode->i_count++;
		// 挂接类型处理
		// 是否是另一个文件系统的安装点
		if (inode->i_mount) {
			int i;

			// 扫描获得超级块
			for (i = 0 ; i<NR_SUPER ; i++)
				if (super_block[i].s_imount==inode)
					break;
			// 不是超级块 退出，收回empty
			if (i >= NR_SUPER) {
				printk("Mounted inode hasn't got sb\n");
				if (empty)
					iput(empty);
				return inode;
			}
			iput(inode);
			dev = super_block[i].s_dev;
			nr = ROOT_INO;
			inode = inode_table;
			continue;
		}
		if (empty)
			iput(empty);
		return inode;
	}
	if (!empty)
		return (NULL);
	inode=empty;
	inode->i_dev = dev;
	inode->i_num = nr;
	read_inode(inode);
	return inode;
}

/**
 * @brief 读取指定i节点信息
 * 
 * @param inode 
 */
static void read_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to read inode without dev");
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;		// 以d_inode的大小计算
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	*(struct d_inode *)inode =
		((struct d_inode *)bh->b_data)
			[(inode->i_num-1)%INODES_PER_BLOCK];
	brelse(bh);
	// 如果是块设备
	if (S_ISBLK(inode->i_mode)) {
		int i = inode->i_zone[0];	// 块设备的设备号
		if (blk_size[MAJOR(i)])	// 如果是
			inode->i_size = 1024*blk_size[MAJOR(i)][MINOR(i)];
		else
			inode->i_size = 0x7fffffff;
	}
	unlock_inode(inode);
}

/**
 * @brief 把内存中的i节点信息写入到其对应的设备块的高速缓冲区，最终通过高速缓冲区写入设备中
 * 期间会对i节点加锁
 * @param inode 
 */
static void write_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;
	int block;

	lock_inode(inode);
	if (!inode->i_dirt || !inode->i_dev) {
		unlock_inode(inode);
		return;
	}
	if (!(sb=get_super(inode->i_dev)))
		panic("trying to write inode without device");
	
	// 计算在设备中的逻辑块号
	block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks +
		(inode->i_num-1)/INODES_PER_BLOCK;		
	if (!(bh=bread(inode->i_dev,block)))
		panic("unable to read i-node block");
	// 这里是通过类型转换赋值，把内存中的inode信息赋值到其对应的i节点缓冲区块中，最后通过缓冲区块写到设备中
	((struct d_inode *)bh->b_data)
		[(inode->i_num-1)%INODES_PER_BLOCK] =
			*(struct d_inode *)inode;
	bh->b_dirt=1;
	inode->i_dirt=0;
	brelse(bh);
	unlock_inode(inode);
}
