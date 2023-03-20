/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7		// 读一个扇区允许的最大错误次数
#define MAX_HD		2		// 系统支持的最大硬盘数

// 重新校正处理函数
// 复位操作时在硬盘中断处理程序中调用此函数
static void recal_intr(void);
// 读写硬盘失败处理函数
// 结束该次请求项 会设置重试标记
static void bad_rw_intr(void);

// 重新校正标记， 设置后，程序会调用recal_intr 将磁头移动到0柱面
static int recalibrate = 0;
// 复位标记
static int reset = 0;

/*
 *  This struct defines the HD's and their types.
 */
 // 硬盘信息结构
struct hd_i_struct {
	int head;			// 磁头数量
	int sect;			// 每磁道扇区
	int cyl;			// 柱面数
	int wpcom;			// 写前补偿柱面号
	int lzone;			// 磁头着陆区柱面号
	int ctl;			// 控制字节
	};

// 初始化硬盘结构信息
#ifdef HD_TYPE
	// include/linux/config.h 定义了就取其配置
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
// 默认为0， 会在setup中设置
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;	// 硬盘数量
#endif

// 定义硬盘分区结构
// 给出每个分区从硬盘0道开始算起的物理其实扇区号和该分区的扇区总数
// hd[0] hd[5] 代表整个硬盘的参数
// hd[1-4] hd[6-9] 都表示分区， 所以一个块硬盘只能分个区
static struct hd_struct {
	long start_sect;		// 分区在硬盘中的起始物理（绝对）扇区
	long nr_sects;			// 分区中扇区总数
} hd[5*MAX_HD]={{0,0},};

// 硬盘每个分区的数据块总数组
static int hd_sizes[5*MAX_HD] = {0, };

// 读端口 汇编宏， 读端口port，读取nr*2字节，保存在buff中
#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")

// 写端口 汇编宏，写端口port,从buf中写nr*2字节
#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")

// 硬盘中断 定义在 sys_call.s
extern void hd_interrupt(void);
// 虚拟盘创建加载函数 定义在 ramdisk.c
extern void rd_load(void);

/* This may be used only once, enforced by 'static int callable' */
// 系统设置函数  只能调用一次callable限制 
//	除妖读取参数，设置硬盘分区结构信息hd,并尝试加载RAM虚拟盘和根文件系统
// 参数 BIOS 由初始化程序 init/main.c 中 init子程序设置为指向硬盘参数表结构的指针
// 数据从内存 0x90080复制，最开始时由setup.s程序利用ROM BIOS取得

int sys_setup(void * BIOS)
{
	static int callable = 1;
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;

	if (!callable)
		return -1;
	callable = 0;
#ifndef HD_TYPE
	for (drive=0 ; drive<2 ; drive++) {
		hd_info[drive].cyl = *(unsigned short *) BIOS;
		hd_info[drive].head = *(unsigned char *) (2+BIOS);
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS);
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS);
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS);
		hd_info[drive].sect = *(unsigned char *) (14+BIOS);
		BIOS += 16;		// 外部BIOS 硬盘参数 16字节
	}
	// 在setup.s中，如果只有1个硬盘，会对第2个结构的数据都清零
	// 所以可以根据第2个结构判断硬盘数量有几个
	if (hd_info[1].cyl)
		NR_HD=2;
	else
		NR_HD=1;
