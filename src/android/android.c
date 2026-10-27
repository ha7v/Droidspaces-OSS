/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "droidspace.h"
#include <fcntl.h>
#include <grp.h>
#include <sys/personality.h>

/* Android detection */

int is_android(void) {
  static int cached_result = -1;
  if (cached_result != -1)
    return cached_result;

  /* Priority 1: Check for recovery environment (e.g., TWRP) */
  if (access("/system/bin/recovery", F_OK) == 0) {
    cached_result = 0;
  }
  /* Priority 2: Check for core Android system markers */
  else if (access("/system/build.prop", F_OK) == 0 ||
           access("/system/bin/app_process", F_OK) == 0) {
    cached_result = 1;
  }
  /* Fallback: Not a standard Android environment */
  else {
    cached_result = 0;
  }

  return cached_result;
}

/* A 32-bit Android on a 64-bit kernel runs init as PER_LINUX32 and every
 * process inherits it, the container included, so a 64-bit rootfs sees uname
 * report armv8l and install scripts fetch 32-bit builds. Only the 64-bit
 * backend can be running on a 64-bit kernel, so it alone resets this. Call
 * while the host /system is still visible, is_android() looks there. */
void android_native_personality(void) {
#if UINTPTR_MAX > 0xffffffffu
  if (!is_android())
    return;
  int persona = personality(0xffffffff);
  if (persona != -1 && (persona & PER_MASK) == PER_LINUX32)
    personality((unsigned long)((persona & ~PER_MASK) | PER_LINUX));
#endif
}

/* Android optimizations */

void android_optimizations(int enable) {
  if (!is_android())
    return;

  if (enable) {
    ds_log("Applying Android system optimizations...");
    char *args1[] = {"cmd",
                     "device_config",
                     "put",
                     "activity_manager",
                     "max_phantom_processes",
                     "2147483647",
                     NULL};
    run_command_quiet(args1);
    char *args2[] = {"cmd", "device_config", "set_sync_disabled_for_tests",
                     "persistent", NULL};
    run_command_quiet(args2);
    char *args3[] = {"dumpsys", "deviceidle", "disable", NULL};
    run_command_quiet(args3);
  } else {
    char *args1[] = {"cmd",
                     "device_config",
                     "put",
                     "activity_manager",
                     "max_phantom_processes",
                     "32",
                     NULL};
    run_command_quiet(args1);
    char *args2[] = {"cmd", "device_config", "set_sync_disabled_for_tests",
                     "none", NULL};
    run_command_quiet(args2);
    char *args3[] = {"dumpsys", "deviceidle", "enable", NULL};
    run_command_quiet(args3);
  }
}

/* Data partition remount (for suid support) */

void android_remount_data_suid(void) {
  if (!is_android())
    return;

  ds_log("Ensuring /data is mounted with suid support...");
  /* On some Android versions, /data is mounted nosuid. We need suid for
   * sudo/su/ping within the container if it's stored on /data. */
  char *args[] = {"mount", "-o", "remount,suid", "/data", NULL};
  if (run_command_quiet(args) != 0) {
    ds_warn(
        "Failed to remount /data with suid support. su/sudo might not work.");
  }
}

/* Storage */

/* Shared storage is a FUSE mount served by MediaProvider since Android 11
 * (sdcardfs before that). The filesystem type is the test that matters: until
 * that mount exists, vold's /storage tmpfs already holds an empty
 * /storage/emulated/0 directory, so stat() and access() both succeed and a
 * bind of it silently captures an empty, RAM-backed directory. Nothing ever
 * lands there, and whatever is written to it dies with the mount. */
#ifndef FUSE_SUPER_MAGIC
#define FUSE_SUPER_MAGIC 0x65735546
#endif
#ifndef SDCARD_FS_MAGIC
#define SDCARD_FS_MAGIC 0x5dca2f04
#endif

