#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <vector>

#include <magisk.hpp>
#include <magiskpolicy.hpp>
#include <utils.hpp>

#include "init.hpp"
#include "magiskrc.inc"

#ifdef USE_64BIT
#define LIBNAME "lib64"
#else
#define LIBNAME "lib"
#endif

using namespace std;

static void patch_socket_name(const char *path) {
	char *buf;
	size_t size;
	mmap_rw(path, buf, size);
	for (int i = 0; i < size; ++i) {
		if (memcmp(buf + i, MAIN_SOCKET, sizeof(MAIN_SOCKET)) == 0) {
			gen_rand_str(buf + i, 16);
			i += sizeof(MAIN_SOCKET);
		}
	}
	munmap(buf, size);
}

static vector<raw_data> rc_list;

static void patch_init_rc(FILE *rc) {
	file_readline("/init.rc", [=](string_view line) -> bool {
		// Do not start vaultkeeper
		if (str_contains(line, "start vaultkeeper")) {
			LOGD("Remove vaultkeeper\n");
			return true;
		}
		// Do not run flash_recovery
		if (str_starts(line, "service flash_recovery")) {
			LOGD("Remove flash_recovery\n");
			fprintf(rc, "service flash_recovery /system/bin/xxxxx\n");
			return true;
		}
		// Else just write the line
		fprintf(rc, "%s", line.data());
		return true;
	});

	fprintf(rc, "\n");

	// Inject custom rc scripts
	for (auto &d : rc_list)
		fprintf(rc, "\n%s\n", d.buf);
	rc_list.clear();

	// Inject Magisk rc scripts
	char pfd_svc[16], ls_svc[16], bc_svc[16];
	gen_rand_str(pfd_svc, sizeof(pfd_svc));
	gen_rand_str(ls_svc, sizeof(ls_svc));
	gen_rand_str(bc_svc, sizeof(bc_svc));
	LOGD("Inject magisk services: [%s] [%s] [%s]\n", pfd_svc, ls_svc, bc_svc);
	fprintf(rc, magiskrc, pfd_svc, pfd_svc, ls_svc, bc_svc, bc_svc);
}

static void load_overlay_rc(const char *overlay) {
	auto dir = open_dir(overlay);
	if (!dir) return;

	int dfd = dirfd(dir.get());
	// Do not allow overwrite init.rc
	unlinkat(dfd, "init.rc", 0);
	for (dirent *entry; (entry = readdir(dir.get()));) {
		if (strend(entry->d_name, ".rc") == 0) {
			LOGD("Found rc script [%s]\n", entry->d_name);
			int rc = xopenat(dfd, entry->d_name, O_RDONLY | O_CLOEXEC);
			raw_data data;
			fd_full_read(rc, data.buf, data.sz);
			close(rc);
			rc_list.push_back(std::move(data));
			unlinkat(dfd, entry->d_name, 0);
		}
	}
}

void RootFSInit::setup_rootfs() {
	if (patch_sepolicy()) {
		char *addr;
		size_t size;
		mmap_rw("/init", addr, size);
		for (char *p = addr; p < addr + size; ++p) {
			if (memcmp(p, SPLIT_PLAT_CIL, sizeof(SPLIT_PLAT_CIL)) == 0) {
				// Force init to load /sepolicy
				LOGD("Remove from init: " SPLIT_PLAT_CIL "\n");
				memset(p, 'x', sizeof(SPLIT_PLAT_CIL) - 1);
				break;
			}
		}
		munmap(addr, size);
	}

	// Handle overlays
	if (access("/overlay.d", F_OK) == 0) {
		LOGD("Merge overlay.d\n");
		load_overlay_rc("/overlay.d");
		mv_path("/overlay.d", "/");
	}

	// Patch init.rc
	FILE *rc = xfopen("/init.p.rc", "we");
	patch_init_rc(rc);
	fclose(rc);
	clone_attr("/init.rc", "/init.p.rc");
	rename("/init.p.rc", "/init.rc");

	// Create hardlink mirror of /sbin to /root
	mkdir("/root", 0750);
	clone_attr("/sbin", "/root");
	link_path("/sbin", "/root");

	// Dump magiskinit as magisk
	int fd = xopen("/sbin/magisk", O_WRONLY | O_CREAT, 0755);
	write(fd, self.buf, self.sz);
	close(fd);
}

