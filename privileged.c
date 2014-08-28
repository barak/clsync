/*
    clsync - file tree sync utility based on inotify/kqueue
    
    Copyright (C) 2013-2014 Dmitry Yu Okunev <dyokunev@ut.mephi.ru> 0x8E30679C
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"			// ctx.h
#include "ctx.h"			// ctx_t
#include "error.h"			// debug()
#include "syscalls.h"			// read_inf()/write_inf()
#include "main.h"			// ncpus

#ifdef CAPABILITIES_SUPPORT
# include <pthread.h>			// pthread_create()
# include <sys/inotify.h>		// inotify_init()
# include <sys/types.h>			// fts_open()
# include <sys/stat.h>			// fts_open()
# include <fts.h>			// fts_open()
# include <errno.h>			// errno
# include <sys/capability.h>		// capset()
# include "malloc.h"			// strdup_protect()
# ifdef CGROUP_SUPPORT
#  include "cgroup.h"			// clsync_cgroup_deinit()
# endif
#endif

#include <unistd.h>			// execvp()


#ifdef UNSHARE_SUPPORT
# include <sched.h>			// unshare()
#endif

#ifndef HL_LOCKS
# ifdef HL_LOCK_TRIES_AUTO
#  undef HL_LOCK_TRIES_AUTO
# endif
#endif

#ifdef HL_LOCK_TRIES_AUTO
# include <time.h>			// time()
# include <math.h>			// fabs()
#endif

#include "privileged.h"

#ifdef SECCOMP_SUPPORT
# include <seccomp.h>			// __NR_*
# include <sys/prctl.h>			// prctl()
# include <linux/filter.h>		// struct sock_filter
# include <linux/seccomp.h>		// SECCOMP_RET_*

#define syscall_nr (offsetof(struct seccomp_data, nr))

/* Read: http://www.rawether.net/support/bpfhelp.htm */
# define SECCOMP_COPY_SYSCALL_TO_ACCUM				\
	BPF_STMT(BPF_LD+BPF_W+BPF_ABS, syscall_nr)

# define SECCOMP_ALLOW						\
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW)

# define SECCOMP_DENY						\
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_TRAP)

# define SECCOMP_ALLOW_ACCUM_SYSCALL(syscall)			\
	BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_##syscall, 0, 1),	\
	SECCOMP_ALLOW

# define FILTER_TABLE_NONPRIV						\
	SECCOMP_ALLOW_ACCUM_SYSCALL(futex),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(inotify_init1),			\
	SECCOMP_ALLOW_ACCUM_SYSCALL(alarm),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(stat),		/* unused */	\
	SECCOMP_ALLOW_ACCUM_SYSCALL(fstat),		/* unused */	\
	SECCOMP_ALLOW_ACCUM_SYSCALL(lstat),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(open),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(write),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(close),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(wait4),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(unlink),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(tgkill),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(clock_gettime),			\
	SECCOMP_ALLOW_ACCUM_SYSCALL(rt_sigreturn),			\
	SECCOMP_ALLOW_ACCUM_SYSCALL(brk),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(mmap),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(munmap),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(wait4),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(rmdir),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(exit_group),			\
	SECCOMP_ALLOW_ACCUM_SYSCALL(select),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(read),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(rt_sigprocmask),			\
	SECCOMP_ALLOW_ACCUM_SYSCALL(rt_sigaction),			\
	SECCOMP_ALLOW_ACCUM_SYSCALL(nanosleep),				\
	SECCOMP_ALLOW_ACCUM_SYSCALL(mprotect),				\

/* Syscalls allowed to non-privileged thread */
static struct sock_filter filter_table[] = {
	SECCOMP_COPY_SYSCALL_TO_ACCUM,
	FILTER_TABLE_NONPRIV
	SECCOMP_DENY,
};

int nonprivileged_seccomp_init() {
	static struct sock_fprog filter = {
		.len = (unsigned short)(sizeof(filter_table)/sizeof(filter_table[0])),
		.filter = filter_table,
	};

	SAFE (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0),			return -1);
	SAFE (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter),	return -1);

	return 0;
}
#endif

int (*privileged_fork_execvp)(const char *file, char *const argv[]);
int (*privileged_kill_child)(pid_t pid, int sig);

#ifdef CAPABILITIES_SUPPORT
pid_t		helper_pid;
pthread_t	privileged_thread;
pthread_mutex_t	*pthread_mutex_privileged_p;
pthread_mutex_t	*pthread_mutex_action_signal_p;
pthread_mutex_t	*pthread_mutex_action_entrance_p;
pthread_mutex_t	*pthread_mutex_runner_p;
pthread_cond_t	*pthread_cond_privileged_p;
pthread_cond_t	*pthread_cond_action_p;
pthread_cond_t	*pthread_cond_runner_p;

# ifdef READWRITE_SIGNALLING
int priv_read_fd;
int priv_write_fd;
int nonp_read_fd;
int nonp_write_fd;
# endif

enum privileged_action {
	PA_UNKNOWN = 0,

	PA_SETUP,

	PA_DIE,

	PA_FTS_OPEN,
	PA_FTS_READ,
	PA_FTS_CLOSE,

	PA_INOTIFY_INIT,
	PA_INOTIFY_INIT1,
	PA_INOTIFY_ADD_WATCH,
	PA_INOTIFY_RM_WATCH,

	PA_FORK_EXECVP,

	PA_KILL_CHILD,

	PA_CLSYNC_CGROUP_DEINIT,
};

struct pa_fts_open_arg {
	char _path_argv[MAXARGUMENTS+1][PATH_MAX];
	char *path_argv[MAXARGUMENTS+1];
	int options;
	int (*compar)(const FTSENT **, const FTSENT **);
};

struct pa_inotify_add_watch_arg {
	int fd;
	char pathname[PATH_MAX];
	uint32_t mask;
};

struct pa_inotify_rm_watch_arg {
	int fd;
	int wd;
};

struct pa_fork_execvp_arg {
	char file[PATH_MAX];
	char _argv[MAXARGUMENTS+1][BUFSIZ];
	char *argv[MAXARGUMENTS+1];
};

struct pa_kill_child_arg {
	pid_t pid;
	int   signal;
};

union pa_arg {
	struct pa_fts_open_arg		 fts_open;
	struct pa_inotify_add_watch_arg	 inotify_add_watch;
	struct pa_inotify_rm_watch_arg	 inotify_rm_watch;
	struct pa_fork_execvp_arg	 fork_execvp;
	struct pa_kill_child_arg	 kill_child;
	void				*void_v;
	ctx_t				*ctx_p;
	uint32_t			 uint32_v;
};

