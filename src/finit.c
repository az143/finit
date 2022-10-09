/* Finit - Fast /sbin/init replacement w/ I/O, hook & service plugins
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2022  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <ctype.h>
#include <dirent.h>
#include <getopt.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif
#include <time.h>		/* tzet() */
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/stat.h>		/* umask(), mkdir() */
#include <sys/wait.h>
#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif
#ifdef _LIBITE_LITE
# include <libite/lite.h>
#else
# include <lite/lite.h>
#endif

#include "finit.h"
#include "cgroup.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "service.h"
#include "sig.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "schedule.h"
#include "watchdog.h"

int   runlevel  = 0;		/* Bootstrap 'S' */
int   cfglevel  = RUNLEVEL;	/* Fallback if no configured runlevel */
int   cmdlevel  = 0;		/* runlevel override from cmdline */
int   prevlevel = -1;
int   debug     = 0;		/* debug mode from kernel cmdline */
int   rescue    = 0;		/* rescue mode from kernel cmdline */
int   single    = 0;		/* single user mode from kernel cmdline */
int   bootstrap = 1;		/* set while bootrapping (for TTYs) */
int   kerndebug = 0;		/* set if /proc/sys/kernel/printk > 7 */
char *fstab     = FINIT_FSTAB;
char *sdown     = NULL;
char *network   = NULL;
char *hostname  = NULL;
char *rcsd      = FINIT_RCSD;
char *runparts  = NULL;
char *osheading = NULL;

uev_ctx_t *ctx  = NULL;		/* Main loop context */
svc_t *wdog     = NULL;		/* No watchdog by default */


/*
 * Show user configured banner before service bootstrap progress
 */
static void banner(void)
{
	/*
	 * Silence kernel logs, assuming users have sysklogd or
	 * similar enabled to start emptying /dev/kmsg, but for
	 * our progress we want to own the console.
	 */
	if (!debug && !kerndebug)
		klogctl(6, NULL, 0);

	/*
	 * First level hooks, if you want to run here, you're
	 * pretty much on your own.  Nothing's up yet ...
	 */
	plugin_run_hooks(HOOK_BANNER);

#ifdef INIT_OSHEADING
	osheading = INIT_OSHEADING;
	if (osheading) {
		if (!osheading[0])
			osheading = release_heading();
		print_banner(osheading);
	}
#endif
}

static int sulogin(int do_reboot)
{
	int rc = EX_OSFILE;
	char *cmd[] = {
		_PATH_SULOGIN,
		"sulogin",
	};
	size_t i;

	for (i = 0; i < NELEMS(cmd); i++) {
		char *path = which(cmd[i]);

		if (!path)
			continue;

		if (access(path, X_OK)) {
			free(path);
			continue;
		}

		rc = systemf("%s", path);
		free(path);
		break;
	}

	if (do_reboot) {
		do_shutdown(SHUT_REBOOT);
		exit(rc);
	}

	return rc;
}

char *fs_root_dev(char *real, size_t len)
{
	struct dirent *d;
	struct stat st;
	int found = 0;
	dev_t dev;
	DIR *dir;

	if (stat("/", &st))
		return NULL;

	if (S_ISBLK(st.st_mode))
		dev = st.st_rdev;
	else
		dev = st.st_dev;

	dir = opendir("/sys/block");
	if (!dir)
		return NULL;

	while ((d = readdir(dir))) {
		char buf[10];
		char *ptr;

		if (fnread(buf, sizeof(buf), "/sys/block/%s/dev", d->d_name) == -1)
			continue;

		ptr = strchr(buf, ':');
		if (!ptr)
			continue;
		*ptr++ = 0;

		if (atoi(buf) == (int)major(dev) && atoi(ptr) == (int)minor(dev)) {
			/* Guess name, assume no renaming */
			snprintf(real, len, "/dev/%s", d->d_name);
			found = 1;
			break;
		}
	}

	closedir(dir);
	if (!found)
		return NULL;

	return real;
}