/* Seconds to wait for MediaProvider to publish the mount. service.sh already
 * waits before autostarting; this covers a start from the CLI or the app on a
 * device that has not finished booting yet. */
#define DS_STORAGE_WAIT_SECS 30

/* 1 when path is served by real shared storage, with the type name left in
 * *fstype for the log. 0 while it is still vold's empty placeholder on the
 * /storage tmpfs, or nothing at all. Anything that is not RAM-backed is taken
 * as real: a filesystem type this build has never heard of is not a reason to
 * refuse a mount that would otherwise work. */
static int android_storage_is_live(const char *path, const char **fstype) {
  struct statfs sfs;

  if (statfs(path, &sfs) < 0) {
    *fstype = "unreachable";
    return 0;
  }

  switch ((unsigned long)sfs.f_type) {
  case TMPFS_MAGIC:
    *fstype = "tmpfs";
    return 0;
  case RAMFS_MAGIC:
    *fstype = "ramfs";
    return 0;
  case FUSE_SUPER_MAGIC:
    *fstype = "fuse";
    return 1;
  case SDCARD_FS_MAGIC:
    *fstype = "sdcardfs";
    return 1;
  default:
    *fstype = "other";
    return 1;
  }
}

/* Waits for MediaProvider's FUSE mount, called from the start path before the
 * container's boot. The wait cannot live in the boot: init's exec watchdog is
 * the same 30 seconds, and spending them there reports a healthy container as
 * failed to boot. Returns 1 when storage is there, 0 when it never showed up. */
int android_wait_for_storage(void) {
  const char *storage_src = "/storage/emulated/0";
  const char *fstype = "unknown";
  int waited = 0;

  if (!is_android())
    return 1;

  while (!android_storage_is_live(storage_src, &fstype)) {
    if (waited == 0)
      ds_log("Android storage is not mounted yet (%s at %s), waiting up to "
             "%ds...",
             fstype, storage_src, DS_STORAGE_WAIT_SECS);
    if (waited >= DS_STORAGE_WAIT_SECS) {
      ds_warn("Android storage at %s is still %s after %ds: MediaProvider has "
              "not published its FUSE mount. Starting without it; start the "
              "container again once the phone has finished booting.",
              storage_src, fstype, DS_STORAGE_WAIT_SECS);
      return 0;
    }
    sleep(1);
    waited++;
  }
  if (waited)
    ds_log("Android storage is ready (%s, waited %ds)", fstype, waited);

  return 1;
}

int android_setup_storage(const char *rootfs_path) {
  if (!is_android()) {
    return 0;
  }

  if (!rootfs_path) {
    ds_warn("android_setup_storage called with NULL rootfs_path");
    return -1;
  }

  const char *storage_src = "/storage/emulated/0";
  const char *fstype = "unknown";
  struct stat st;

  /* The wait happened in the start path (android_wait_for_storage). What is at
   * the path now is either the real mount or vold's placeholder on the tmpfs
   * shadow, and the placeholder must not be bound: it answers stat() and
   * access(), so the container would carry an empty RAM-backed directory that
   * never picks up the real storage, and say nothing about it. */
  if (!android_storage_is_live(storage_src, &fstype)) {
    ds_warn("Android storage at %s is %s, not binding it: the MediaProvider "
            "mount is not there yet. /storage/emulated/0 stays empty until "
            "the container is started again.",
            storage_src, fstype);
    return -1;
  }

  if (stat(storage_src, &st) < 0 || !S_ISDIR(st.st_mode) ||
      access(storage_src, R_OK) < 0) {
    ds_warn("Android storage not found or not readable at %s", storage_src);
    return -1;
  }

  /* Create target directories inside rootfs: storage/, storage/emulated/,
   * storage/emulated/0 */
  char path[PATH_MAX];
  int ret;

  ret = snprintf(path, sizeof(path), "%s/storage", rootfs_path);
  if (ret < 0 || (size_t)ret >= sizeof(path))
    return -1;
  if (mkdir(path, 0755) < 0 && errno != EEXIST)
    return -1;

  ret = snprintf(path, sizeof(path), "%s/storage/emulated", rootfs_path);
  if (ret < 0 || (size_t)ret >= sizeof(path))
    return -1;
  if (mkdir(path, 0755) < 0 && errno != EEXIST)
    return -1;

  ret = snprintf(path, sizeof(path), "%s/storage/emulated/0", rootfs_path);
  if (ret < 0 || (size_t)ret >= sizeof(path))
    return -1;
  if (mkdir(path, 0755) < 0 && errno != EEXIST)
    return -1;

  ds_log("Mounting Android internal storage to /storage/emulated/0...");
  if (mount(storage_src, path, NULL, MS_BIND | MS_REC, NULL) < 0) {
    ds_warn("Failed to bind-mount Android storage %s -> %s: %s", storage_src,
            path, strerror(errno));
    return -1;
  }

  return 0;
}