# ifdef HL_LOCKS
enum highload_lock_id {
	HLLOCK_HANDLER = 0,

	HLLOCK_MAX
};
typedef enum highlock_lock_id hllockid_t;

enum highlock_lock_state {
	HLLS_UNREADY	= 0x00,
	HLLS_READY	= 0x01,
	HLLS_FALLBACK	= 0x02,
	HLLS_SIGNAL	= 0x04,
	HLLS_GOTSIGNAL	= 0x08,
	HLLS_WORKING	= 0x10,
};
typedef enum highlock_lock_state hllock_state_t;

struct hl_lock {
	int			enabled;
	int			count_wait[HLLOCK_MAX];
	int			count_signal[HLLOCK_MAX];
	hllock_state_t		state[HLLOCK_MAX];
#  ifdef HL_LOCK_TRIES_AUTO
	unsigned long		tries[PC_MAX];
	unsigned long		count[PC_MAX];
	unsigned long		delay[PC_MAX];
	double			tries_step[PC_MAX];

#   define			tries_cur tries[callid]
#  else
	unsigned long		tries;
#   define			tries_cur tries
#  endif
};
# endif

struct pa_fts_read_ret {
	FTSENT		ftsent;
	char		fts_accpath[PATH_MAX];
	char		fts_path[PATH_MAX];
	char		fts_name[PATH_MAX];
};
union pa_ret {
	struct stat		stat;
	struct pa_fts_read_ret	fts_read;
};
struct cmd {
	volatile union pa_arg		 arg;
	volatile union pa_ret		 ret_buf;
	volatile enum privileged_action	 action;
	volatile void			*ret;
	volatile int			 _errno;
# ifdef HL_LOCKS
	volatile struct hl_lock		 hl_lock;
	unsigned long			 hl_lock_tries;
# endif
};
volatile struct cmd *cmd_p;

static inline void cmd_init(volatile struct cmd *cmd_p) {
# ifdef HL_LOCKS
	memset((void *)cmd_p, 0, sizeof(*cmd_p));
	cmd_p->hl_lock.enabled = 1;
#  ifdef HL_LOCK_TRIES_AUTO
	int i;
	i = 0;
	while (i < PC_MAX) {
		cmd_p->hl_lock.tries[i]		= HL_LOCK_TRIES_INITIAL;
		cmd_p->hl_lock.delay[i]		= ((unsigned long)~0)>>2;
		cmd_p->hl_lock.tries_step[i]	= HL_LOCK_AUTO_K;
		i++;
	}
#  else
	cmd_p->hl_lock.tries	= HL_LOCK_TRIES_INITIAL;
#  endif
# endif
	return;
}

struct pa_options {
	synchandler_args_t args[SHARGS_MAX];
	char *label;
	char *exithookfile;
	char *preexithookfile;
	char *permitted_hookfile[MAXPERMITTEDHOOKFILES+1];
	int   permitted_hookfiles;
};

int pthread_mutex_init_shared(pthread_mutex_t **mutex_p) {
	static pthread_mutex_t mutex_initial = PTHREAD_MUTEX_INITIALIZER;
	*mutex_p = shm_malloc(sizeof(**mutex_p));
	memcpy(*mutex_p, &mutex_initial, sizeof(mutex_initial));

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	return pthread_mutex_init(*mutex_p, &attr);
}

int pthread_mutex_destroy_shared(pthread_mutex_t *mutex_p) {
	int rc;
	rc = pthread_mutex_destroy(mutex_p);
	shm_free(mutex_p);
	return rc;
}

int pthread_cond_init_shared(pthread_cond_t **cond_p) {
	static pthread_cond_t cond_initial = PTHREAD_COND_INITIALIZER;
	*cond_p = shm_malloc(sizeof(**cond_p));
	memcpy(*cond_p, &cond_initial, sizeof(cond_initial));

	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	return pthread_cond_init(*cond_p, &attr);
}

int pthread_cond_destroy_shared(pthread_cond_t *cond_p) {
	int rc;
	rc = pthread_cond_destroy(cond_p);
	shm_free(cond_p);
	return rc;
}

FTS *(*_privileged_fts_open)		(
		char * const *path_argv,
		int options,
		int (*compar)(const FTSENT **, const FTSENT **)
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	);

FTSENT *(*_privileged_fts_read)		(
		FTS *ftsp
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	);

int (*_privileged_fts_close)		(
		FTS *ftsp
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	);

int (*_privileged_inotify_init)		();
int (*_privileged_inotify_init1)	(int flags);

int (*_privileged_inotify_add_watch)	(
		int fd,
		const char *pathname,
		uint32_t mask
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	);

int (*_privileged_inotify_rm_watch)	(
		int fd,
		int wd
	);

int (*_privileged_clsync_cgroup_deinit)	(ctx_t *ctx_p);


int cap_enable(__u32 caps) {
	debug(1, "Enabling Linux capabilities 0x%x", caps);
	struct __user_cap_header_struct	cap_hdr = {0};
	struct __user_cap_data_struct	cap_dat = {0};

	cap_hdr.version = _LINUX_CAPABILITY_VERSION;
	if (capget(&cap_hdr, &cap_dat) < 0) {
		error("Cannot get capabilites with capget()");
		return errno;
	}

	debug(3, "old: cap.eff == 0x%04x; new: cap.eff == 0x%04x", cap_dat.effective, cap_dat.effective|caps);
	cap_dat.effective |= caps;

	if (capset(&cap_hdr, &cap_dat) < 0) {
		error("Cannot set capabilities with capset().");
		return errno;
	}

	return 0;
}

int cap_drop(ctx_t *ctx_p, __u32 caps) {
	debug(1, "Dropping all Linux capabilites but 0x%x", caps);

	struct __user_cap_header_struct	cap_hdr = {0};
	struct __user_cap_data_struct	cap_dat = {0};

	cap_hdr.version = _LINUX_CAPABILITY_VERSION;
	if (capget(&cap_hdr, &cap_dat) < 0) {
		error_or_debug((ctx_p->flags[CAP_PRESERVE] != CAP_PRESERVE_TRY) ? -1 : 3, "Cannot get capabilites with capget()");
		return errno;
	}
	debug(3, "old: cap.eff == 0x%04x; cap.prm == 0x%04x; cap.inh == 0x%04x.",
		cap_dat.effective, cap_dat.permitted, cap_dat.inheritable);

	switch (ctx_p->flags[CAPS_INHERIT]) {
		case CI_PERMITTED:
			cap_dat.inheritable = cap_dat.permitted;
			break;
		case CI_DONTTOUCH:
			break;
		case CI_CLSYNC:
			cap_dat.inheritable = caps;
			break;
		case CI_EMPTY:
			cap_dat.inheritable = 0;
			break;
	}
	cap_dat.effective  = caps;
	cap_dat.permitted  = caps;

	debug(3, "new: cap.eff == 0x%04x; cap.prm == 0x%04x; cap.inh == 0x%04x.",
		cap_dat.effective, cap_dat.permitted, cap_dat.inheritable);

	if (capset(&cap_hdr, &cap_dat) < 0) {
		error_or_debug((ctx_p->flags[CAP_PRESERVE] != CAP_PRESERVE_TRY) ? -1 : 3, "Cannot set capabilities with capset().");
		return errno;
	}

	return 0;
}