bool MagiskInit::patch_sepolicy(const char *file) {
	bool patch_init = false;

	if (access(SPLIT_PLAT_CIL, R_OK) == 0) {
		LOGD("sepol: split policy\n");
		patch_init = true;
	} else if (access("/sepolicy", R_OK) == 0) {
		LOGD("sepol: monolithic policy\n");
		load_policydb("/sepolicy");
	} else {
		LOGD("sepol: no selinux\n");
		return false;
	}

	// Mount selinuxfs to communicate with kernel
	xmount("selinuxfs", SELINUX_MNT, "selinuxfs", 0, nullptr);
	mount_list.emplace_back(SELINUX_MNT);

	if (patch_init)
		load_split_cil();

	sepol_magisk_rules();
	sepol_allow(SEPOL_PROC_DOMAIN, ALL, ALL, ALL);

	// Custom rules
	if (auto dir = xopen_dir(persist_dir); dir) {
		char path[4096];
		for (dirent *entry; (entry = xreaddir(dir.get()));) {
			if (entry->d_name == "."sv || entry->d_name == ".."sv)
				continue;
			snprintf(path, sizeof(path), "%s/%s/sepolicy.rule", persist_dir, entry->d_name);
			if (access(path, R_OK) == 0) {
				LOGD("Loading custom sepolicy patch: %s\n", path);
				load_rule_file(path);
			}
		}
	}

	dump_policydb(file);
	destroy_policydb();

	// Remove OnePlus stupid debug sepolicy and use our own
	if (access("/sepolicy_debug", F_OK) == 0) {
		unlink("/sepolicy_debug");
		link("/sepolicy", "/sepolicy_debug");
	}

	return patch_init;
}

static void sbin_overlay(const raw_data &self, const raw_data &config) {
	mount_sbin();

	// Dump binaries
	xmkdir(MAGISKTMP, 0755);
	int fd = xopen(MAGISKTMP "/config", O_WRONLY | O_CREAT, 0000);
	xwrite(fd, config.buf, config.sz);
	close(fd);
	fd = xopen("/sbin/magiskinit", O_WRONLY | O_CREAT, 0755);
	xwrite(fd, self.buf, self.sz);
	close(fd);
	dump_magisk("/sbin/magisk", 0755);
	patch_socket_name("/sbin/magisk");

	// Create applet symlinks
	char path[64];
	for (int i = 0; applet_names[i]; ++i) {
		sprintf(path, "/sbin/%s", applet_names[i]);
		xsymlink("./magisk", path);
	}
	xsymlink("./magiskinit", "/sbin/magiskpolicy");
	xsymlink("./magiskinit", "/sbin/supolicy");
}

static void recreate_sbin(const char *mirror, bool use_bind_mount) {
	auto dp = xopen_dir(mirror);
	int src = dirfd(dp.get());
	char buf[4096];
	for (dirent *entry; (entry = xreaddir(dp.get()));) {
		if (entry->d_name == "."sv || entry->d_name == ".."sv)
			continue;
		string sbin_path = "/sbin/"s + entry->d_name;
		struct stat st;
		fstatat(src, entry->d_name, &st, AT_SYMLINK_NOFOLLOW);
		if (S_ISLNK(st.st_mode)) {
			xreadlinkat(src, entry->d_name, buf, sizeof(buf));
			xsymlink(buf, sbin_path.data());
		} else {
			sprintf(buf, "%s/%s", mirror, entry->d_name);
			if (use_bind_mount) {
				auto mode = st.st_mode & 0777;
				// Create dummy
				if (S_ISDIR(st.st_mode))
					xmkdir(sbin_path.data(), mode);
				else
					close(xopen(sbin_path.data(), O_CREAT | O_WRONLY | O_CLOEXEC, mode));

				xmount(buf, sbin_path.data(), nullptr, MS_BIND, nullptr);
			} else {
				xsymlink(buf, sbin_path.data());
			}
		}
	}
}

#define ROOTMIR MIRRDIR "/system_root"
#define ROOTBLK BLOCKDIR "/system_root"
#define MONOPOLICY  "/sepolicy"
#define PATCHPOLICY "/sbin/.se"
#define LIBSELINUX  "/system/" LIBNAME "/libselinux.so"

static string magic_mount_list;

static void magic_mount(const string &sdir, const string &ddir = "") {
	auto dir = xopen_dir(sdir.data());
	for (dirent *entry; (entry = readdir(dir.get()));) {
		if (entry->d_name == "."sv || entry->d_name == ".."sv)
			continue;
		string src = sdir + "/" + entry->d_name;
		string dest = ddir + "/" + entry->d_name;
		if (access(dest.data(), F_OK) == 0) {
			if (entry->d_type == DT_DIR) {
				// Recursive
				magic_mount(src, dest);
			} else {
				LOGD("Mount [%s] -> [%s]\n", src.data(), dest.data());
				xmount(src.data(), dest.data(), nullptr, MS_BIND, nullptr);
				magic_mount_list += dest;
				magic_mount_list += '\n';
			}
		}
	}
}

