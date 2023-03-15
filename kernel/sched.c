/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

// 获取信号在信号位图中的对应位的二进制数值   nr=5, -> 0b10000
// 除了SIGKILL 和SIGSTOP信号外，其他信号都是可阻塞的

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

// 调试函数，显示任务号位nr的进程号进程状态等
// j表示内核栈最低的栈顶位置，因为任务的内核态栈是从页面末端开始
// 且任务结构task_struct和任务的内核态栈在同一内存页面
void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, father=%d, child=%d, ",nr,p->pid,
		p->state, p->p_pptr->pid, p->p_cptr ? p->p_cptr->pid : -1);
	i=0;
	while (i<j && !((char *)(p+1))[i])	// 计算内核堆栈中任务结构p后面有多少字节数据为0 
		i++;
	printk("%d/%d chars free in kstack\n\r",i,j);
	printk("   PC=%08X.", *(1019 + (unsigned long *) p));
	if (p->p_ysptr || p->p_osptr) 
		printk("   Younger sib=%d, older sib=%d\n\r", 
			p->p_ysptr ? p->p_ysptr->pid : -1,
			p->p_osptr ? p->p_osptr->pid : -1);
	else
		printk("\n\r");
}

// 打印所有任务状态据
void show_state(void)
{
	int i;

	printk("\rTask-info:\n\r");
	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

// 设置定时器中断频率 为100hz
#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);	// 定时中断  kernel/system_call.s 
extern int system_call(void);		// 系统调用中断

// 用于内核态任务堆栈结构  占用一个内存页的大小
union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};

unsigned long volatile jiffies=0;	// 嘀嗒 内核脉搏
unsigned long startup_time=0;		// 开机时间 unix时间戳 秒
// 用于累计需要调整的时间嘀嗒数
int jiffies_offset = 0;		/* # clock ticks to add to get "true
				   time".  Should always be less than
				   1 second's worth.  For time fanatics
				   who like to syncronize their machines
				   to WWV :-) */

struct task_struct *current = &(init_task.task);		// 当前任务指针(初始化指向任务0)
struct task_struct *last_task_used_math = NULL;			// 使用过协处理器任务的指针

struct task_struct * task[NR_TASKS] = {&(init_task.task), };	// 任务指针数组

long user_stack [ PAGE_SIZE>>2 ] ;	// 任务0和任务1的用户态堆栈 1KB

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };


// 当任务调度交换后，需要把交换前的任务的协处理器状态保存，并加载新的任务的协处理器状态
// 协处理器状态的存储与恢复
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		// 新任务第一次使用协处理器 向协处理器发送初始化命令
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
 // 调度函数处理
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	// 唤醒 触发超时或触发警告的 处于可中断睡眠状态的进程
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			// @doubt jiffies是啥意思,系统开机开始算起的嘀嗒数 (10ms 一嘀嗒)
			// 超时计时没有超过jiffies，则重置，如果处于睡眠可中断状态，则修改为就绪状态
			if ((*p)->timeout && (*p)->timeout < jiffies) {
				(*p)->timeout = 0;
				if ((*p)->state == TASK_INTERRUPTIBLE)
					(*p)->state = TASK_RUNNING;
			}
			// 如果任务设置了警告，且警告小于jiffies，则设置警报信号，并清空警告值
			if ((*p)->alarm && (*p)->alarm < jiffies) {
				(*p)->signal |= (1<<(SIGALRM-1));
				(*p)->alarm = 0;
			}
			// 可阻塞信号& 设置的要阻塞的信号 = 实际需要阻塞的信号
			// 在排除了需要阻塞的信号后还有信号，如果处于可中断的睡眠状态，则修改为就绪状态
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		// 这里会默认跳过任务0的处理
		while (--i) {
			if (!*--p)
				continue;
			
			// 找到任务是就绪状态且剩余运行时间最大的任务
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		// 如果c==0，表示除0号外的所有就绪任务，都已调度运行完分配的时间片（嘀嗒数）
		//		因而需要对所有任务进行重新分配时间
		// 如果c==-1，表示没有就绪任务，执行0号任务（idle），这里假定counter>=0, 查看代码确实是
		if (c) break;
		
		// 走到这里表示，已经没有可供运行的任务（比如任务的时间片用完，或者任务不是就绪状态）
		// 对所有任务进行的时间片进行赋值
		// counter = counter/2 + priority
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	// @doubt 切换执行任务
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

// 对当前任务设置成睡眠（可中断和不可中断）
// 参数有要求：
// 入参**p 表示一个等待队列的队头元素
//		state=TASK_UNINTERRUPTIBLE|TASK_INTERRUPTIBLE
static inline void __sleep_on(struct task_struct **p, int state)
{
	struct task_struct *tmp;

	if (!p)
		return;
	// 0号任务不可睡眠
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;	// 设置外部需要修改的任务指针为当前任务
	current->state = state;	// 对当前任务设置成 睡眠状态（中断或不可中断）
repeat:	schedule();
	// 执行调度后，current可能会被修改成其他任务
	// 传入的指定任务不是当前任务，则设置成就绪状态
	// 对当前任务设置成不可中断的睡眠
	if (*p && *p != current) {
		(**p).state = 0;
		current->state = TASK_UNINTERRUPTIBLE;
		goto repeat;
	}
	if (!*p)
		printk("Warning: *P = NULL\n\r");
	if (*p = tmp)
		tmp->state=0;
}

// 对当前任务 设置可中断睡眠
void interruptible_sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_INTERRUPTIBLE);
}