/*
 * Check all filesystems in /etc/fstab with a fs_passno > 0
 */
static int fsck(int pass)
{
	struct mntent mount;
	struct mntent *mnt;
	char real[192];
	char buf[256];
	int rc = 0;
	FILE *fp;

	fp = setmntent(fstab, "r");
	if (!fp) {
		err(1, "Failed opening fstab: %s", fstab);
		sulogin(1);
	}
	dbg("Opened %s, pass %d", fstab, pass);
	while ((mnt = getmntent_r(fp, &mount, buf, sizeof(buf)))) {
		int fsck_rc = 0;
		struct stat st;
		char cmd[256];
		char *dev;

		dbg("got: fsname '%s' dir '%s' type '%s' opts '%s' freq '%d' passno '%d'",
		   mnt->mnt_fsname, mnt->mnt_dir, mnt->mnt_type, mnt->mnt_opts,
		   mnt->mnt_freq, mnt->mnt_passno);

		if (mnt->mnt_passno == 0 || mnt->mnt_passno != pass)
			continue;

		/* Device to maybe fsck,  */
		dev = mnt->mnt_fsname;

		errno = 0;
		if (stat(dev, &st) || !S_ISBLK(st.st_mode)) {
			int skip = 1;

			if (string_match(dev, "UUID=") || string_match(dev, "LABEL="))
				skip = 0;

			/*
			 * Kernel short form for root= device, figure out
			 * actual device since we cannot rely on symlinks
			 * https://bugs.busybox.net/show_bug.cgi?id=8891
			 */
			else if (string_compare(dev, "/dev/root")) {
				if (fs_root_dev(real, sizeof(real))) {
					dev = real;
					skip = 0;
				}
			}

			if (skip) {
				dbg("Cannot fsck %s, not a block device: %s", dev, strerror(errno));
				continue;
			}
		}

		if (ismnt("/proc/mounts", mnt->mnt_dir, "rw")) {
			dbg("Skipping fsck of %s, already mounted rw on %s.", dev, mnt->mnt_dir);
			continue;
		}

#ifdef FSCK_FIX
		snprintf(cmd, sizeof(cmd), "fsck -yf %s", dev);
#else
		snprintf(cmd, sizeof(cmd), "fsck -a %s", dev);
#endif
		dbg("Running pass %d fsck command %s", pass, cmd);
		fsck_rc = run_interactive(cmd, "Checking filesystem %s", dev);
		/*
		 * "failure" is defined as exiting with a return code of
		 * 2 or larger.  A return code of 1 indicates that filesystem
		 * errors were corrected but that the boot may proceed.
		 */
		if (fsck_rc > 1) {
			logit(LOG_CONSOLE | LOG_ALERT, "Failed fsck %s, attempting sulogin ...", dev);
			sulogin(1);
		}
		rc += fsck_rc;
	}

	endmntent(fp);

	return rc;
}

static int fsck_all(void)
{
	int rc = 0;
#ifndef FAST_BOOT
	int pass;

	for (pass = 1; pass < 10; pass++) {
		rc = fsck(pass);
		if (rc)
			break;
	}
#endif
	return rc;
}

/* Wrapper for mount(2), logs any errors to stderr */
static void fs_mount(const char *src, const char *tgt, const char *fstype,
		     unsigned long flags, const void *data)
{
	const char *msg = !fstype ? "MS_MOVE" : "mounting";
	int rc;

	rc = mount(src, tgt, fstype, flags, data);
	if (rc && errno != EBUSY)
		err(1, "Failed %s %s on %s", msg, src, tgt);
}