#endif
	for (i=0 ; i<NR_HD ; i++) {
		hd[i*5].start_sect = 0;	// 设置起始扇区号
		hd[i*5].nr_sects = hd_info[i].head* 			// 总扇区数=磁头数*每磁道扇区数*柱面数
				hd_info[i].sect*hd_info[i].cyl;
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/

	// 从CMOS 读值 再次校验
	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
	// 清零 如果硬盘数量读出来不对
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}
	for (drive=0 ; drive<NR_HD ; drive++) {
		// 0x300 0x305 是硬盘设备号
		// 读取硬盘的第一个块号
		if (!(bh = bread(0x300 + drive*5,0))) {
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		// 根据第一个扇区的最后两个字节  判断是否为硬盘标志 0xAA55 小端
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) {
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		// 获取分区表信息  分区表位置0x1BE
		p = 0x1BE + (void *)bh->b_data;
		for (i=1;i<5;i++,p++) {
			hd[i+5*drive].start_sect = p->start_sect;
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		brelse(bh);	// 释放缓冲区
	}
	for (i=0 ; i<5*MAX_HD ; i++)
		hd_sizes[i] = hd[i].nr_sects>>1 ;	// 设置硬盘的数据块数量  一个数据块=2个扇区 
	blk_size[MAJOR_NR] = hd_sizes;
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	
	// 尝试在内存虚拟盘中加载启动盘中包含的根文件系统镜像
	// 		如果启动盘上有根文件系统镜像，则尝试放到虚拟盘中，
	//		并把根文件系统设备号ROOT_DEV修改为虚拟盘的设备号
	//		然后初始化交换设备，最后安装根文件系统
	rd_load();				// 加载 虚拟内存盘
	init_swapping();		// 初始化虚拟内存盘 swap.c
	mount_root();			// 挂在根文件系统	fs/super.c
	return (0);
}

// 等待硬盘控制器准备就绪
// reutrn = >0#表示在设定时间内ok | =0#表示等待超时，设定时间内还是没有准备好
static int controller_ready(void)
{
	int retries = 100000;
	// 0xc0 =  (SEEK_STAT | WRERR_STAT | READY_STAT | BUSY_STAT)
	// 0x40 = READY_STAT
	// 这么写 应该是冗余的检测
	while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40);
	return (retries);
}

// 检测硬盘执行命令后的状态，(win表示温切斯特硬盘的缩写)
// return = 0#正常 | 1#出错 错误号在错误寄存器中HD_ERROR(0x1f1)
static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	// 这里是检测的  当前为  寻道和就绪 其他已复位
	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);  // 有错误读错误码
	return (1);
}

// 向硬盘控制器发送命令块
static void hd_out(
		unsigned int drive,			// 硬盘号 0-1
		unsigned int nsect,			// 读写扇区数
		unsigned int sect,			// 起始扇区
		unsigned int head,			// 磁头号
		unsigned int cyl,			// 柱面号
		unsigned int cmd,			// 命令码
		void (*intr_addr)(void))	// 硬盘中断处理程序的函数指针
{
	register int port asm("dx");

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())		// 等控制器就绪
		panic("HD controller not ready");
	SET_INTR(intr_addr);
	outb_p(hd_info[drive].ctl,HD_CMD);
	port=HD_DATA;
	outb_p(hd_info[drive].wpcom>>2,++port);	// 写预补偿柱面号 （需除以4？？）
	outb_p(nsect,++port);					// 操作扇区总数 和读error同一个地址
	outb_p(sect,++port);					// 起始扇区
	outb_p(cyl,++port);						// 柱面号低8位
	outb_p(cyl>>8,++port);					// 柱面号高8位
	outb_p(0xA0|(drive<<4)|head,++port);	// 驱动器号+磁头号 公用
	outb(cmd,++port);						// 刷命令  读/写
}

// 等待硬盘就绪
static int drive_busy(void)
{
	unsigned int i;
	unsigned char c;

	for (i = 0; i < 50000; i++) {
		c = inb_p(HD_STATUS);
		c &= (BUSY_STAT | READY_STAT | SEEK_STAT);
		if (c == (READY_STAT | SEEK_STAT))
			return 0;
	}
	printk("HD controller times out\n\r");
	return(1);
}

// 尝试复位控制器
static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);			// 发送复位新号
	for(i = 0; i < 1000; i++) nop();	// 空等
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

