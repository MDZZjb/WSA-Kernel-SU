// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright (C) 2021 LoveSy <shana@zju.edu.cn>. All Rights Reserved.
 */

/* Hello. If this is enabled in your kernel for some reason, whoever is
 * distributing your kernel to you is a complete moron, and you shouldn't
 * use their kernel anymore. But it's not my fault! People: don't enable
 * this driver! (Note that the existence of this file does not imply the
 * driver is actually in use. Look in your .config to see whether this is
 * enabled.) -Jason
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/ptrace.h>
#include <linux/syscalls.h>

typedef long (* syscall_wrapper)(struct pt_regs *);

/* Policy capabilities */
enum {
	POLICYDB_CAPABILITY_NETPEER,
	POLICYDB_CAPABILITY_OPENPERM,
	POLICYDB_CAPABILITY_EXTSOCKCLASS,
	POLICYDB_CAPABILITY_ALWAYSNETWORK,
	POLICYDB_CAPABILITY_CGROUPSECLABEL,
	POLICYDB_CAPABILITY_NNP_NOSUID_TRANSITION,
	POLICYDB_CAPABILITY_GENFS_SECLABEL_SYMLINKS,
	__POLICYDB_CAPABILITY_MAX
};

struct selinux_state {
#ifdef CONFIG_SECURITY_SELINUX_DISABLE
	bool disabled;
#endif
#ifdef CONFIG_SECURITY_SELINUX_DEVELOP
	bool enforcing;
#endif
	bool checkreqprot;
	bool initialized;
	bool policycap[__POLICYDB_CAPABILITY_MAX];

	struct page *status_page;
	struct mutex status_lock;

	struct selinux_avc *avc;
	struct selinux_policy __rcu *policy;
	struct mutex policy_mutex;
} __randomize_layout;

extern struct selinux_state selinux_state;

static inline void enforcing_set(struct selinux_state *state, bool value)
{
	WRITE_ONCE(state->enforcing, value);
}

static bool is_su(const char __user *filename)
{
	static const char su_path[] = "/system/xbin/su";
	char ufn[sizeof(su_path)];

	return likely(!copy_from_user(ufn, filename, sizeof(ufn))) &&
	       unlikely(!memcmp(ufn, su_path, sizeof(ufn)));
}

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
	/* To avoid having to mmap a page in userspace, just write below the stack pointer. */
	char __user *p = (void __user *)current_user_stack_pointer() - len;

	return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *sh_user_path(void)
{
	static const char sh_path[] = "/system/bin/sh";

	return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static syscall_wrapper old_newfstatat;

static long new_newfstatat(struct pt_regs* regs)
{
	if (is_su((const char __user*)regs->si))
		regs->si = sh_user_path();
	return old_newfstatat(regs);
}

static syscall_wrapper old_faccessat;
static long new_faccessat(struct pt_regs* regs)
{
	if (is_su((const char __user*)regs->si))
		regs->si = sh_user_path();
	return old_faccessat(regs);
}

static syscall_wrapper old_execve;
static long new_execve(struct pt_regs* regs)
{
	static const char now_root[] = "You are now root.\n";
	struct cred *cred;
	const char __user * filename = regs->di;
	if (!is_su(filename))
		return old_execve(regs);

	if (!old_execve(regs))
		return 0;

	/* It might be enough to just change the security ctx of the
	 * current task, but that requires slightly more thought than
	 * just axing the whole thing here.
	 */
	enforcing_set(&selinux_state, false);

	/* Rather than the usual commit_creds(prepare_kernel_cred(NULL)) idiom,
	 * we manually zero out the fields in our existing one, so that we
	 * don't have to futz with the task's key ring for disk access.
	 */
	cred = (struct cred *)__task_cred(current);
	memset(&cred->uid, 0, sizeof(cred->uid));
	memset(&cred->gid, 0, sizeof(cred->gid));
	memset(&cred->suid, 0, sizeof(cred->suid));
	memset(&cred->euid, 0, sizeof(cred->euid));
	memset(&cred->egid, 0, sizeof(cred->egid));
	memset(&cred->fsuid, 0, sizeof(cred->fsuid));
	memset(&cred->fsgid, 0, sizeof(cred->fsgid));
	memset(&cred->cap_inheritable, 0xff, sizeof(cred->cap_inheritable));
	memset(&cred->cap_permitted, 0xff, sizeof(cred->cap_permitted));
	memset(&cred->cap_effective, 0xff, sizeof(cred->cap_effective));
	memset(&cred->cap_bset, 0xff, sizeof(cred->cap_bset));
	memset(&cred->cap_ambient, 0xff, sizeof(cred->cap_ambient));

	ksys_write(2, userspace_stack_buffer(now_root, sizeof(now_root)),
		  sizeof(now_root) - 1);

	regs->di = sh_user_path();
	return old_execve(regs);
}

extern const unsigned long sys_call_table[];
static void read_syscall(void **ptr, unsigned int syscall)
{
	*ptr = READ_ONCE(*((void **)sys_call_table + syscall));
}
static void replace_syscall(unsigned int syscall, void *ptr)
{
	WRITE_ONCE(*((void **)sys_call_table + syscall), ptr);
}
#define read_and_replace_syscall(name) do { \
	read_syscall((void **)&old_ ## name, __NR_ ## name); \
	replace_syscall(__NR_ ## name, &new_ ## name); \
} while (0)

static int superuser_init(void)
{
	pr_err("WARNING WARNING WARNING WARNING WARNING\n");
	pr_err("This kernel has kernel-assisted superuser and contains a\n");
	pr_err("trivial way to get root. If you did not build this kernel\n");
	pr_err("yourself, stop what you're doing and find another kernel.\n");
	pr_err("This one is not safe to use.\n");
	pr_err("WARNING WARNING WARNING WARNING WARNING\n");

	read_and_replace_syscall(newfstatat);
	read_and_replace_syscall(faccessat);
	read_and_replace_syscall(execve);

	return 0;
}

module_init(superuser_init);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Kernel-assisted superuser for Android");
MODULE_AUTHOR("Jason A. Donenfeld <Jason@zx2c4.com> & LoveSy <shana@zju.edu.cn>");
