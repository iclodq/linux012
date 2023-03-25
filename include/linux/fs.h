/*
 * This file has definitions for some important file table
 * structures etc.
 */

#ifndef _FS_H
#define _FS_H

#include <sys/types.h>

/* devices are as follows: (same as minix, so we can use the minix
 * file system. These are major numbers.)
 *
 * 0 - unused (nodev)
 * 1 - /dev/mem
 * 2 - /dev/fd
 * 3 - /dev/hd
 * 4 - /dev/ttyx
 * 5 - /dev/tty
 * 6 - /dev/lp
 * 7 - unnamed pipes
 */

#define IS_SEEKABLE(x) ((x)>=1 && (x)<=3)

#define READ 0
#define WRITE 1
#define READA 2		/* read-ahead - don't pause */
#define WRITEA 3	/* "write-ahead" - silly, but somewhat useful */

void buffer_init(long buffer_end);

#define MAJOR(a) (((unsigned)(a))>>8)
#define MINOR(a) ((a)&0xff)

#define NAME_LEN 14
#define ROOT_INO 1

#define I_MAP_SLOTS 8
#define Z_MAP_SLOTS 8
#define SUPER_MAGIC 0x137F

#define NR_OPEN 20
#define NR_INODE 64
#define NR_FILE 64
#define NR_SUPER 8
#define NR_HASH 307
#define NR_BUFFERS nr_buffers
#define BLOCK_SIZE 1024
#define BLOCK_SIZE_BITS 10
#ifndef NULL
#define NULL ((void *) 0)
#endif

#define INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct d_inode)))
#define DIR_ENTRIES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct dir_entry)))

#define PIPE_READ_WAIT(inode) ((inode).i_wait)
#define PIPE_WRITE_WAIT(inode) ((inode).i_wait2)
#define PIPE_HEAD(inode) ((inode).i_zone[0])
#define PIPE_TAIL(inode) ((inode).i_zone[1])
#define PIPE_SIZE(inode) ((PIPE_HEAD(inode)-PIPE_TAIL(inode))&(PAGE_SIZE-1))
#define PIPE_EMPTY(inode) (PIPE_HEAD(inode)==PIPE_TAIL(inode))
#define PIPE_FULL(inode) (PIPE_SIZE(inode)==(PAGE_SIZE-1))

#define NIL_FILP	((struct file *)0)
#define SEL_IN		1
#define SEL_OUT		2
#define SEL_EX		4

typedef char buffer_block[BLOCK_SIZE];

// 缓冲区头
struct buffer_head {
	char * b_data;			/* pointer to data block (1024 bytes) */	// 读取的数据块指针
	unsigned long b_blocknr;	/* block number */						// 块号
	unsigned short b_dev;		/* device (0 = free) */					// 数据源的设备号 0表示空闲
	unsigned char b_uptodate;											// 更新标记，表示数据是否已更新
	unsigned char b_dirt;		/* 0-clean,1-dirty */					// 修改标记，0#没有|1#有修改
	unsigned char b_count;		/* users using this block */			// 使用该块的用户数
	unsigned char b_lock;		/* 0 - ok, 1 -locked */					// 是否被锁定 =0#未锁|1#锁住
	struct task_struct * b_wait;										// 等待此缓冲区的任务
	struct buffer_head * b_prev;										// hash队列上前一块
	struct buffer_head * b_next;										// hash队列上后一块
	struct buffer_head * b_prev_free;									// 空前列表前一块
	struct buffer_head * b_next_free;									// 空闲列表后一块
};

struct d_inode {
	unsigned short i_mode;
	unsigned short i_uid;
	unsigned long i_size;
	unsigned long i_time;
	unsigned char i_gid;
	unsigned char i_nlinks;
	unsigned short i_zone[9];
};

struct m_inode {
	unsigned short i_mode;
	unsigned short i_uid;
	unsigned long i_size;
	unsigned long i_mtime;
	unsigned char i_gid;
	unsigned char i_nlinks;
	unsigned short i_zone[9];
/* these are in memory also */
	struct task_struct * i_wait;
	struct task_struct * i_wait2;	/* for pipes */
	unsigned long i_atime;
	unsigned long i_ctime;
	unsigned short i_dev;
	unsigned short i_num;
	unsigned short i_count;
	unsigned char i_lock;
	unsigned char i_dirt;
	unsigned char i_pipe;
	unsigned char i_mount;
	unsigned char i_seek;
	unsigned char i_update;
};

