#include <sys/mount.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <libgen.h>
#include <vector>
#include <string>

#include <magisk.hpp>
#include <db.hpp>
#include <utils.hpp>
#include <daemon.hpp>
#include <resetprop.hpp>
#include <selinux.hpp>

using namespace std;

static bool no_secure_dir = false;
static bool pfs_done = false;

/*********
 * Setup *
 *********/

#define DIR_IS(part)      (me->mnt_dir == "/" #part ""sv)
#define SETMIR(b, part)   sprintf(b, "%s/" MIRRDIR "/" #part, MAGISKTMP.data())
#define SETBLK(b, part)   sprintf(b, "%s/" BLOCKDIR "/" #part, MAGISKTMP.data())

#define mount_mirror(part, flag) \
else if (DIR_IS(part) && me->mnt_type != "tmpfs"sv && lstat(me->mnt_dir, &st) == 0) { \
	SETMIR(buf1, part); \
	SETBLK(buf2, part); \
	mknod(buf2, S_IFBLK | 0600, st.st_dev); \
	xmkdir(buf1, 0755); \
	xmount(buf2, buf1, me->mnt_type, flag, nullptr); \
	LOGI("mount: %s\n", buf1); \
}

#define link_mirror(part) \
SETMIR(buf1, part); \
if (access("/system/" #part, F_OK) == 0 && access(buf1, F_OK) != 0) { \
	xsymlink("./system/" #part, buf1); \
	LOGI("link: %s\n", buf1); \
}

static bool magisk_env() {
	LOGI("* Initializing Magisk environment\n");

	string pkg;
	check_manager(&pkg);

	char buf1[4096];
	char buf2[4096];

	sprintf(buf1, "%s/0/%s/install", APP_DATA_DIR, pkg.data());

	// Alternative binaries paths
	const char *alt_bin[] = { "/cache/data_adb/magisk", "/data/magisk", buf1 };
	for (auto alt : alt_bin) {
		struct stat st;
		if (lstat(alt, &st) == 0) {
			if (S_ISLNK(st.st_mode)) {
				unlink(alt);
				continue;
			}
			rm_rf(DATABIN);
			cp_afc(alt, DATABIN);
			rm_rf(alt);
			break;
		}
	}

	// Remove stuffs
	rm_rf("/cache/data_adb");
	rm_rf("/data/adb/modules/.core");
	unlink("/data/adb/magisk.img");
	unlink("/data/adb/magisk_merge.img");
	unlink("/data/magisk.img");
	unlink("/data/magisk_merge.img");
	unlink("/data/magisk_debug.log");

	sprintf(buf1, "%s/" MODULEMNT, MAGISKTMP.data());
	xmkdir(buf1, 0755);

	// Directories in /data/adb
	xmkdir(DATABIN, 0755);
	xmkdir(MODULEROOT, 0755);
	xmkdir(SECURE_DIR "/post-fs-data.d", 0755);
	xmkdir(SECURE_DIR "/service.d", 0755);

	LOGI("* Mounting mirrors");

	parse_mnt("/proc/mounts", [&](mntent *me) {
		struct stat st;
		if (0) {}
		mount_mirror(system, MS_RDONLY)
		mount_mirror(vendor, MS_RDONLY)
		mount_mirror(product, MS_RDONLY)
		mount_mirror(system_ext, MS_RDONLY)
		mount_mirror(data, 0)
		else if (SDK_INT >= 24 && DIR_IS(proc) && !strstr(me->mnt_opts, "hidepid=2")) {
			xmount(nullptr, "/proc", nullptr, MS_REMOUNT, "hidepid=2,gid=3009");
		}
		return true;
	});
	SETMIR(buf1, system);
	SETMIR(buf2, system_root);
	if (access(buf1, F_OK) != 0 && access(buf2, F_OK) == 0) {
		xsymlink("./system_root/system", buf1);
		LOGI("link: %s\n", buf1);
	}
	link_mirror(vendor);
	link_mirror(product);
	link_mirror(system_ext);

	// Disable/remove magiskhide, resetprop
	if (SDK_INT < 19) {
		unlink("/sbin/resetprop");
		unlink("/sbin/magiskhide");
	}

	if (access(DATABIN "/busybox", X_OK) == -1)
		return false;

	// TODO: Remove. Backwards compatibility for old manager
	LOGI("* Setting up internal busybox\n");
	sprintf(buf1, "%s/" BBPATH "/busybox", MAGISKTMP.data());
	mkdir(dirname(buf1), 0755);
	cp_afc(DATABIN "/busybox", buf1);
	exec_command_sync(buf1, "--install", "-s", dirname(buf1));

	return true;
}

void reboot() {
	if (RECOVERY_MODE)
		exec_command_sync("/system/bin/reboot", "recovery");
	else
		exec_command_sync("/system/bin/reboot");
}

static bool check_data() {
	bool mnt = false;
	bool data = false;
	file_readline("/proc/mounts", [&](string_view s) -> bool {
		if (str_contains(s, " /data ") && !str_contains(s, "tmpfs"))
			mnt = true;
		return true;
	});
	if (mnt) {
		auto crypto = getprop("ro.crypto.state");
		if (!crypto.empty()) {
			if (crypto == "unencrypted") {
				// Unencrypted, we can directly access data
				data = true;
			} else {
				// Encrypted, check whether vold is started
				data = !getprop("init.svc.vold").empty();
			}
		} else {
			// ro.crypto.state is not set, assume it's unencrypted
			data = true;
		}
	}
	return data;
}

void unlock_blocks() {
	int fd, dev, OFF = 0;

	auto dir = xopen_dir("/dev/block");
	if (!dir)
		return;
	dev = dirfd(dir.get());

	for (dirent *entry; (entry = readdir(dir.get()));) {
		if (entry->d_type == DT_BLK) {
			if ((fd = openat(dev, entry->d_name, O_RDONLY | O_CLOEXEC)) < 0)
				continue;
			if (ioctl(fd, BLKROSET, &OFF) < 0)
				PLOGE("unlock %s", entry->d_name);
			close(fd);
		}
	}
}

static bool log_dump = false;
static void dump_logs() {
	if (log_dump)
		return;
	int test = exec_command_sync("/system/bin/logcat", "-d", "-f", "/dev/null");
	chmod("/dev/null", 0666);
	if (test != 0)
		return;
	rename(LOGFILE, LOGFILE ".bak");
	log_dump = true;
	// Start a daemon thread and wait indefinitely
	new_daemon_thread([]() -> void {
		int fd = xopen(LOGFILE, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
		exec_t exec {
			.fd = fd,
			.fork = fork_no_zombie
		};
		int pid = exec_command(exec, "/system/bin/logcat", "-s", "Magisk");
		close(fd);
		if (pid < 0) {
			log_dump = false;
		} else {
			waitpid(pid, nullptr, 0);
		}
	});
}

/****************
 * Entry points *
 ****************/

[[noreturn]] static void unblock_boot_process() {
	close(xopen(UNBLOCKFILE, O_RDONLY | O_CREAT, 0));
	pthread_exit(nullptr);
}

[[noreturn]] static void core_only() {
	pfs_done = true;
	auto_start_magiskhide();
	unblock_boot_process();
}

void post_fs_data(int client) {
	// ack
	write_int(client, 0);
	close(client);

	if (getenv("REMOUNT_ROOT"))
		xmount(nullptr, "/", nullptr, MS_REMOUNT | MS_RDONLY, nullptr);

	if (!check_data())
		unblock_boot_process();

	dump_logs();

	LOGI("** post-fs-data mode running\n");

	// Unlock all blocks for rw
	unlock_blocks();

	if (access(SECURE_DIR, F_OK) != 0) {
		/* If the folder is not automatically created by the system,
		 * do NOT proceed further. Manual creation of the folder
		 * will cause bootloops on FBE devices. */
		LOGE(SECURE_DIR " is not present, abort...");
		no_secure_dir = true;
		unblock_boot_process();
	}

	if (!magisk_env()) {
		LOGE("* Magisk environment setup incomplete, abort\n");
		unblock_boot_process();
	}

	LOGI("* Running post-fs-data.d scripts\n");
	exec_common_script("post-fs-data");

	// Core only mode
	if (access(DISABLEFILE, F_OK) == 0)
		core_only();

	handle_modules();

	core_only();
}

void late_start(int client) {
	LOGI("** late_start service mode running\n");
	// ack
	write_int(client, 0);
	close(client);

	dump_logs();

	if (no_secure_dir) {
		// It's safe to create the folder at this point if the system didn't create it
		if (access(SECURE_DIR, F_OK) != 0)
			xmkdir(SECURE_DIR, 0700);
		// And reboot to make proper setup possible
		reboot();
	}

	if (!pfs_done)
		return;

	auto_start_magiskhide();

	LOGI("* Running service.d scripts\n");
	exec_common_script("service");

	// Core only mode
	if (access(DISABLEFILE, F_OK) != 0) {
		LOGI("* Running module service scripts\n");
		exec_module_script("service", module_list);
	}

	// All boot stage done, cleanup
	module_list.clear();
	module_list.shrink_to_fit();
}

void boot_complete(int client) {
	LOGI("** boot_complete triggered\n");
	// ack
	write_int(client, 0);
	close(client);

	if (!pfs_done)
		return;

	auto_start_magiskhide();

	if (access(MANAGERAPK, F_OK) == 0) {
		// Install Magisk Manager if exists
		rename(MANAGERAPK, "/data/magisk.apk");
		install_apk("/data/magisk.apk");
	} else {
		// Check whether we have manager installed
		if (!check_manager()) {
			// Install stub
			exec_command_sync("/sbin/magiskinit", "-x", "manager", "/data/magisk.apk");
			install_apk("/data/magisk.apk");
		}
	}
}
