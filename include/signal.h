#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

typedef int sig_atomic_t;
typedef unsigned int sigset_t;		/* 32 bits */

#define _NSIG             32
#define NSIG		_NSIG

/*
信号	信号	系统默认操作	描述
1	SIGHUP	Abort	(Hangup)当你不再有控制终端时内核会产生该信号，或者当你关闭Xterm或断开modem。由于后台程序没有控制的终端，因而它们常用SIGHUP来发出需要重新读取其配置文件的信号。
2	SIGINT	Abort	(Interrupt)来自键盘的中断。通常终端驱动程序会将其与^C绑定。
3	SIGOUIT	CoreDump	(Quit)来自键盘的退出中断。通常终端驱动程序会将其与A绑定
4	SIGILL	CoreDump	(IllegalInstruction)程序出错或者执行了一条非法操作指令。
5	SIGTRAP		(Breakpoint/Trace Trap)调试用，跟踪断点
6	SIGABRT	CoreDump	(Abort)放弃执行，异常结束
6	SIGIOT	CoreDump	(IO Trap)同 SIGABRT
7	SIGUNUSED		(Unused)没有使用
8	SIGFPE	CoreDump	(Floating Point Exception)浮点异常
9	SIGKILL	Abort	(Kill)程序被终止。该信号不能被捕获或者被忽略。想立刻终止个进程，就发送信号9。注意程序将没有任何机会做清理工作
10	SIGUSR1	Abort	(User defined Signal1)用户定义的信号。
11	SIGSEGV	CoreDump	(SegmentationViolation)当程序引用无效的内存时会产生此信号比如:寻址没有映射的内存;寻址未许可的内存。
12	SIGUSR2		(User definedSignal2)保留给用户程序用于IPC或其他目的
13	SIGPIPE	Abort	(Pipe)当程序向一个套接字或管道写时由于没有读者而产生该信号
14	SIGALRM	Abort	(Alarm)该信号会在用户调用 alarm系统调用所设置的延迟时间到后产生。该信号常用于判别系统调用超时
15	SIGTERM	Abort	(Terminate)用于和善地要求一个程序终止。它是 kill 的默认信号与SIGKILL不同，该信号能被捕获，这样就能在退出运行前做清理工作。
16	SIGSTKFLT	Abort	(Stack faulton coprocessor)协处理器堆栈错误。
17	SIGCHLD	Ignore	(Child)子进程发出。子进程已停止或终止。可改变其含义挪作它用
18	SIGCONT		(Continue)该信号致使被 SIGSTOP停止的进程恢复运行。可以被捕获。
19	SIGSTOP		(Stop)停止进程的运行。该信号不可被捕获或忽略。
20	SIGTSTP		(TerminalStop)向终端发送停止键序列。该信号可以被捕获或忽略。
21	SIGTTIN		(TTYInputon Background)后台进程试图从一个不再被控制的终端上读取数据，此时该进程将被停止，直到收到 SIGCONT 信号。该信号可以被捕获或忽略。
22	SIGTTOU		(TTYOutput on Background)后台进程试图向一个不再被控制的终端上输出数据，此时该进程将被停止，直到收到SIGCONT信号该信号可被捕获或忽略。
*/
#define SIGHUP		 1   // Abort	(Hangup)当你不再有控制终端时内核会产生该信号，或者当你关闭Xterm或断开modem。由于后台程序没有控制的终端，因而它们常用SIGHUP来发出需要重新读取其配置文件的信号。
#define SIGINT		 2
#define SIGQUIT		 3
#define SIGILL		 4
#define SIGTRAP		 5
#define SIGABRT		 6
#define SIGIOT		 6
#define SIGUNUSED	 7
#define SIGFPE		 8
#define SIGKILL		 9
#define SIGUSR1		10
#define SIGSEGV		11
#define SIGUSR2		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGSTKFLT	16
#define SIGCHLD		17
#define SIGCONT		18
#define SIGSTOP		19
#define SIGTSTP		20
#define SIGTTIN		21
#define SIGTTOU		22

/* Ok, I haven't implemented sigactions, but trying to keep headers POSIX */
#define SA_NOCLDSTOP	1
#define SA_INTERRUPT	0x20000000
#define SA_NOMASK	0x40000000
#define SA_ONESHOT	0x80000000

#define SIG_BLOCK          0	/* for blocking signals */
#define SIG_UNBLOCK        1	/* for unblocking signals */
#define SIG_SETMASK        2	/* for setting the signal mask */

#define SIG_DFL		((void (*)(int))0)	/* default signal handling */
#define SIG_IGN		((void (*)(int))1)	/* ignore signal */
#define SIG_ERR		((void (*)(int))-1)	/* error return from signal */

#ifdef notdef
#define sigemptyset(mask) ((*(mask) = 0), 1)
#define sigfillset(mask) ((*(mask) = ~0), 1)
#endif

struct sigaction {
	void (*sa_handler)(int);		// =1 忽略信号执行
	sigset_t sa_mask;
	int sa_flags;
	void (*sa_restorer)(void);
};

void (*signal(int _sig, void (*_func)(int)))(int);
int raise(int sig);
int kill(pid_t pid, int sig);
int sigaddset(sigset_t *mask, int signo);
int sigdelset(sigset_t *mask, int signo);
int sigemptyset(sigset_t *mask);
int sigfillset(sigset_t *mask);
int sigismember(sigset_t *mask, int signo); /* 1 - is, 0 - not, -1 error */
int sigpending(sigset_t *set);
int sigprocmask(int how, sigset_t *set, sigset_t *oldset);
int sigsuspend(sigset_t *sigmask);
int sigaction(int sig, struct sigaction *act, struct sigaction *oldact);

#endif /* _SIGNAL_H */