#endif
int __privileged_kill_child_itself(pid_t child_pid, int signal) {
	// Checking if it's a child
	if (waitpid(child_pid, NULL, WNOHANG)>=0) {
		debug(3, "Sending signal %u to child process with pid %u.",
			signal, child_pid);
		if (kill(child_pid, signal)) {
			error("Got error while kill(%u, %u)", child_pid, signal);
			return errno;
		}

		sleep(1);	// TODO: replace this sleep() with something to do not sleep if process already died
	} else
		return ENOENT;

	return 0;
}
#ifdef CAPABILITIES_SUPPORT

int pa_strcmp(const char *s1, const char *s2, int isexpanded) {
	if (isexpanded)
		return strcmp(s1, s2);

	{
		const char *s1_start = NULL;
		const char *s2_start = NULL;
		while (1) {
			while (1) {
				if (!*s1 || !*s2) {
					if (!*s1 && s1_start != NULL)
						return 0;
					return *s1 != *s2;
				}

				if (*s1 == '%') {
					s1++;
					while (*s1 && *s1 != '%') s1++;
					s1++;
					s1_start = s1;
					s2_start = s2;
					continue;
				}

				if (*s1 != *s2)
					break;

				s1++;
				s2++;
			}

			if (s2_start == NULL)
				break;

			s2_start++;
			s1 = s1_start;
			s2 = s2_start;
		}

		return *s1 != *s2;
	}
}

int privileged_execvp_check_arguments(struct pa_options *opts, const char *u_file, char *const *u_argv) {
	int a_i;
	size_t u_argc;
	synchandler_args_t *args = opts->args;
	
	debug(9, "");

	// Counting the number of arguments
	u_argc = 0;
	while (u_argv[u_argc] != NULL) u_argc++;

	a_i = 0;
	do {
		int i;
		int    argc;
		char **argv;
		char  *isexpanded;

		argc       = args[a_i].c;
		argv       = args[a_i].v;
		isexpanded = args[a_i].isexpanded;

		// Checking the number of arguments
		if (argc != u_argc)
			continue;

		critical_on (!argc);

		// Checking the execution file
		if (pa_strcmp(argv[0], u_file, isexpanded[0]))
			continue;

		// Checking arguments
		i = 1;
		while (i < argc) {
			if (pa_strcmp(argv[i], u_argv[i], isexpanded[i]))
				break;
			i++;
		}

		// All arguments right?
		if (i == argc)
			break;

		// No? Ok the next "shargs".
	} while (++a_i < SHARGS_MAX);

	if (a_i < SHARGS_MAX)
		return 0;

	if (u_argc == 2) {
		int i;

		if ((opts->exithookfile != NULL) || (opts->preexithookfile != NULL)) {
			if (!strcmp(opts->label, u_argv[1])) {
				if (opts->exithookfile != NULL)
					if (!strcmp(opts->exithookfile,    u_file))
						return 0;
				if (opts->preexithookfile != NULL)
					if (!strcmp(opts->preexithookfile, u_file))
						return 0;
			}
		}

		i = 0;
		while (i < opts->permitted_hookfiles) {
			if (!strcmp(opts->permitted_hookfile[i], u_file))
				return 0;
			i++;
		}
	}

	critical("Arguments are wrong. This should happend only on hacking attack.");
	return EPERM;
}

int pa_setup(struct pa_options *opts, ctx_t *ctx_p, uid_t *exec_uid_p, gid_t *exec_gid_p) {
	synchandler_args_t *args = opts->args;
	int a_i;

	a_i = 0;
	do {
		int i, argc_s;
		char **argv_s, **argv_d, *isex_s, *isex_d;

		argc_s = ctx_p->synchandler_args[a_i].c;
		argv_s = ctx_p->synchandler_args[a_i].v;
		isex_s = ctx_p->synchandler_args[a_i].isexpanded;
		argv_d = args[a_i].v;
		isex_d = args[a_i].isexpanded;

		if (argc_s >= MAXARGUMENTS)
			critical("Too many arguments");

		if (argc_s < 1)
			continue;

		argv_d[0] = strdup_protect(ctx_p->handlerfpath, PROT_READ);

		i = 0;
		while (i < argc_s) {
			argv_d[i+1] = strdup_protect(argv_s[i], PROT_READ);
			isex_d[i+1] = isex_s[i];
			i++;
		}
		i++;
		argv_d[i] = NULL;
		args[a_i].c = i;

		a_i++;
	} while (++a_i < SHARGS_MAX);

	*exec_uid_p = ctx_p->synchandler_uid;
	*exec_gid_p = ctx_p->synchandler_gid;

	opts->label = strdup_protect(ctx_p->label, PROT_READ);
	if (ctx_p->exithookfile != NULL)
		opts->exithookfile = strdup_protect(ctx_p->exithookfile, PROT_READ);
	if (ctx_p->preexithookfile != NULL)
		opts->preexithookfile = strdup_protect(ctx_p->preexithookfile, PROT_READ);

	{
		int i = 0;

		while (i < ctx_p->permitted_hookfiles) {
			opts->permitted_hookfile[i] = strdup_protect(ctx_p->permitted_hookfile[i], PROT_READ);
			i++;
		}
		opts->permitted_hookfile[i] = NULL;
		opts->permitted_hookfiles   = i;
	}
	return 0;
}

int pa_unsetup(struct pa_options *opts) {
	free(opts->exithookfile);
	free(opts->preexithookfile);
	free(opts->label);

	{
		int a_i = 0;
		do {
			int i;

			i = 0;
			while (i < opts->args[a_i].c) {
				free(opts->args[a_i].v[i]);
				i++;
			}
		} while(++a_i < SHARGS_MAX);
	}

	{
		int i = 0;
		while (i < opts->permitted_hookfiles) {
			free(opts->permitted_hookfile[i]);
			i++;
		}
	}

	return 0;
}

