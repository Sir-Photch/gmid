/*
 * Copyright (c) 2021 Omar Polo <op@omarpolo.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "gmid.h"

#if defined(__FreeBSD__)

#include <sys/capsicum.h>

void
sandbox_server_process(void)
{
	if (cap_enter() == -1)
		fatal("cap_enter");
}

void
sandbox_executor_process(void)
{
	/* We cannot capsicum the executor process because it needs
	 * to fork(2)+execve(2) cgi scripts */
	return;
}

void
sandbox_logger_process(void)
{
	if (cap_enter() == -1)
		fatal("cap_enter");
}

#elif defined(__linux__)

#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* uncomment to enable debugging.  ONLY FOR DEVELOPMENT */
/* #define SC_DEBUG */

#ifdef SC_DEBUG
# define SC_FAIL SECCOMP_RET_TRAP
#else
# define SC_FAIL SECCOMP_RET_KILL
#endif

#if (BYTE_ORDER == LITTLE_ENDIAN)
# define SC_ARG_LO	0
# define SC_ARG_HI	sizeof(uint32_t)
#elif (BYTE_ORDER == BIG_ENDIAN)
# define SC_ARG_LO	sizeof(uint32_t)
# define SC_ARG_HI	0
#else
# error "Uknown endian"
#endif

/* make the filter more readable */
#define SC_ALLOW(nr)						\
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##nr, 0, 1),	\
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

/*
 * SC_ALLOW_ARG and the SECCOMP_AUDIT_ARCH below are courtesy of
 * https://roy.marples.name/git/dhcpcd/blob/HEAD:/src/privsep-linux.c
 */
#define SC_ALLOW_ARG(_nr, _arg, _val)						\
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, (_nr), 0, 6),			\
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,					\
	    offsetof(struct seccomp_data, args[(_arg)]) + SC_ARG_LO),		\
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K,					\
	    ((_val) & 0xffffffff), 0, 3),					\
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,					\
	    offsetof(struct seccomp_data, args[(_arg)]) + SC_ARG_HI),		\
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K,					\
	    (((uint32_t)((uint64_t)(_val) >> 32)) & 0xffffffff), 0, 1),		\
	BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),				\
	BPF_STMT(BPF_LD + BPF_W + BPF_ABS,					\
	    offsetof(struct seccomp_data, nr))

/*
 * I personally find this quite nutty.  Why can a system header not
 * define a default for this?
 */
#if defined(__i386__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_I386
#elif defined(__x86_64__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__arc__)
#  if defined(__A7__)
#    if (BYTE_ORDER == LITTLE_ENDIAN)
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARCOMPACT
#    else
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARCOMPACTBE
#    endif
#  elif defined(__HS__)
#    if (BYTE_ORDER == LITTLE_ENDIAN)
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARCV2
#    else
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARCV2BE
#    endif
#  else
#    error "Platform does not support seccomp filter yet"
#  endif
#elif defined(__arm__)
#  ifndef EM_ARM
#    define EM_ARM 40
#  endif
#  if (BYTE_ORDER == LITTLE_ENDIAN)
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARM
#  else
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ARMEB
#  endif
#elif defined(__aarch64__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_AARCH64
#elif defined(__alpha__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_ALPHA
#elif defined(__hppa__)
#  if defined(__LP64__)
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_PARISC64
#  else
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_PARISC
#  endif
#elif defined(__ia64__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_IA64
#elif defined(__microblaze__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_MICROBLAZE
#elif defined(__m68k__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_M68K
#elif defined(__mips__)
#  if defined(__MIPSEL__)
#    if defined(__LP64__)
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_MIPSEL64
#    else
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_MIPSEL
#    endif
#  elif defined(__LP64__)
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_MIPS64
#  else
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_MIPS
#  endif
#elif defined(__nds32__)
#  if (BYTE_ORDER == LITTLE_ENDIAN)
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_NDS32
#else
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_NDS32BE
#endif
#elif defined(__nios2__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_NIOS2
#elif defined(__or1k__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_OPENRISC
#elif defined(__powerpc64__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_PPC64
#elif defined(__powerpc__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_PPC
#elif defined(__riscv)
#  if defined(__LP64__)
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_RISCV64
#  else
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_RISCV32
#  endif
#elif defined(__s390x__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_S390X
#elif defined(__s390__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_S390
#elif defined(__sh__)
#  if defined(__LP64__)
#    if (BYTE_ORDER == LITTLE_ENDIAN)
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_SHEL64
#    else
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_SH64
#    endif
#  else
#    if (BYTE_ORDER == LITTLE_ENDIAN)
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_SHEL
#    else
#      define SECCOMP_AUDIT_ARCH AUDIT_ARCH_SH
#    endif
#  endif
#elif defined(__sparc__)
#  if defined(__arch64__)
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_SPARC64
#  else
#    define SECCOMP_AUDIT_ARCH AUDIT_ARCH_SPARC
#  endif
#elif defined(__xtensa__)
#  define SECCOMP_AUDIT_ARCH AUDIT_ARCH_XTENSA
#else
#  error "Platform does not support seccomp filter yet"
#endif