#ifndef SYSROOT
static void fs_remount_root(int fsckerr)
{
	struct mntent *mnt;
	FILE *fp;

	fp = setmntent(fstab, "r");
	if (!fp)
		return;

	while ((mnt = getmntent(fp))) {
		if (!strcmp(mnt->mnt_dir, "/"))
			break;
	}

	/* If / is not listed in fstab, or listed as 'ro', leave it alone */
	if (!mnt || hasmntopt(mnt, "ro"))
		goto out;

	if (fsckerr)
		print(1, "Cannot remount / as read-write, fsck failed before");
	else
		run_interactive("mount -n -o remount,rw /",
				"Remounting / as read-write");

out:
	endmntent(fp);
}
#else
static void fs_remount_root(int fsckerr)
{
	/*
	 * XXX: Untested, in the initramfs age we should
	 *      probably use switch_root instead.
	 */
	fs_mount(SYSROOT, "/", NULL, MS_MOVE, NULL);
}
#endif	/* SYSROOT */

/*
 * Opinionated file system setup.  Checks for critical mount points and
 * mounts them as most users expect.  All file systems are checked with
 * /proc/mounts before mounting.
 *
 * Embedded systems, and other people who want full control, can set up
 * their system with /etc/fstab, which is handled before this function
 * is called.  For systems like Debian/Ubuntu, who only have / and swap
 * in their /etc/fstab, this function does all the magic necessary.
 */
static void fs_finalize(void)
{
	/*
	 * Some systems rely on us to both create /dev/shm and, to mount
	 * a tmpfs there.  Any system with dbus needs shared memory, so
	 * mount it, unless its already mounted, but not if listed in
	 * the /etc/fstab file already.
	 */
	if (!fismnt("/dev/shm")) {
		makedir("/dev/shm", 0777);
		fs_mount("shm", "/dev/shm", "tmpfs", 0, "mode=0777");
	}

	/* Modern systems use /dev/pts */
	if (!fismnt("/dev/pts")) {
		char opts[32];
		int mode;
		int gid;

		gid = getgroup("tty");
		if (gid == -1)
			gid = 0;

		/* 0600 is default on Debian, use 0620 to get mesg y by default */
		mode = 0620;
		snprintf(opts, sizeof(opts), "gid=%d,mode=%d,ptmxmode=0666", gid, mode);

		makedir("/dev/pts", 0755);
		fs_mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, opts);
	}

	/*
	 * Modern systems use tmpfs for /run.  Fallback to /var/run if
	 * /run doesn't exist is handled by the bootmisc plugin.  It
	 * also sets up compat symlinks.
	 *
	 * The unconditional mount of /run/lock is for DoS prevention.
	 * To override any of this behavior, add entries to /etc/fstab
	 * for /run (and optionally /run/lock).
	 */
	if (fisdir("/run") && !fismnt("/run")) {
		fs_mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME, "mode=0755,size=10%");

		/* This prevents user DoS of /run by filling /run/lock at the expense of another tmpfs, max 5MiB */
		makedir("/run/lock", 1777);
		fs_mount("tmpfs", "/run/lock", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME, "mode=0777,size=5252880");
	}

	/* Modern systems use tmpfs for /tmp */
	if (!fismnt("/tmp"))
		fs_mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

static void fs_swapon(char *cmd, size_t len)
{
	struct mntent *mnt;
	FILE *fp;

	if (!whichp("swapon"))
		return;

	fp = setmntent(fstab, "r");
	if (!fp)
		return;

	while ((mnt = getmntent(fp))) {
		if (strcmp(mnt->mnt_type, MNTTYPE_SWAP))
			continue;

		snprintf(cmd, len, "swapon %s", mnt->mnt_fsname);
		run_interactive(cmd, "Enabling swap %s", mnt->mnt_fsname);
	}

	endmntent(fp);
}