# ifdef HL_LOCKS

static inline int hl_isanswered(int lockid) {
	return cmd_p->hl_lock.count_wait[lockid] == cmd_p->hl_lock.count_signal[lockid]+1;
}

static inline int hl_isready(int lockid) {
	return cmd_p->hl_lock.count_wait[lockid] == cmd_p->hl_lock.count_signal[lockid];
}

static inline void hl_setstate(int lockid, hllock_state_t stateid) {
	cmd_p->hl_lock.state[lockid] = stateid;
}

int hl_setstate_ifstate(int lockid, hllock_state_t stateid_new, hllock_state_t stateid_old_mask) {
	static int local_lock = 0;

	if (local_lock)
		return 0;

	g_atomic_int_inc(&local_lock);
	if (local_lock != 1) {
		g_atomic_int_dec_and_test(&local_lock);
		return 0;
	}

	if (!(cmd_p->hl_lock.state[lockid]&stateid_old_mask)) {
		g_atomic_int_dec_and_test(&local_lock);
		return 0;
	}

	cmd_p->hl_lock.state[lockid] = stateid_new;

	g_atomic_int_dec_and_test(&local_lock);
	return 1;
}

static inline int hl_wait(
		int lockid
#  ifdef HL_LOCK_TRIES_AUTO
		, unsigned long hl_lock_tries
#  endif
) {
	volatile long try = 0;

	debug(15, "");

	while (cmd_p->hl_lock.state[lockid] == HLLS_GOTSIGNAL);
	while (!hl_isready(lockid));
	hl_setstate(lockid, HLLS_READY);
	cmd_p->hl_lock.count_wait[lockid]++;

	while (try++ < hl_lock_tries)
		if (cmd_p->hl_lock.state[lockid] == HLLS_SIGNAL) {
			hl_setstate(lockid, HLLS_GOTSIGNAL);
			debug(15, "got signal");
			return 1;
		}

	while (!hl_setstate_ifstate(lockid, HLLS_FALLBACK, HLLS_READY|HLLS_SIGNAL));
	debug(14, "fallback: cmd_p->hl_lock.count_wait[%u] == %u; cmd_p->hl_lock.count_signal[%u] = %u", lockid, cmd_p->hl_lock.count_wait[lockid], lockid, cmd_p->hl_lock.count_signal[lockid]);
	return 0;
}

static inline int hl_signal(int lockid) {
	debug(15, "%u", lockid);
	cmd_p->hl_lock.count_signal[lockid]++;

	if (hl_setstate_ifstate(lockid, HLLS_SIGNAL, HLLS_READY)) {
		while (cmd_p->hl_lock.state[lockid] != HLLS_GOTSIGNAL)
			if (cmd_p->hl_lock.state[lockid] == HLLS_FALLBACK) {
				debug(15, "fallback");
				return 0;
			}
		debug(15, "the signal is sent");
		hl_setstate(lockid, HLLS_WORKING);
		return 1;
	}

	debug(15, "not ready");
	return 0;
}

void hl_shutdown(int lockid) {
	debug(1, "");

#  ifdef PARANOID
	critical_on (HLLOCK_MAX != 1);	// TODO: do this on compile time (not on running time)
#   ifdef HL_LOCK_TRIES_AUTO
	memset((void *)cmd_p->hl_lock.tries, 0, sizeof(cmd_p->hl_lock.tries));
#   else
	cmd_p->hl_lock.tries = 0;
#   endif
#  endif
	cmd_p->hl_lock.state[lockid]	 = HLLS_FALLBACK;
	cmd_p->hl_lock.enabled 	 = 0;

	return;
}

# endif

static inline int parent_isalive() {
	int rc;
	debug(12, "parent_pid == %u", parent_pid);

	if ((rc=kill(parent_pid, 0))) {
		debug(1, "kill(%u, 0) => %i", parent_pid, rc);
		return 0;
	}

	return 1;
}

static int helper_isalive_cache;
static inline int helper_isalive() {
	int rc;
	debug(12, "helper_pid == %u", helper_pid);

	if ((rc=waitpid(helper_pid, NULL, WNOHANG))>=0)
		return helper_isalive_cache=1;

	debug(1, "waitpid(%u, NULL, WNOHANG) => %i", helper_pid, rc);

	return helper_isalive_cache=0;
}

int privileged_check() {
	critical_on(!helper_isalive());
	return 0;
}

int privileged_handler(ctx_t *ctx_p)
{
# ifdef READWRITE_SIGNALLING
	char buf[1] = {0};
# endif
	int setup = 0;
	uid_t exec_uid = 65535;
	gid_t exec_gid = 65535;
	
	struct pa_options *opts;
	int use_args_check = 0;
	int helper_isrunning = 1;

	opts  = calloc_align(1, sizeof(*opts));

	sigset_t sigset;
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGALRM);
	sigaddset(&sigset, SIGHUP);
	sigaddset(&sigset, SIGQUIT);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGCHLD);
	critical_on(pthread_sigmask(SIG_UNBLOCK, &sigset, NULL));

# ifndef __linux__
	critical_on(!parent_isalive());