static struct sock_filter filter[] = {
	/* load the *current* architecture */
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
	    (offsetof(struct seccomp_data, arch))),
	/* ensure it's the same that we've been compiled on */
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
	    SECCOMP_AUDIT_ARCH, 1, 0),
	/* if not, kill the program */
	BPF_STMT(BPF_RET | BPF_K, SC_FAIL),

	/* load the syscall number */
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
	    (offsetof(struct seccomp_data, nr))),

#ifdef __NR_accept
	SC_ALLOW(accept),
#endif
#ifdef __NR_accept4
	SC_ALLOW(accept4),
#endif
#ifdef __NR_brk
	SC_ALLOW(brk),
#endif
#ifdef __NR_clock_gettime
	SC_ALLOW(clock_gettime),
#endif
#if defined(__x86_64__) && defined(__ILP32__) && defined(__X32_SYSCALL_BIT)
	SECCOMP_ALLOW(__NR_clock_gettime & ~__X32_SYSCALL_BIT),
#endif
#ifdef __NR_clock_gettime64
	SC_ALLOW(clock_gettime64),
#endif
#ifdef __NR_close
	SC_ALLOW(close),
#endif
#ifdef __NR_epoll_ctl
	SC_ALLOW(epoll_ctl),
#endif
#ifdef __NR_epoll_pwait
	SC_ALLOW(epoll_pwait),
#endif
#ifdef __NR_epoll_wait
	SC_ALLOW(epoll_wait),
#endif
#ifdef __NR_exit
	SC_ALLOW(exit),
#endif
#ifdef __NR_exit_group
	SC_ALLOW(exit_group),
#endif
#ifdef __NR_fcntl
	SC_ALLOW(fcntl),
#endif
#ifdef __NR_fcntl64
	SC_ALLOW(fcntl64),
#endif
#ifdef __NR_fstat
	SC_ALLOW(fstat),
#endif
#ifdef __NR_getdents64
	SC_ALLOW(getdents64),
#endif
#ifdef __NR_getpid
	SC_ALLOW(getpid),
#endif
#ifdef __NR_getrandom
	SC_ALLOW(getrandom),
#endif
#ifdef __NR_gettimeofday
	SC_ALLOW(gettimeofday),
#endif
#ifdef __NR_ioctl
	/* allow ioctl only on fd 1, glibc doing stuff? */
        SC_ALLOW_ARG(__NR_ioctl, 0, 1),
#endif
#ifdef __NR_lseek
	SC_ALLOW(lseek),
#endif
#ifdef __NR_madvise
	SC_ALLOW(madvise),
#endif
#ifdef __NR_mmap
	SC_ALLOW(mmap),
#endif
#ifdef __NR_mmap2
	SC_ALLOW(mmap2),
#endif
#ifdef __NR_munmap
	SC_ALLOW(munmap),
#endif
#ifdef __NR_newfstatat
	SC_ALLOW(newfstatat),
#endif
#ifdef __NR_oldfstat
	SC_ALLOW(oldfstat),
#endif
#ifdef __NR_openat
	SC_ALLOW(openat),
#endif
#ifdef __NR_prlimit64
	SC_ALLOW(prlimit64),
