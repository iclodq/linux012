#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#include <sys/types.h>
#include <sys/param.h>

// 保存系统的一些信息
struct utsname {
	char sysname[9];					// 操作系统名字Linux
	char nodename[MAXHOSTNAMELEN+1];	// 网络节点名字（主机名） (none)  hostname
	char release[9];					// 操作系统发行级别	0
	char version[9];					// 操作系统版本号		0.12
	char machine[9];					// 系统运行的硬件类型名称 i386
};

extern int uname(struct utsname * utsbuf);

#endif