# endif
	cap_drop(ctx_p, ctx_p->caps);

	debug(2, "Syncing with the runner");
	pthread_mutex_lock(pthread_mutex_privileged_p);

	// Waiting for runner to get ready for signal
	pthread_mutex_lock(pthread_mutex_runner_p);
	pthread_mutex_unlock(pthread_mutex_runner_p);

	// Sending the signal that we're ready
	pthread_cond_signal(pthread_cond_runner_p);

	// The loop
	debug(2, "Running the loop");
	while (helper_isrunning) {
		errno = 0;

		// Waiting for command
		debug(10, "Waiting for command");
# ifdef HL_LOCKS
		if (!cmd_p->hl_lock.enabled || !hl_wait(
			HLLOCK_HANDLER
#  ifdef HL_LOCK_TRIES_AUTO
			, cmd_p->hl_lock_tries
#  endif
		)) {
			critical_on(!parent_isalive());
# endif
# ifdef READWRITE_SIGNALLING
			read_inf(priv_read_fd, buf, 1);
# else
			pthread_cond_wait(pthread_cond_privileged_p, pthread_mutex_privileged_p);
# endif
# ifdef HL_LOCKS
			if (cmd_p->hl_lock.enabled)
				hl_setstate(HLLOCK_HANDLER, HLLS_WORKING);
		}
# endif

		debug(10, "Got command %u", cmd_p->action);

		if (!setup && cmd_p->action != PA_SETUP)
			critical("A try to use commands before PA_SETUP");

		switch (cmd_p->action) {
			case PA_SETUP: {
				debug(20, "PA_SETUP");
				if (setup)
					critical("Double privileged_handler setuping. It can be if somebody is trying to hack the clsync.");

				critical_on(pa_setup(opts, cmd_p->arg.ctx_p, &exec_uid, &exec_gid));
				mprotect(opts, sizeof(*opts), PROT_READ);
				use_args_check = cmd_p->arg.ctx_p->flags[CHECK_EXECVP_ARGS];
				setup++;
				critical_on(errno);
				break;
			}
			case PA_DIE:
				debug(20, "PA_DIE");
				helper_isrunning = 0;
				break;
			case PA_FTS_OPEN: {
				volatile struct pa_fts_open_arg *arg_p = &cmd_p->arg.fts_open;
				debug(20, "PA_FTS_OPEN");
				if (arg_p->compar != NULL)
					critical("\"arg_p->compar != NULL\" (arg_p->compar == %p) is forbidden because may be used to run an arbitrary code in the privileged thread.", arg_p->compar);

				cmd_p->ret = fts_open((void *)arg_p->path_argv, arg_p->options, NULL);
				debug(21, "/PA_FTS_OPEN => %p", cmd_p->ret);
				break;
			}
			case PA_FTS_READ: {
				debug(20, "PA_FTS_READ(%p)", cmd_p->arg.void_v);
				FTSENT *ret = fts_read(cmd_p->arg.void_v);
				if (ret == NULL) {
					cmd_p->ret = NULL;
					debug(10, "cmd_p->ret == NULL");
					break;
				}

				{
					struct pa_fts_read_ret *ret_buf = (void *)&cmd_p->ret_buf.fts_read;
					memcpy(&ret_buf->ftsent, ret, sizeof(ret_buf->ftsent));
					cmd_p->ret = &ret_buf->ftsent;
					debug(25, "fts_path == <%s>", ret_buf->fts_path);
					strncpy(ret_buf->fts_accpath, ret->fts_accpath, sizeof(ret_buf->fts_accpath));
					strncpy(ret_buf->fts_path,    ret->fts_path,    sizeof(ret_buf->fts_path));
					ret_buf->ftsent.fts_accpath = ret_buf->fts_accpath;
					ret_buf->ftsent.fts_path    = ret_buf->fts_path;
				}
				break;
			}
			case PA_FTS_CLOSE:
				debug(20, "PA_FTS_CLOSE");
				cmd_p->ret = (void *)(long)fts_close(cmd_p->arg.void_v);
				break;
			case PA_INOTIFY_INIT:
				debug(20, "PA_INOTIFY_INIT");
				cmd_p->ret = (void *)(long)inotify_init();
				break;
# ifndef INOTIFY_OLD
			case PA_INOTIFY_INIT1:
				debug(20, "PA_INOTIFY_INIT1");
				cmd_p->ret = (void *)(long)inotify_init1((long)cmd_p->arg.ctx_p);
				break;
# endif
			case PA_INOTIFY_ADD_WATCH: {
				struct pa_inotify_add_watch_arg *arg_p = (void *)&cmd_p->arg.inotify_add_watch;
				debug(20, "PA_INOTIFY_ADD_WATCH(%u, <%s>, 0x%o)", arg_p->fd, arg_p->pathname, arg_p->mask);
				cmd_p->ret = (void *)(long)inotify_add_watch(arg_p->fd, arg_p->pathname, arg_p->mask);
				break;
			}
			case PA_INOTIFY_RM_WATCH: {
				debug(20, "PA_INOTIFY_RM_WATCH");
				struct pa_inotify_rm_watch_arg *arg_p = (void *)&cmd_p->arg.inotify_rm_watch;
				cmd_p->ret = (void *)(long)inotify_rm_watch(arg_p->fd, arg_p->wd);
				break;
			}
			case PA_FORK_EXECVP: {
				debug(20, "PA_FORK_EXECVP");
				struct pa_fork_execvp_arg *arg_p = (void *)&cmd_p->arg.fork_execvp;
				if (use_args_check)
					privileged_execvp_check_arguments(opts, arg_p->file, arg_p->argv);
				pid_t pid = fork();
				switch (pid) {
					case -1: 
						error("Cannot fork().");
						break;
					case  0:
						debug(4, "setgid(%u) == %i", exec_gid, setgid(exec_gid));
						debug(4, "setuid(%u) == %i", exec_uid, setuid(exec_uid));
						debug(3, "execvp(\"%s\", cmd_p->arg.argv)", arg_p->file);
						exit(execvp(arg_p->file, arg_p->argv));
				}
				cmd_p->ret = (void *)(long)pid;
				break;
			}
			case PA_KILL_CHILD: {
				debug(20, "PA_KILL_CHILD");
				struct pa_kill_child_arg *arg_p = (void *)&cmd_p->arg.kill_child;
				cmd_p->ret = (void *)(long)__privileged_kill_child_itself(arg_p->pid, arg_p->signal);
				break;
			}
# ifdef CGROUP_SUPPORT
			case PA_CLSYNC_CGROUP_DEINIT: {
				debug(20, "PA_CLSYNC_CGROUP_DEINIT");
				/*
				 * That is strange, but setuid() doesn't work
				 * without fork() in case of enabled seccomp
				 * filter. So sorry for this hacky thing.
				 *
				 * TODO: fix that.
				 */
				int status;
				pid_t pid = fork();
				switch (pid) {
					case -1: 
						error("Cannot fork().");
						break;
					case  0:
						debug(4, "setgid(0) == %i", setgid(0));
						debug(4, "setuid(0) == %i", setuid(0));
						exit(clsync_cgroup_deinit(cmd_p->arg.void_v));
				}

				if (waitpid(pid, &status, 0) != pid) {
					switch (errno) {
						case ECHILD:
							debug(2, "Child %u has already died.", pid);
							break;
						default:
							error("Cannot waitid().");
							cmd_p->_errno = errno;
							cmd_p->ret    = (void *)(long)errno;
					}
				}
#ifdef VERYPARANOID
				pthread_sigmask(SIG_SETMASK, &sigset_old, NULL);
#endif
				// Return
				int exitcode = WEXITSTATUS(status);
				debug(3, "execution completed with exitcode %i", exitcode);

				cmd_p->_errno = exitcode;
				cmd_p->ret    = (void *)(long)exitcode;

				break;
			}
# endif
			default:
				critical("Unknown command type \"%u\". It's a buffer overflow (which means a security problem) or just an internal error.");
		}

		cmd_p->_errno = errno;
		debug(10, "Result: %p, errno: %u. Sending the signal to non-privileged thread/process.", cmd_p->ret, cmd_p->_errno);
# ifdef HL_LOCKS
		if (!cmd_p->hl_lock.enabled) {
# endif
# ifndef __linux__
			critical_on(!parent_isalive());
# endif
# ifdef READWRITE_SIGNALLING
			write_inf(nonp_write_fd, buf, 1);
# else
			pthread_mutex_lock(pthread_mutex_action_signal_p);
			pthread_mutex_unlock(pthread_mutex_action_signal_p);
			pthread_cond_signal(pthread_cond_action_p);
# endif
# ifdef HL_LOCKS
		}
# endif
	}

	pa_unsetup(opts);