/* SELinux + Termux privilege helpers
 *
 * Shared by x11.c and pulseaudio-android.c.  Android-specific. */

/* SELinux domains to try -- untrusted_app_27 first (matches real Termux) */
static const char *const untrusted_domains[] = {
    "u:r:untrusted_app_27", "u:r:untrusted_app_30", "u:r:untrusted_app_29",
    "u:r:untrusted_app_25", "u:r:untrusted_app_32", "u:r:untrusted_app",
};

/*
 * Extract MLS categories from a full SELinux context string.
 * e.g. "u:object_r:app_data_file:s0:c80,c257,c512,c768"
 *                                    ^ returned pointer
 */
const char *ds_extract_mls(const char *ctx) {
  int colons = 0;
  for (const char *p = ctx; *p; p++)
    if (*p == ':' && ++colons == 3)
      return p + 1;
  return NULL;
}

/*
 * Transition the calling process into an untrusted_app SELinux domain
 * with the given MLS categories.  Tries untrusted_app_27 first to match
 * the real Termux process context.
 * Also clears /proc/self/attr/exec to prevent label leaking.
 */
void ds_selinux_dyntransition(const char *mls) {
  char target[256] = "";
  int fd = open("/proc/self/attr/current", O_WRONLY | O_CLOEXEC);
  if (fd >= 0) {
    for (size_t i = 0;
         i < sizeof(untrusted_domains) / sizeof(untrusted_domains[0]); i++) {
      snprintf(target, sizeof(target), "%s:%s", untrusted_domains[i], mls);
      if (write(fd, target, strlen(target) + 1) > 0)
        break;
    }
    close(fd);
  }

  /* Clear any stale exec context to prevent label leaking across execve */
  fd = open("/proc/self/attr/exec", O_WRONLY | O_CLOEXEC);
  if (fd >= 0) {
    if (write(fd, "\0", 1) < 0) { /* ignore */
    }
    close(fd);
  }
}

/*
 * Drop to the given UID with standard Termux Android supplementary groups.
 * Returns 0 on success, -1 on failure.
 */
int ds_drop_privileges(int uid) {
  if (setresgid(uid, uid, uid) < 0 || setresuid(uid, uid, uid) < 0)
    return -1;
  return 0;
}

/*
 * Resolve Termux UID from /data/system/packages.list.
 * Returns the UID on success, -1 on failure.
 */
int ds_resolve_termux_uid(void) {
  FILE *f = fopen(TX11_PACKAGES, "r");
  if (!f) {
    ds_warn("[Android] cannot open packages.list (%s)", TX11_PACKAGES);
    return -1;
  }

  char line[1024];
  int uid = -1;

  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "com.termux ", 11) == 0) {
      uid = atoi(line + 11);
      break;
    }
  }
  fclose(f);

  if (uid < 0)
    ds_warn("[Android] com.termux not found in packages.list");
  return uid;
}
