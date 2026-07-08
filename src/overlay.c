#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>

#include "../include/mctr.h"

/* ---------------------------------------------------------------------
 * mkdir_p: like `mkdir -p` -- create every missing component of path.
 * cgroupfs/sysfs never needed this (those directories already exist),
 * but /run/mctr/containers/<id>/upper genuinely doesn't until we make
 * it, component by component.
 * --------------------------------------------------------------------- */

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                fprintf(stderr, "[mctr overlay] mkdir(%s): %s\n", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        fprintf(stderr, "[mctr overlay] mkdir(%s): %s\n", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * rm_rf: recursively remove a directory tree. Used only to clean up
 * mctr's own scratch directories (upper/work/merged) after a container
 * exits -- never applied to cfg->rootfs (the shared, read-only image),
 * which this file never deletes anything from.
 * --------------------------------------------------------------------- */

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return (errno == ENOENT) ? 0 : -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }

    int rc = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[600];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (rm_rf(child) != 0) {
            rc = -1; /* keep going, report failure at the end */
        }
    }
    closedir(d);

    if (rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

/* ---------------------------------------------------------------------
 * overlay_make_id: a value both the parent and the about-to-be-cloned
 * child will agree on, computed before clone() so it's present in cfg
 * at the moment the child's memory is copied -- no pipe round-trip
 * needed for the child to learn it later.
 *
 * getpid() here is the mctr *runtime* process's own host pid (this
 * function always runs before clone(), so there's no PID namespace
 * involved yet) -- combined with a timestamp to avoid collisions across
 * back-to-back runs after pid reuse.
 * --------------------------------------------------------------------- */

int overlay_make_id(container_config_t *cfg) {
    snprintf(cfg->container_id, sizeof(cfg->container_id), "%d-%ld",
              (int)getpid(), (long)time(NULL));
    return 0;
}

/* ---------------------------------------------------------------------
 * overlay_prepare_dirs: host-side, parent, before clone(). Creates the
 * three directories OverlayFS needs:
 *   upper  -- this container's writable layer (copy-up target)
 *   work   -- OverlayFS-internal scratch space, same filesystem as upper
 *   merged -- the mountpoint; what the container will pivot_root into
 * --------------------------------------------------------------------- */

int overlay_prepare_dirs(container_config_t *cfg) {
    snprintf(cfg->overlay_base, sizeof(cfg->overlay_base), "%s/%s",
              MCTR_RUN_DIR, cfg->container_id);

    char upper[340], work[340], merged[340];
    snprintf(upper, sizeof(upper), "%s/upper", cfg->overlay_base);
    snprintf(work, sizeof(work), "%s/work", cfg->overlay_base);
    snprintf(merged, sizeof(merged), "%s/merged", cfg->overlay_base);

    if (mkdir_p(upper, 0755) != 0) return -1;
    if (mkdir_p(work, 0755) != 0) return -1;
    if (mkdir_p(merged, 0755) != 0) return -1;

    snprintf(cfg->overlay_merged, sizeof(cfg->overlay_merged), "%s", merged);
    cfg->overlay_active = 1;
    return 0;
}

/* ---------------------------------------------------------------------
 * overlay_mount: child-side, after CLONE_NEWNS + the "/" MS_PRIVATE
 * remount. Mounts the actual overlay: lowerdir is the shared, read-only
 * base image (cfg->rootfs); upperdir/workdir are this container's own
 * scratch dirs computed from container_id (identical derivation to what
 * the parent already created -- no new information needed here).
 * --------------------------------------------------------------------- */

int overlay_mount(container_config_t *cfg) {
    char upper[340], work[340], opts[1024];
    snprintf(upper, sizeof(upper), "%s/upper", cfg->overlay_base);
    snprintf(work, sizeof(work), "%s/work", cfg->overlay_base);

    snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s",
              cfg->rootfs, upper, work);

    if (mount("overlay", cfg->overlay_merged, "overlay", 0, opts) != 0) {
        fprintf(stderr, "[mctr child] mount overlay (%s): %s\n", opts, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * overlay_cleanup: parent-side, after waitpid(). By the time we get
 * here the container's mount namespace has already been destroyed (its
 * only process exited), which automatically tears down the overlay
 * mount itself. The umount2() call below is purely defensive -- e.g. in
 * case pivot_root never actually happened due to some earlier failure
 * and the mount somehow ended up visible outside the namespace -- and
 * its result is intentionally ignored in the common case where there's
 * simply nothing left to unmount.
 * --------------------------------------------------------------------- */

int overlay_cleanup(container_config_t *cfg) {
    if (!cfg->overlay_active) return 0;

    umount2(cfg->overlay_merged, MNT_DETACH); /* best-effort, ignore result */

    if (rm_rf(cfg->overlay_base) != 0) {
        fprintf(stderr, "[mctr overlay] warning: failed to fully clean up %s\n", cfg->overlay_base);
        return -1;
    }
    return 0;
}