# ifdef HL_LOCKS
	hl_shutdown(HLLOCK_HANDLER);
# endif
	pthread_mutex_unlock(pthread_mutex_privileged_p);
	debug(2, "Finished");
	return 0;
}

static inline int privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
		int callid,
# endif
		enum privileged_action action,
		void **ret_p
	)
{
	int rc = 0;

# ifdef READWRITE_SIGNALLING
	char buf[1] = {0};
# endif
# ifdef HL_LOCK_TRIES_AUTO
	clock_t start_ticks;

	int isadjusting;
# endif
# ifdef HL_LOCKS
	debug(10, "(%u, %p): %i", action, ret_p, cmd_p->hl_lock.enabled);
# else
	debug(10, "(%u, %p)",     action, ret_p);
# endif

	pthread_mutex_lock(pthread_mutex_action_entrance_p);
# ifndef READWRITE_SIGNALLING
	debug(10, "Waiting the privileged thread/process to get prepared for signal");
#  ifdef HL_LOCKS
	if (cmd_p->hl_lock.enabled) {
		while (!hl_isanswered(HLLOCK_HANDLER))
			if (!helper_isalive_cache) {
				debug(1, "The privileged thread/process is dead (#0). Ignoring the command.");
				rc = ENOENT;
				goto privileged_action_end;
			}
	} else {
#  endif
		critical_on(!helper_isalive_cache);
		pthread_mutex_lock(pthread_mutex_privileged_p);
		pthread_mutex_unlock(pthread_mutex_privileged_p);
#  ifdef HL_LOCKS
	}
#  endif
# endif
	if (!helper_isalive_cache) {
		debug(1, "The privileged thread/process is dead (#1). Ignoring the command.");
		rc = ENOENT;
		goto privileged_action_end;
	}

	cmd_p->action = action;
	debug(10, "Sending information (action == %i) to the privileged thread/process", action);
# ifdef HL_LOCK_TRIES_AUTO
	cmd_p->hl_lock_tries = cmd_p->hl_lock.tries[callid];

	if ((isadjusting = cmd_p->hl_lock.enabled)) {
		isadjusting = cmd_p->hl_lock.tries[callid];
		if (isadjusting) {
			isadjusting = ((double)fabs(cmd_p->hl_lock.tries_step[callid]-1) > (double)HL_LOCK_AUTO_K_FINISH);
			if (isadjusting) {
				isadjusting = !((++cmd_p->hl_lock.count[callid]) << (sizeof(cmd_p->hl_lock.count[callid])*CHAR_BIT - HL_LOCK_AUTO_INTERVAL));
				debug(11, "isadjusting == %u; cmd_p->hl_lock.tries_step[%i] == %lf; cmd_p->hl_lock.count[%i] == %lu", isadjusting, callid, cmd_p->hl_lock.tries_step[callid], callid, cmd_p->hl_lock.count[callid]);
				if (isadjusting)
					start_ticks = clock();
			}
		}
	}

# endif
# ifdef HL_LOCKS
	if (!cmd_p->hl_lock.enabled || !hl_signal(HLLOCK_HANDLER)) {
# endif
		critical_on(!helper_isalive_cache);
# ifdef READWRITE_SIGNALLING
		write_inf(priv_write_fd, buf, 1);
# else
#  ifdef HL_LOCKS
		if (cmd_p->hl_lock.enabled) {
			debug(10, "Waiting the privileged thread/process to get prepared for signal (by fallback)");
			pthread_mutex_lock(pthread_mutex_privileged_p);
			pthread_mutex_unlock(pthread_mutex_privileged_p);
		} else
#  endif
		pthread_mutex_lock(pthread_mutex_action_signal_p);
		debug(10, "pthread_cond_signal(&pthread_cond_privileged)");
		pthread_cond_signal(pthread_cond_privileged_p);
# endif
# ifdef HL_LOCKS
	}
# endif

	if (action == PA_DIE)
		goto privileged_action_end;
	debug(10, "Waiting for the answer");

# ifdef HL_LOCKS
	if (cmd_p->hl_lock.enabled) {
		while (!hl_isanswered(HLLOCK_HANDLER))
			if (!helper_isalive_cache) {
				debug(1, "The privileged thread/process is dead (#2). Ignoring the command.");
				rc = ENOENT;
				goto privileged_action_end;
			}

#  ifdef HL_LOCK_TRIES_AUTO
		if (isadjusting) {
			unsigned long delay = (long)clock() - (long)start_ticks;
			long diff  = delay - cmd_p->hl_lock.delay[callid];

			debug(13, "diff == %li; cmd_p->hl_lock.delay[%i] == %lu; delay == %lu; delay*HL_LOCK_AUTO_THREADHOLD == %lu", diff, callid, cmd_p->hl_lock.delay[callid], delay, delay*HL_LOCK_AUTO_THREADHOLD)

			if (diff && ((unsigned long)labs(diff) > (unsigned long)delay*HL_LOCK_AUTO_THREADHOLD)) {

				if (diff > 0)
					cmd_p->hl_lock.tries_step[callid] = 1/((cmd_p->hl_lock.tries_step[callid]-1)/HL_LOCK_AUTO_DECELERATION+1);

				cmd_p->hl_lock.delay[callid]  = delay;

				debug(12, "diff == %li; cmd_p->hl_lock.tries_step[%i] == %lf; cmd_p->hl_lock.delay[%i] == %lu", diff, callid, cmd_p->hl_lock.tries_step[callid], callid, cmd_p->hl_lock.delay[callid]);
			}
			cmd_p->hl_lock.tries[callid] *= cmd_p->hl_lock.tries_step[callid];

			if (cmd_p->hl_lock.tries[callid] > HL_LOCK_AUTO_LIMIT_HIGH)
				cmd_p->hl_lock.tries[callid] = HL_LOCK_AUTO_LIMIT_HIGH;

			debug(14, "cmd_p->hl_lock.tries[%i] == %lu", callid, cmd_p->hl_lock.tries[callid]);
		}
#  endif
	} else {
# endif
		critical_on(!helper_isalive_cache);
# ifdef READWRITE_SIGNALLING
		read_inf(nonp_read_fd, buf, 1);
# else
		pthread_cond_wait(pthread_cond_action_p, pthread_mutex_action_signal_p);
# endif
# ifdef HL_LOCKS
	}
# endif

	if (ret_p != NULL)
		*ret_p = (void *)cmd_p->ret;
	errno = cmd_p->_errno;

privileged_action_end:
	debug(10, "Unlocking pthread_mutex_action_*");
# ifdef HL_LOCKS
#  ifndef READWRITE_SIGNALLING
	if (!cmd_p->hl_lock.enabled)
#  endif
# endif
		pthread_mutex_unlock(pthread_mutex_action_signal_p);
	pthread_mutex_unlock(pthread_mutex_action_entrance_p);

	return rc;
}

