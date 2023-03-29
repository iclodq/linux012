/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/sched.h>
#include <linux/kernel.h>

// 清理块数据置0 
#define clear_block(addr) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"stosl" \
	::"a" (0),"c" (BLOCK_SIZE/4),"D" ((long) (addr)):"cx","di")

/// 设置指定地址开始的多少位偏移的比特位为1
#define set_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btsl %2,%3\n\tsetb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

/// 清零指定比特位，指定地址开始的多少位偏移的比特位为0
#define clear_bit(nr,addr) ({\
register int res __asm__("ax"); \
__asm__ __volatile__("btrl %2,%3\n\tsetnb %%al": \
"=a" (res):"0" (0),"r" (nr),"m" (*(addr))); \
res;})

/// 找到第一个0位的偏移
#define find_first_zero(addr) ({ \
int __res; \
__asm__("cld\n" \
	"1:\tlodsl\n\t" \
	"notl %%eax\n\t" \
	"bsfl %%eax,%%edx\n\t" \
	"je 2f\n\t" \
	"addl %%edx,%%ecx\n\t" \
	"jmp 3f\n" \
	"2:\taddl $32,%%ecx\n\t" \
	"cmpl $8192,%%ecx\n\t" \
	"jl 1b\n" \
	"3:" \
	:"=c" (__res):"c" (0),"S" (addr):"ax","dx","si"); \
__res;})

/// 释放设备dev上的指定逻辑块号
int free_block(int dev, int block)
{
	struct super_block * sb;
	struct buffer_head * bh;

	// 检查设备是否存在
	if (!(sb = get_super(dev)))
		panic("trying to free block on nonexistent device");
	// 检查块号是否合法
	if (block < sb->s_firstdatazone || block >= sb->s_nzones)
		panic("trying to free block not in datazone");
	
	// hash表中找到对应缓冲区块
	bh = get_hash_table(dev,block);
	if (bh) {
		if (bh->b_count > 1) {	// 还有其他进程使用
			brelse(bh);
			return 0;
		}
		// 没有其他进程使用
		bh->b_dirt=0;
		bh->b_uptodate=0;
		if (bh->b_count)
			brelse(bh);
	}
	block -= sb->s_firstdatazone - 1 ;	// 得到相对于数据区的块号 就是从数据区数是第几块
	// 这里回有点绕
	if (clear_bit(block&8191,sb->s_zmap[block/8192]->b_data)) {
		printk("block (%04x:%d) ",dev,block+sb->s_firstdatazone-1);
		printk("free_block: bit already cleared\n");
	}
	sb->s_zmap[block/8192]->b_dirt = 1;	// 位图有修改
	return 1;
}

/**
 * @brief 
 * 
 * @param dev 
 * @return int 
 */
int new_block(int dev)
{
	struct buffer_head * bh;
	struct super_block * sb;
	int i,j;

	if (!(sb = get_super(dev)))
		panic("trying to get new block from nonexistant device");
	j = 8192;
	// 获取位图偏移
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_zmap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	// 检查偏移合法
	if (i>=8 || !bh || j>=8192)
		return 0;
	// 设置位图
	if (set_bit(j,bh->b_data))
		panic("new_block: bit already set");
	bh->b_dirt = 1;
	j += i*8192 + sb->s_firstdatazone-1;  // 计算得到逻辑块号
	if (j >= sb->s_nzones)
		return 0;

	if (!(bh=getblk(dev,j)))
		panic("new_block: cannot get block");
	if (bh->b_count != 1)
		panic("new block: count is != 1");

	clear_block(bh->b_data);// 清理块数据
	bh->b_uptodate = 1;  	// 值最新标记
	bh->b_dirt = 1;			// 置脏标记
	brelse(bh);
	return j;
}

void free_inode(struct m_inode * inode)
{
	struct super_block * sb;
	struct buffer_head * bh;

	if (!inode)
		return;
	if (!inode->i_dev) {
		memset(inode,0,sizeof(*inode));
		return;
	}
	if (inode->i_count>1) {
		printk("trying to free inode with count=%d\n",inode->i_count);
		panic("free_inode");
	}
	if (inode->i_nlinks)
		panic("trying to free inode with links");
	if (!(sb = get_super(inode->i_dev)))
		panic("trying to free inode on nonexistent device");
	if (inode->i_num < 1 || inode->i_num > sb->s_ninodes)
		panic("trying to free inode 0 or nonexistant inode");
	if (!(bh=sb->s_imap[inode->i_num>>13]))
		panic("nonexistent imap in superblock");
	if (clear_bit(inode->i_num&8191,bh->b_data))
		printk("free_inode: bit already cleared.\n\r");
	bh->b_dirt = 1;
	memset(inode,0,sizeof(*inode));
}

/**
 * @brief 在设备中创建一个新的inode节点，相应位图置一，并初始化i节点数据
 * 
 * @param dev 
 * @return struct m_inode* 
 */
struct m_inode * new_inode(int dev)
{
	struct m_inode * inode;
	struct super_block * sb;
	struct buffer_head * bh;
	int i,j;

	if (!(inode=get_empty_inode()))
		return NULL;
	if (!(sb = get_super(dev)))
		panic("new_inode with unknown device");
	j = 8192;
	for (i=0 ; i<8 ; i++)
		if (bh=sb->s_imap[i])
			if ((j=find_first_zero(bh->b_data))<8192)
				break;
	
	// 检查找到的位图位置是否合法
	if (!bh || j >= 8192 || j+i*8192 > sb->s_ninodes) {
		iput(inode);
		return NULL;
	}
	// 设置位图
	if (set_bit(j,bh->b_data))
		panic("new_inode: bit already set");
	
	bh->b_dirt = 1;  // 对位图设置脏标记
	// 初始化i节点数据
	inode->i_count=1;				// 引用加一
	inode->i_nlinks=1;				// 链接数量
	inode->i_dev=dev;
	inode->i_uid=current->euid;		// 使用有效用户id和有效组id赋值
	inode->i_gid=current->egid;
	inode->i_dirt=1;
	inode->i_num = j + i*8192;   // 块号
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	return inode;
}