#endif
#ifdef __NR_read
	SC_ALLOW(read),
#endif
#ifdef __NR_recvmsg
	SC_ALLOW(recvmsg),
#endif
#ifdef __NR_redav
	SC_ALLOW(redav),
#endif
#ifdef __NR_rt_sigaction
	SC_ALLOW(rt_sigaction),
#endif
#ifdef __NR_rt_sigreturn
	SC_ALLOW(rt_sigreturn),
#endif
#ifdef __NR_sendmsg
	SC_ALLOW(sendmsg),
#endif
#ifdef __NR_statx
	SC_ALLOW(statx),
#endif
#ifdef __NR_write
	SC_ALLOW(write),
#endif
#ifdef __NR_writev
	SC_ALLOW(writev),
#endif

	/* disallow enything else */
	BPF_STMT(BPF_RET | BPF_K, SC_FAIL),
};

#ifdef SC_DEBUG

#include <signal.h>
#include <unistd.h>

static void
sandbox_seccomp_violation(int signum, siginfo_t *info, void *ctx)
{
	(void)signum;
	(void)ctx;

	fprintf(stderr, "%s: unexpected system call (arch:0x%x,syscall:%d @ %p)\n",
	    __func__, info->si_arch, info->si_syscall, info->si_call_addr);
	_exit(1);
}

static void
sandbox_seccomp_catch_sigsys(void)
{
	struct sigaction act;
	sigset_t mask;

	memset(&act, 0, sizeof(act));
	sigemptyset(&mask);
	sigaddset(&mask, SIGSYS);

	act.sa_sigaction = &sandbox_seccomp_violation;
	act.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSYS, &act, NULL) == -1)
		fatal("%s: sigaction(SIGSYS): %s",
		    __func__, strerror(errno));

	if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
		fatal("%s: sigprocmask(SIGSYS): %s\n",
		    __func__, strerror(errno));
}
#endif	/* SC_DEBUG */

void
sandbox_server_process(void)
{
	struct sock_fprog prog = {
		.len = (unsigned short) (sizeof(filter) / sizeof(filter[0])),
		.filter = filter,
	};

#ifdef SC_DEBUG
	sandbox_seccomp_catch_sigsys();
#endif

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1)
		fatal("%s: prctl(PR_SET_NO_NEW_PRIVS): %s",
		    __func__, strerror(errno));

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == -1)
		fatal("%s: prctl(PR_SET_SECCOMP): %s\n",
		    __func__, strerror(errno));
}

void
sandbox_executor_process(void)
{
	/* We cannot use seccomp for the executor process because we
	 * don't know what the child will do.  Also, our filter will
	 * be inherited so the child cannot set its own seccomp
	 * policy. */
	return;
}

void
sandbox_logger_process(void)
{
	/* To be honest, here we could use a seccomp policy to only
	 * allow writev(2) and memory allocations. */
	return;
}

#elif defined(__OpenBSD__)

#include <unistd.h>

void
sandbox_server_process(void)
{
	struct vhost *h;

	for (h = hosts; h->domain != NULL; ++h) {
		if (unveil(h->dir, "r") == -1)
			fatal("unveil %s for domain %s", h->dir, h->domain);
	}

	if (pledge("stdio recvfd rpath inet", NULL) == -1)
		fatal("pledge");
}

void
sandbox_executor_process(void)
{
	struct vhost	*vhost;

	for (vhost = hosts; vhost->domain != NULL; ++vhost) {
		/* r so we can chdir into the correct directory */
		if (unveil(vhost->dir, "rx") == -1)
			err(1, "unveil %s for domain %s",
			    vhost->dir, vhost->domain);
	}

	/* rpath to chdir into the correct directory */
	if (pledge("stdio rpath sendfd proc exec", NULL))
		err(1, "pledge");
}

void
sandbox_logger_process(void)
{
	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");
}

#else

#warning "No sandbox method known for this OS"

void
sandbox_server_process(void)
{
	return;
}

void
sandbox_executor_process(void)
{
	log_notice(NULL, "no sandbox method known for this OS");
}

void
sandbox_logger_process(void)
{
	return;
}

#endif