FTS *__privileged_fts_open(
		char *const *path_argv,
		int options,
		int (*compar)(const FTSENT **, const FTSENT **)
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	)
{
	void *ret = NULL;
	int i;

	i = 0;
	while (path_argv[i] != NULL) {
		cmd_p->arg.fts_open.path_argv[i] = (void *)cmd_p->arg.fts_open._path_argv[i];
		debug(25, "path_argv[%i] == <%s> (%p) -> %p", i, path_argv[i], path_argv[i], cmd_p->arg.fts_open.path_argv[i]);
		strncpy(cmd_p->arg.fts_open.path_argv[i], path_argv[i], sizeof(cmd_p->arg.fts_open._path_argv[i]));
		i++;
		critical_on(i >= MAXARGUMENTS);
	}
	cmd_p->arg.fts_open.path_argv[i]	= NULL;
	cmd_p->arg.fts_open.options		= options;
	cmd_p->arg.fts_open.compar		= compar;

	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			callid,
# endif
			PA_FTS_OPEN,
			&ret
		);

	return ret;
}

FTSENT *__privileged_fts_read(
		FTS *ftsp
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	)
{
	void *ret = NULL;
	cmd_p->arg.void_v = ftsp;
	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			callid,
# endif
			PA_FTS_READ,
			&ret
		);
	return ret;
}

int __privileged_fts_close(
		FTS *ftsp
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	)
{
	void *ret = (void *)(long)-1;
	cmd_p->arg.void_v = ftsp;
	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			callid,
# endif
			PA_FTS_CLOSE,
			&ret
		);
	return (long)ret;
}

int __privileged_inotify_init() {
	void *ret = (void *)(long)-1;
	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_INOTIFY_INIT,
			&ret
		);
	return (long)ret;
}

int __privileged_inotify_init1(int flags) {
	void *ret = (void *)(long)-1;
	cmd_p->arg.uint32_v = flags;
	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_INOTIFY_INIT1,
			&ret
		);
	return (long)ret;
}

int __privileged_inotify_add_watch(
		int fd,
		const char *pathname,
		uint32_t mask
# ifdef HL_LOCK_TRIES_AUTO
		, int callid
# endif
	)
{
	debug(25, "(%i, <%s>, o%o, ?)", fd, pathname, mask);
	void *ret = (void *)(long)-1;

	strncpy((void *)cmd_p->arg.inotify_add_watch.pathname, pathname, sizeof(cmd_p->arg.inotify_add_watch.pathname));
	cmd_p->arg.inotify_add_watch.fd		= fd;
	cmd_p->arg.inotify_add_watch.mask	= mask;

	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			callid,
# endif
			PA_INOTIFY_ADD_WATCH,
			&ret
		);

	return (long)ret;
}

int __privileged_inotify_rm_watch(
		int fd,
		int wd
	)
{
	void *ret = (void *)(long)-1;

	cmd_p->arg.inotify_rm_watch.fd	= fd;
	cmd_p->arg.inotify_rm_watch.wd	= wd;

	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_INOTIFY_RM_WATCH,
			&ret
		);

	return (long)ret;
}

# ifdef CGROUP_SUPPORT
int __privileged_clsync_cgroup_deinit(ctx_t *ctx_p)
{
	void *ret = (void *)(long)-1;

	cmd_p->arg.ctx_p = ctx_p;

	privileged_action(
#  ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
#  endif
			PA_CLSYNC_CGROUP_DEINIT,
			&ret
		);

	return (long)ret;
}
# endif

int __privileged_fork_setuid_execvp(
		const char *file,
		char *const argv[]
	)
{
	int i;
	void *ret = (void *)(long)-1;

	strncpy((void *)cmd_p->arg.fork_execvp.file, file, sizeof(cmd_p->arg.fork_execvp.file));

	i=0;
	while (argv[i] != NULL) {
		cmd_p->arg.fork_execvp.argv[i] = (void *)cmd_p->arg.fork_execvp._argv[i];
		strncpy(cmd_p->arg.fork_execvp.argv[i], argv[i], sizeof(cmd_p->arg.fork_execvp._argv[i]));
		i++;
		critical_on(i >= MAXARGUMENTS);
	}
	cmd_p->arg.fork_execvp.argv[i] = NULL;

	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_FORK_EXECVP,
			&ret
		);

	return (long)ret;
}

int __privileged_kill_child_wrapper(pid_t pid, int signal)
{
	void *ret = (void *)(long)-1;

	cmd_p->arg.kill_child.pid    = pid;
	cmd_p->arg.kill_child.signal = signal;

	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_KILL_CHILD,
			&ret);

	return (long)ret;
}

#endif

uid_t __privileged_fork_execvp_uid;
gid_t __privileged_fork_execvp_gid;
int __privileged_fork_execvp(const char *file, char *const argv[])
{
	debug(4, "");
	pid_t pid = fork();
	switch (pid) {
		case -1: 
			error("Cannot fork().");
			return -1;
		case  0:
			debug(4, "setgid(%u) == %i", __privileged_fork_execvp_gid, setgid(__privileged_fork_execvp_gid));
			debug(4, "setuid(%u) == %i", __privileged_fork_execvp_uid, setuid(__privileged_fork_execvp_uid));
			exit(execvp(file, argv));
	}

	return pid;
}