static void fs_mount_all(void)
{
	char cmd[256] = "mount -na";

	if (!fstab || !fexist(fstab)) {
		logit(LOG_CONSOLE | LOG_NOTICE, "%s system fstab %s, trying fallback ...",
		      !fstab ? "Missing" : "Cannot find", fstab ?: "\b");
		fstab = FINIT_FSTAB;
	}
	if (!fstab || !fexist(fstab)) {
		logit(LOG_CONSOLE | LOG_EMERG, "%s system fstab %s, attempting sulogin ...",
		      !fstab ? "Missing" : "Cannot find", fstab ?: "\b");
		sulogin(1);
	}

	/*
	 * Needed by fsck, both BusyBox and util-linux support this.
	 * We leave it set in the env. for the benefit of any mount
	 * helpers and other system tools that the user expects to
	 * behave even if we've booted with a different fstab.
	 */
	setenv("FSTAB_FILE", fstab, 1);

	if (!rescue)
		fs_remount_root(fsck_all());

	dbg("Root FS up, calling hooks ...");
	plugin_run_hooks(HOOK_ROOTFS_UP);

	if (fstab && strcmp(fstab, "/etc/fstab"))
		snprintf(cmd, sizeof(cmd), "mount -na -T %s", fstab);

	if (run_interactive(cmd, "Mounting filesystems from %s", fstab))
		plugin_run_hooks(HOOK_MOUNT_ERROR);

	dbg("Calling extra mount hook, after mount -a ...");
	plugin_run_hooks(HOOK_MOUNT_POST);

	dbg("Enable any swap ...");
	fs_swapon(cmd, sizeof(cmd));

	dbg("Finalize, ensure common file systems are available ...");
	fs_finalize();
}

/*
 * We need /proc for rs_remount_root() and conf_parse_cmdline(), /dev
 * for early multi-console, and /sys for the cgroups support.  Any
 * occurrence of these file systems in /etc/fstab will replace these
 * mounts later in fs_mount_all()
 *
 * Ignore any mount errors with EBUSY, kernel likely already mounted
 * the filesystem for us automatically, e.g., CONFIG_DEVTMPFS_MOUNT.
 */
static void fs_init(void)
{
	struct {
		char *spec, *file, *type;
	} fs[] = {
		{ "proc",     "/proc", "proc"     },
		{ "devtmpfs", "/dev",  "devtmpfs" },
		{ "sysfs",    "/sys",  "sysfs"    },
	};
	size_t i;

	/* mask writable bit for g and o */
	umask(022);

	for (i = 0; i < NELEMS(fs); i++) {
		/*
		 * Check if already mounted, we may be running in a
		 * container, or an initramfs ran before us.  The
		 * function fismnt() reliles on /proc/mounts being
		 * unique for each chroot/container.
		 */
		if (fismnt(fs[i].file))
			continue;

		fs_mount(fs[i].spec, fs[i].file, fs[i].type, 0, NULL);
	}
}


/*
 * Handle bootstrap transition to configured runlevel, start TTYs
 *
 * This is the final stage of bootstrap.  It changes to the default
 * (configured) runlevel, calls all external start scripts and final
 * bootstrap hooks before bringing up TTYs.
 *
 * We must ensure that all declared `task [S]` and `run [S]` jobs in
 * finit.conf, or *.conf in finit.d/, run to completion before we
 * finalize the bootstrap process by calling this function.
 */