void SARBase::patch_rootdir() {
	sbin_overlay(self, config);

	// Mount system_root mirror
	struct stat st;
	xstat("/", &st);
	xmkdir(ROOTMIR, 0755);
	mknod(ROOTBLK, S_IFBLK | 0600, st.st_dev);
	if (xmount(ROOTBLK, ROOTMIR, "ext4", MS_RDONLY, nullptr))
		xmount(ROOTBLK, ROOTMIR, "erofs", MS_RDONLY, nullptr);

	// Recreate original sbin structure
	recreate_sbin(ROOTMIR "/sbin", true);

	// Patch init
	raw_data init;
	file_attr attr;
	bool redirect = false;
	int src = xopen("/init", O_RDONLY | O_CLOEXEC);
	fd_full_read(src, init.buf, init.sz);
	fgetattr(src, &attr);
	close(src);
	uint8_t *eof = init.buf + init.sz;
	for (uint8_t *p = init.buf; p < eof; ++p) {
		if (memcmp(p, SPLIT_PLAT_CIL, sizeof(SPLIT_PLAT_CIL)) == 0) {
			// Force init to load monolithic policy
			LOGD("Remove from init: " SPLIT_PLAT_CIL "\n");
			memset(p, 'x', sizeof(SPLIT_PLAT_CIL) - 1);
			p += sizeof(SPLIT_PLAT_CIL) - 1;
		} else if (memcmp(p, MONOPOLICY, sizeof(MONOPOLICY)) == 0) {
			// Redirect /sepolicy to tmpfs
			LOGD("Patch init [" MONOPOLICY "] -> [" PATCHPOLICY "]\n");
			memcpy(p, PATCHPOLICY, sizeof(PATCHPOLICY));
			redirect = true;
			p += sizeof(MONOPOLICY) - 1;
		}
	}
	xmkdir(ROOTOVL, 0);
	int dest = xopen(ROOTOVL "/init", O_CREAT | O_WRONLY | O_CLOEXEC);
	xwrite(dest, init.buf, init.sz);
	fsetattr(dest, &attr);
	close(dest);

	// Patch libselinux
	if (!redirect) {
		raw_data lib;
		// init is dynamically linked, need to patch libselinux
		full_read(LIBSELINUX, lib.buf, lib.sz);
		getattr(LIBSELINUX, &attr);
		eof = lib.buf + lib.sz;
		for (uint8_t *p = lib.buf; p < eof; ++p) {
			if (memcmp(p, MONOPOLICY, sizeof(MONOPOLICY)) == 0) {
				// Redirect /sepolicy to tmpfs
				LOGD("Patch libselinux.so [" MONOPOLICY "] -> [" PATCHPOLICY "]\n");
				memcpy(p, PATCHPOLICY, sizeof(PATCHPOLICY));
				break;
			}
		}
		xmkdir(ROOTOVL "/system", 0755);
		xmkdir(ROOTOVL "/system/" LIBNAME, 0755);
		dest = xopen(ROOTOVL LIBSELINUX, O_CREAT | O_WRONLY | O_CLOEXEC);
		xwrite(dest, lib.buf, lib.sz);
		fsetattr(dest, &attr);
		close(dest);
	}

	// sepolicy
	patch_sepolicy(PATCHPOLICY);

	// Handle overlay
	struct sockaddr_un sun{};
	socklen_t len = setup_sockaddr(&sun);
	int socketfd = xsocket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (connect(socketfd, (struct sockaddr*) &sun, len) == 0) {
		LOGD("ACK init tracer to write backup files\n");
		int ack;
		// Wait for init tracer finish copying files
		read(socketfd, &ack, sizeof(ack));
	} else {
		LOGD("Restore backup files locally\n");
		restore_folder(ROOTOVL, overlays);
		overlays.clear();
	}
	close(socketfd);
	if (access(ROOTOVL "/sbin", F_OK) == 0) {
		file_attr a;
		getattr("/sbin", &a);
		cp_afc(ROOTOVL "/sbin", "/sbin");
		rm_rf(ROOTOVL "/sbin");
		setattr("/sbin", &a);
	}

	// Patch init.rc
	FILE *rc = xfopen(ROOTOVL "/init.rc", "we");
	patch_init_rc(rc);
	fclose(rc);
	clone_attr("/init.rc", ROOTOVL "/init.rc");

	// Mount rootdir
	magic_mount(ROOTOVL);
	dest = xopen(ROOTMNT, O_WRONLY | O_CREAT | O_CLOEXEC);
	write(dest, magic_mount_list.data(), magic_mount_list.length());
	close(dest);
}

int magisk_proxy_main(int argc, char *argv[]) {
	setup_klog();

	raw_data config;
	raw_data self;

	full_read("/sbin/magisk", self.buf, self.sz);
	full_read("/.backup/.magisk", config.buf, config.sz);

	xmount(nullptr, "/", nullptr, MS_REMOUNT, nullptr);

	unlink("/sbin/magisk");
	rm_rf("/.backup");

	sbin_overlay(self, config);

	// Create symlinks pointing back to /root
	recreate_sbin("/root", false);

	setenv("REMOUNT_ROOT", "1", 1);
	execv("/sbin/magisk", argv);

	return 1;
}