// 硬盘复位操作
static void reset_hd(void)
{
	static int i;

repeat:
	if (reset) {
		reset = 0;
		i = -1;
		reset_controller();
	} else if (win_result()) {
		bad_rw_intr();
		if (reset)
			goto repeat;
	}
	i++;
	if (i < NR_HD) {
		hd_out(i,hd_info[i].sect,hd_info[i].sect,hd_info[i].head-1,
			hd_info[i].cyl,WIN_SPECIFY,&reset_hd);
	} else
		do_hd_request();
}

// 硬盘意外中断调用的默认函数
void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
	reset = 1;
	do_hd_request();
}

// 读写失败处理
//		超过最大次数，则结束请求
//		超过最大次数的一般，则设置复位标记
static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}

// 读扇区中断中断调用函数
static void read_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256);
	CURRENT->errors = 0;
	CURRENT->buffer += 512;
	CURRENT->sector++;
	if (--CURRENT->nr_sectors) {
		// 未读完，继续
		SET_INTR(&read_intr);
		return;
	}
	// 读操作结束
	end_request(1);
	do_hd_request();
}

// 写扇区 中断调用函数
static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		SET_INTR(&write_intr);
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}

// 硬盘中断服务程序中调用的重新校正
static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

// 硬盘操作超时处理函数
void hd_times_out(void)
{
	if (!CURRENT)
		return;
	printk("HD timeout");
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	SET_INTR(NULL);
	reset = 1;
	do_hd_request();
}

// 执行硬盘读写请求操作
void do_hd_request(void)
{
	int i,r;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

/*
INIT_REQUEST 展开
repeat: 
	if (!CURRENT) {
		CLEAR_DEVICE_INTR 
		CLEAR_DEVICE_TIMEOUT 
		return; 
	} 
	if (MAJOR(CURRENT->dev) != MAJOR_NR) 
		panic(DEVICE_NAME ": request list destroyed"); 
	if (CURRENT->bh) { 
		if (!CURRENT->bh->b_lock) 
			panic(DEVICE_NAME ": block not locked"); 
	}
*/

	INIT_REQUEST;
	dev = MINOR(CURRENT->dev);
	block = CURRENT->sector;
	// 检查有效性 block+2是因为每次至少操作2个扇区
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) {
		end_request(0);
		goto repeat;
	}
	block += hd[dev].start_sect;	// 现在是物理扇区位置
	dev /= 5;						// 硬盘全局信息 0/1
	// 根据分区表信息 计算磁盘寻道坐标
	//		代码等价
	/*
		sec = block % hd_info[dev].sect				// 得到扇区标记
		block = block / hd_info[dev].sect			// 
		cyl = block / /hd_info[dev].cyl				// 得到柱面号
		head = block % hd_info[dev].cyl				// 得到磁道号
		由此可反推 hdsect[cyl][head][sect] 物理扇区地址 = 
			cyl * (dev.cyl * dev.sect) + head * dev.sect + sect
	*/
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect));
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++;		// 调整？ 为啥呢
	nsect = CURRENT->nr_sectors;
	if (reset) {
		recalibrate = 1;
		reset_hd();		// 重置一下硬盘状态
		return;
	}
	/// 校准
	if (recalibrate) {
		recalibrate = 0;
		// 设置 恢复 硬盘会执行寻道操作，磁头停在0柱面
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr);
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		// 发送请求命令给硬盘控制器
		// 并检测 请求是否准备就绪
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		for(i=0 ; i<10000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
		if (!r) {
			// 超时处理
			bad_rw_intr();
			goto repeat;
		}
		// 发数据给硬盘控制器
		port_write(HD_DATA,CURRENT->buffer,256);
	} else if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}

// 硬盘系统初始化
void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;	// 设置请求函数指针
	set_intr_gate(0x2E,&hd_interrupt);				// 设置中断门处理函数指针
	outb_p(inb_p(0x21)&0xfb,0x21);					// 复位主片屏蔽位?
	outb(inb_p(0xA1)&0xbf,0xA1);					// 复位从片屏蔽位？
}