static void finalize(void *unused)
{
	/* Clean up bootstrap-only tasks/services that never started */
	dbg("Clean up all bootstrap-only tasks/services ...");
	svc_prune_bootstrap();

	/* All services/tasks/etc. in configure runlevel have started */
	dbg("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Convenient SysV compat for when you just don't care ... */
	if (!access(FINIT_RC_LOCAL, X_OK) && !rescue)
		run_interactive(FINIT_RC_LOCAL, "Calling %s", FINIT_RC_LOCAL);

	/* Hooks that should run at the very end */
	dbg("Calling all system up hooks ...");
	plugin_run_hooks(HOOK_SYSTEM_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Disable progress output at normal runtime */
	enable_progress(0);

	/* System bootrapped, launch TTYs et al */
	bootstrap = 0;
	service_step_all(SVC_TYPE_RESPAWN);
}

/*
 * Start cranking the big state machine
 */
static void crank_worker(void *unused)
{
	/*
	 * Initialize state machine and start all bootstrap tasks
	 * NOTE: no network available!
	 */
	sm_init(&sm);
	sm_step(&sm);
}

/*
 * Wait for system bootstrap to complete, all SVC_TYPE_RUNTASK must be
 * allowed to complete their work in [S], or timeout, before we switch
 * to the configured runlevel and call finalize(), should not take more
 * than 120 sec.
 */
static void bootstrap_worker(void *work)
{
	static struct wq final = {
		.cb = finalize,
		.delay = 10
	};
	static int cnt = 120 * 10;	/* We run with 100ms period */
	int level = cfglevel;

	/*
	 * Set up inotify watcher for /etc/finit.conf, /etc/finit.d, and
	 * their deps, to figure out how to bootstrap the system.
	 */
	conf_monitor();

	/*
	 * Background service tasks
	 */
	service_init();

	dbg("Step all services ...");
	service_step_all(SVC_TYPE_ANY);

	if (cnt-- > 0 && !service_completed()) {
		dbg("Not all bootstrap run/tasks have completed yet ... %d", cnt);
		schedule_work(work);
		return;
	}

	if (cnt > 0)
		dbg("All run/task have completed, resuming bootstrap.");
	else
		dbg("Timeout, resuming bootstrap.");

	dbg("Starting runlevel change finalize ...");
	schedule_work(&final);

	/*
	 * Run startup scripts in the runparts directory, if any.
	 */
	if (runparts && fisdir(runparts) && !rescue)
		run_parts(runparts, NULL);


	/*
	 * Start all tasks/services in the configured runlevel, or jump
	 * into the runlevel selected from the command line.
	 */
	if (cmdlevel) {
		dbg("Runlevel %d requested from command line, starting all services ...", cmdlevel);
		level = cmdlevel;
	} else
		dbg("Change to default runlevel(%d), starting all services ...", cfglevel);

	service_runlevel(level);
}

static int version(int rc)
{
	puts(PACKAGE_STRING);
	printf("Bug report address: %-40s\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
	printf("Project homepage: %s\n", PACKAGE_URL);
#endif

	return rc;
}

static int usage(int rc)
{
	printf("Usage: %s [OPTIONS] [q | Q | 0-9]\n\n"
	       "Options:\n"
//	       "  -a       Ignored, compat SysV init\n"
//	       "  -b       Ignored, compat SysV init\n"
//	       "  -e arg   Ignored, compat SysV init\n"
	       "  -h       This help text\n"
//	       "  -s       Ignored, compat SysV init\n"
//	       "  -t sec   Ignored, compat SysV init\n"
	       "  -v       Show Finit version\n"
//	       "  -z xxx   Ignored, compat SysV init\n"
	       "\n"
	       "Commands:\n"
	       "  0        Power-off the system, same as initctl poweroff\n"
	       "  6        Reboot the system, same as initctl reboot\n"
	       "  2-9      Change runlevel\n"
	       "  q, Q     Reload /etc/finit.conf and/or any *.conf in /etc/finit.d/\n"
	       "           if modified, same as initctl reload or SIGHUP to PID 1\n"
	       "  1, s, S  Enter system rescue mode, runlevel 1\n"
	       "\n", prognm);

	return rc;
}

/*
 * wrapper for old-style init/telinit commands, for compat with
 * /usr/bin/shutdown from sysvinit, and old fingers
 */
static int telinit(int argc, char *argv[])
{
	int c;

	progname(argv[0]);
	while ((c = getopt(argc, argv, "abe:h?st:vVz:")) != EOF) {
		switch(c) {
		case 'a': case 'b': case 'e': case 's': case 'z':
			break;		/* ign, compat */

		case 't':		/* optarg == killdelay */
			break;

		case 'v': case 'V':
			return version(0);

		case 'h':
		case '?':
			return usage(0);
		}
	}

	if (optind < argc) {
		int req = (int)argv[optind][0];

		if (isdigit(req))
			return systemf("initctl -b runlevel %c", req);

		if (req == 'q' || req == 'Q')
			return systemf("initctl -b reload");

		if (req == 's' || req == 'S')
			return systemf("initctl -b runlevel %c", req);
	}

	/* XXX: add non-pid1 process monitor here
	 *
	 *       finit -f ~/.config/finit.conf &
	 *
	 */

	return usage(1);
}

int main(int argc, char *argv[])
{
	struct wq crank_work = {
		.cb = crank_worker,
		.delay = 10
	};
	struct wq bootstrap_work = {
		.cb = bootstrap_worker,
		.delay = 100
	};
	uev_ctx_t loop;

	/* user calling telinit or init */
	if (getpid() != 1)
		return telinit(argc, argv);

	/*
	 * Need /dev, /proc, and /sys for console=, remount and cgroups
	 */
	fs_init();

	/*
	 * Parse /proc/cmdline (debug, rescue, console=, etc.)
	 */
	conf_parse_cmdline(argc, argv);

	/*
	 * Figure out system console(s) and call log_init() to set
	 * correct log level, possibly finit.debug enabled.
	 */
	console_init();

	/*
	 * Initialize event context.
	 */
	uev_init1(&loop, 1);
	ctx = &loop;

	/*
	 * Set PATH, SHELL, and PWD early to something sane
	 */
	conf_reset_env();

	if (chdir("/"))
		err(1, "Failed cd /");

	/*
	 * In case of emergency.
	 */
	if (rescue)
		rescue = sulogin(0);

	/*
	 * Load plugins early, the first hook is in banner(), so we
	 * need plugins loaded before calling it.
	 */
	plugin_init(&loop);

	/*
	 * Hello world.
	 */
	enable_progress(1);	/* Allow progress, if enabled */
	banner();

	if (osheading)
		logit(LOG_CONSOLE | LOG_NOTICE, "%s, entering runlevel S", osheading);
	else
		logit(LOG_CONSOLE | LOG_NOTICE, "Entering runlevel S");

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Initialize default control groups, if available
	 */
	cgroup_init(&loop);

	/*
	 * Check custom fstab from cmdline, including fallback, then run
	 * fsck before mounting all filesystems, on error call sulogin.
	 */
	fs_mount_all();

	/* Bootstrap conditions, needed for hooks */
	cond_init();

	/*
	 * Emit conditions for early hooks that ran before the condition
	 * system was initialized in case anyone.
	 */
	cond_set_oneshot(plugin_hook_str(HOOK_BANNER));
	cond_set_oneshot(plugin_hook_str(HOOK_ROOTFS_UP));

	/*
	 * Initialize .conf system and load static /etc/finit.conf.
	 */
	conf_init(&loop);

	/*
	 * Start built-in watchdogd as soon as possible, if enabled
	 */
	if (whichp(FINIT_LIBPATH_ "/watchdogd") && fexist(WDT_DEVNODE)) {
		service_register(SVC_TYPE_SERVICE, "[123456789] cgroup.init name:watchdog :finit " FINIT_LIBPATH_ "/watchdogd -- Finit watchdog daemon", global_rlimit, NULL);
		wdog = svc_find("watchdog", "finit");
	}

	/*
	 * Start kernel event daemon as soon as possible, if enabled
	 */
	if (whichp(FINIT_LIBPATH_ "/keventd"))
		service_register(SVC_TYPE_SERVICE, "[123456789] cgroup.init " FINIT_LIBPATH_ "/keventd -- Finit kernel event daemon", global_rlimit, NULL);

	/* Base FS up, enable standard SysV init signals */
	sig_setup(&loop);

	dbg("Base FS up, calling hooks ...");
	plugin_run_hooks(HOOK_BASEFS_UP);

	dbg("Starting initctl API responder ...");
	api_init(&loop);

	dbg("Starting the big state machine ...");
	schedule_work(&crank_work);

	dbg("Starting bootstrap finalize timer ...");
	schedule_work(&bootstrap_work);

	/*
	 * Enter main loop to monitor /dev/initctl and services
	 */
	dbg("Entering main loop ...");
	return uev_run(&loop, 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