// 对当前任务 设置为不可中断睡眠 可用于保证中断程序和进程的同步机制
void sleep_on(struct task_struct **p)
{
	__sleep_on(p,TASK_UNINTERRUPTIBLE);
}

// 唤醒任务
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		if ((**p).state == TASK_STOPPED)
			printk("wake_up: TASK_STOPPED");
		if ((**p).state == TASK_ZOMBIE)
			printk("wake_up: TASK_ZOMBIE");
		(**p).state=0;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

// 当tiemr被触发后，fn会被置空 
// next_timer为timer的有效队列头
static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

// 新加入的timer会被放到队尾，并且其时间jiffies会依次减去排在它前面的元素的jiffies

// 从算法描述看，传入的jiffies需要是个固定值，或者比next_timer->jiffes大才正确
void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		// 找到可设置的timer的结构
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");

		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;

		// next_timer 可理解为list的队头，当插入新的timer后，会根据next链条一直处理到尾部
		// 每个当前p会与它的下一个节点交换（如果jiffies值比他大），并把它的jiffies减掉它的父节点，最终队尾的有可能为负数
		//	如果新加入的节点 jiffies 小于当前的队头节点的jiffies，逻辑就不对了  @doubt-bug
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			// 这里已经开始交换timer
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	static int blanked = 0;

	if (blankcount || !blankinterval) {
		if (blanked)
			unblank_screen();
		if (blankcount)
			blankcount--;
		blanked = 0;
	} else if (!blanked) {
		blank_screen();
		blanked = 1;
	}
	if (hd_timeout)
		if (!--hd_timeout)
			hd_times_out();

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		// 如果有timer队列
		// 调度执行所有jiffies<=0的
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;
	current->counter=0;
	if (!cpl) return;
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;	// 需要换成时间  嘀嗒数*每嘀嗒时间（0.01秒）
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

// 获取进程的pid
int sys_getpid(void)
{
	return current->pid;
}

// 获取父进程的pid
int sys_getppid(void)
{
	return current->p_pptr->pid;
}

// 获取进程uid
int sys_getuid(void)
{
	return current->uid;
}
// 获取进程有效的uid  euid
int sys_geteuid(void)
{
	return current->euid;
}

// 获取进程的组号 gid
int sys_getgid(void)
{
	return current->gid;
}

// 获取进程的有效组号  egid
int sys_getegid(void)
{
	return current->egid;
}

// 降低进程的优先级
// 当counter为0是， priority 会赋值给counter
// 而counter是调度器中每个任务需要消耗的时间片  所以减少priority，就减少了进程的CPU占用时间
int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

// 调度器初始化
void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	
	// 在全局描述表gdt中设置0号任务的任务状态信息和局部描述符表信息
	// gdt实际定义在head.s 中的_gdt 
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);
	lldt(0);
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}