struct file {
	unsigned short f_mode;
	unsigned short f_flags;
	unsigned short f_count;
	struct m_inode * f_inode;
	off_t f_pos;
};
/*
  磁盘
  ┌────────┬───────┬─────────┬──────────┬──────────────────────────┬────────────────────────────────┐
  │ boot   │ super │ inode   │ logic    │   inode inode inode ...  │            data zone           │
  │ block  │ block │ bitmap  │ bitmap   │                          │                                │
  │        │       │         │          │                          │                                │
  │        │       │         │          │                          │                                │
  │        │       │         │          │                          │                                │
  └────────┴───────┴─────────┴──────────┴──────────────────────────┴────────────────────────────────┘
  逻辑块位图中每个比特位一次代表盘上数据区的一个逻辑块 所以第一位bit位置1，表示数据区的第一个块，而不是整个磁盘的第一个块（引导块）
  当数组区中的一个块被占用后，逻辑块位图中对应的比特位置一。

  由于查找空闲数据块的函数在所有数据块都被占用时会返回0，因此逻辑块位图最低比特位会置一，然后闲置不用。
*/


/// 超级块，用于存放设备上文件系统的结构信息，并说明各部分的大小。
struct super_block {
	unsigned short s_ninodes;			// 设备上的inode节点数量
	unsigned short s_nzones;			// 设备上以逻辑块为单位的总数
	unsigned short s_imap_blocks;		// inode的位图
	unsigned short s_zmap_blocks;		// 逻辑块的位图
	unsigned short s_firstdatazone;		// 数据区中第一个逻辑块块号
	unsigned short s_log_zone_size;		// log2(磁盘块数/逻辑块数)
	unsigned long s_max_size;			// 最大文件长度
	unsigned short s_magic;				// 文件系统幻数
/* These are only in memory */
	struct buffer_head * s_imap[8];		// i节点位图在高速缓冲块指针数组
	struct buffer_head * s_zmap[8];		// 逻辑块位图在高速缓冲块指针数组
	unsigned short s_dev;				// 超级快所在设备号
	struct m_inode * s_isup;			// 被安装文件系统的根目录的i节点
	struct m_inode * s_imount;			// 该文件系统被安装到的i节点
	unsigned long s_time;				// 修改时间
	struct task_struct * s_wait;		// 等待此超级块的进程指针
	unsigned char s_lock;				// 锁定标志
	unsigned char s_rd_only;			// 只读标志
	unsigned char s_dirt;				// 被修改后的脏标记
};

struct d_super_block {
	unsigned short s_ninodes;
	unsigned short s_nzones;
	unsigned short s_imap_blocks;
	unsigned short s_zmap_blocks;
	unsigned short s_firstdatazone;
	unsigned short s_log_zone_size;
	unsigned long s_max_size;
	unsigned short s_magic;
};

// 文件目录项
struct dir_entry {
	unsigned short inode;		// i节点
	char name[NAME_LEN];		// 文件名
};

extern struct m_inode inode_table[NR_INODE];
extern struct file file_table[NR_FILE];
extern struct super_block super_block[NR_SUPER];
extern struct buffer_head * start_buffer;
extern int nr_buffers;

extern void check_disk_change(int dev);
extern int floppy_change(unsigned int nr);
extern int ticks_to_floppy_on(unsigned int dev);
extern void floppy_on(unsigned int dev);
extern void floppy_off(unsigned int dev);
extern void truncate(struct m_inode * inode);
extern void sync_inodes(void);
extern void wait_on(struct m_inode * inode);
extern int bmap(struct m_inode * inode,int block);
extern int create_block(struct m_inode * inode,int block);
extern struct m_inode * namei(const char * pathname);
extern struct m_inode * lnamei(const char * pathname);
extern int open_namei(const char * pathname, int flag, int mode,
	struct m_inode ** res_inode);
extern void iput(struct m_inode * inode);
extern struct m_inode * iget(int dev,int nr);
extern struct m_inode * get_empty_inode(void);
extern struct m_inode * get_pipe_inode(void);
extern struct buffer_head * get_hash_table(int dev, int block);
extern struct buffer_head * getblk(int dev, int block);

/// 低级读写块,会把hb中所指的数据写入设备中，
//	或者从设备读取bh设定指定块数据到缓冲区
extern void ll_rw_block(int rw, struct buffer_head * bh);
extern void ll_rw_page(int rw, int dev, int nr, char * buffer);
extern void brelse(struct buffer_head * buf);
// 读取设备号dev的 指定块的数据(block 从0开始)
extern struct buffer_head * bread(int dev,int block);
extern void bread_page(unsigned long addr,int dev,int b[4]);
extern struct buffer_head * breada(int dev,int block,...);
extern int new_block(int dev);
extern int free_block(int dev, int block);
extern struct m_inode * new_inode(int dev);
extern void free_inode(struct m_inode * inode);
extern int sync_dev(int dev);
extern struct super_block * get_super(int dev);
extern int ROOT_DEV;

// 挂在根文件系统  定义在super.c中
extern void mount_root(void);

#endif