int privileged_init(ctx_t *ctx_p)
{
#ifdef READWRITE_SIGNALLING
	int pipefds[2];
#endif

#ifdef CAPABILITIES_SUPPORT
# ifdef HL_LOCKS
	if (ncpus == 1)
		hl_shutdown(HLLOCK_HANDLER);
# endif

	if (!ctx_p->flags[PROCESSSPLITTING]) {
#endif

		_privileged_fork_execvp		= __privileged_fork_execvp;

		__privileged_fork_execvp_uid	= ctx_p->synchandler_uid;
		__privileged_fork_execvp_gid	= ctx_p->synchandler_gid;

		_privileged_kill_child		= __privileged_kill_child_itself;

#ifdef CAPABILITIES_SUPPORT
		_privileged_fts_open		= (typeof(_privileged_fts_open))		fts_open;
		_privileged_fts_read		= (typeof(_privileged_fts_read))		fts_read;
		_privileged_fts_close		= (typeof(_privileged_fts_close))		fts_close;
		_privileged_inotify_init	= (typeof(_privileged_inotify_init))		inotify_init;
		_privileged_inotify_init1	= (typeof(_privileged_inotify_init1))		inotify_init1;
		_privileged_inotify_add_watch	= (typeof(_privileged_inotify_add_watch))	inotify_add_watch;
		_privileged_inotify_rm_watch	= (typeof(_privileged_inotify_rm_watch))	inotify_rm_watch;
# ifdef CGROUP_SUPPORT
		_privileged_clsync_cgroup_deinit= (typeof(_privileged_clsync_cgroup_deinit))	clsync_cgroup_deinit;
# endif

		cap_drop(ctx_p, ctx_p->caps);
#endif

		return 0;

#ifdef CAPABILITIES_SUPPORT
	}

	_privileged_fts_open		= __privileged_fts_open;
	_privileged_fts_read		= __privileged_fts_read;
	_privileged_fts_close		= __privileged_fts_close;
	_privileged_inotify_init	= __privileged_inotify_init;
	_privileged_inotify_init1	= __privileged_inotify_init1;
	_privileged_inotify_add_watch	= __privileged_inotify_add_watch;
	_privileged_inotify_rm_watch	= __privileged_inotify_rm_watch;
	_privileged_fork_execvp		= __privileged_fork_setuid_execvp;
	_privileged_kill_child		= __privileged_kill_child_wrapper;
# ifdef CGROUP_SUPPORT
	_privileged_clsync_cgroup_deinit= __privileged_clsync_cgroup_deinit;
# endif

	SAFE ( pthread_mutex_init_shared(&pthread_mutex_privileged_p),		return errno;);
	SAFE ( pthread_mutex_init_shared(&pthread_mutex_action_entrance_p),	return errno;);
	SAFE ( pthread_mutex_init_shared(&pthread_mutex_action_signal_p),	return errno;);
	SAFE ( pthread_mutex_init_shared(&pthread_mutex_runner_p),		return errno;);
	SAFE ( pthread_cond_init_shared (&pthread_cond_privileged_p),		return errno;);
	SAFE ( pthread_cond_init_shared (&pthread_cond_action_p),		return errno;);
	SAFE ( pthread_cond_init_shared (&pthread_cond_runner_p),		return errno;);

# ifdef READWRITE_SIGNALLING
	SAFE ( pipe2(pipefds, O_CLOEXEC), 				return errno;);
	priv_read_fd  = pipefds[0];
	priv_write_fd = pipefds[1];

	SAFE ( pipe2(pipefds, O_CLOEXEC), 				return errno;);
	nonp_read_fd  = pipefds[0];
	nonp_write_fd = pipefds[1];
# endif

	SAFE ( pthread_mutex_lock(pthread_mutex_runner_p),		return errno;);

	unshare(CLONE_NEWIPC);

	cmd_p = shm_malloc(sizeof(*cmd_p));

	// Running the privileged helper
	SAFE ( (helper_pid = myfork()) == -1,	return errno);
	if (!helper_pid)
		exit(privileged_handler(ctx_p));
	critical_on(!helper_isalive());

	// The rest routines
	if (ctx_p->flags[DETACH_NETWORK] == DN_NONPRIVILEGED) {
		SAFE ( cap_enable(CAP_TO_MASK(CAP_SYS_ADMIN)),	return errno; );
		SAFE ( unshare(CLONE_NEWNET),			return errno; );
	}
	SAFE ( cap_drop(ctx_p, 0),				return errno; );

	debug(4, "Waiting for the privileged thread to get prepared");
	pthread_cond_wait(pthread_cond_runner_p, pthread_mutex_runner_p);
	pthread_mutex_unlock(pthread_mutex_runner_p);

	debug(4, "Sending the settings (exec_uid == %u; exec_gid == %u)", ctx_p->synchandler_uid, ctx_p->synchandler_gid);
	cmd_p->arg.ctx_p = ctx_p;
	privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_SETUP,
			NULL
		);

	SAFE (pthread_mutex_destroy_shared(pthread_mutex_runner_p),	return errno;);
	SAFE (pthread_cond_destroy_shared(pthread_cond_runner_p),	return errno;);

# ifdef SECCOMP_SUPPORT
	if (ctx_p->flags[SECCOMP_FILTER])
		nonprivileged_seccomp_init();
# endif

	debug(5, "Finish");
	return 0;
#endif
}


int privileged_deinit(ctx_t *ctx_p)
{
	int ret = 0;
#ifdef CAPABILITIES_SUPPORT

	if (!ctx_p->flags[PROCESSSPLITTING])
		return 0;

	SAFE ( privileged_action(
# ifdef HL_LOCK_TRIES_AUTO
			PC_DEFAULT,
# endif
			PA_DIE,
			NULL
		),
		ret = errno
	);

# ifdef HL_LOCK_TRIES_AUTO
{
	int i=0;
	while (i < PC_MAX) {
		debug(1, "cmd_p->hl_lock.tries[%i] == %lu", i, cmd_p->hl_lock.tries[i]);
		i++;
	}
}
# endif

# ifdef HL_LOCKS
	hl_shutdown(HLLOCK_HANDLER);
# endif

	{
		int status;
		__privileged_kill_child_itself(helper_pid, SIGKILL);
		waitpid(helper_pid, &status, 0);
	}
/*
	SAFE ( pthread_mutex_destroy_shared(pthread_mutex_privileged_p),	ret = errno );
	SAFE ( pthread_mutex_destroy_shared(pthread_mutex_action_entrance_p),	ret = errno );
	SAFE ( pthread_mutex_destroy_shared(pthread_mutex_action_signal_p),	ret = errno );
	SAFE ( pthread_cond_destroy_shared(pthread_cond_privileged_p),		ret = errno );
	SAFE ( pthread_cond_destroy_shared(pthread_cond_action_p),		ret = errno );
*/
	shm_free((void *)cmd_p);
#endif

	debug(2, "endof privileged_deinit()");
	return ret;
}


