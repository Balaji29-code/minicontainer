#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../include/mctr.h"

/* ---------------------------------------------------------------------
 * Low-level helper: cgroup v2 control files (memory.max, cpu.max,
 * cgroup.procs, cgroup.subtree_control, ...) are all "write the whole
 * value in one write() syscall" interfaces -- the kernel parses exactly
 * what's written in a single call. Buffered I/O (fopen/fprintf) can
 * split that across multiple write()s and is unreliable here, so we use
 * raw open()/write()/close().
 * --------------------------------------------------------------------- */

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "[mctr cgroups] open(%s): %s\n", path, strerror(errno));
        return -1;
    }

    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);

    if (n < 0 || (size_t)n != len) {
        fprintf(stderr, "[mctr cgroups] write(%s, \"%s\"): %s\n", path, value, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * cgroup_init: make sure /sys/fs/cgroup/mctr exists and that its
 * children are allowed to use the memory and cpu controllers.
 *
 * cgroup v2's unified hierarchy requires controllers to be explicitly
 * handed down: writing "+memory +cpu" into a cgroup's
 * cgroup.subtree_control says "my child cgroups may enable these
 * controllers for themselves". Without this step, creating memory.max
 * or cpu.max files would fail with ENOENT/EPERM in the child cgroup
 * we create per container.
 * --------------------------------------------------------------------- */

int cgroup_init(void) {
    if (mkdir(MCTR_CGROUP_PARENT, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[mctr cgroups] mkdir(%s): %s\n", MCTR_CGROUP_PARENT, strerror(errno));
        fprintf(stderr, "[mctr cgroups] (cgroups v2 usually needs root -- are you running with sudo?)\n");
        return -1;
    }

    char subtree_control[300];
    snprintf(subtree_control, sizeof(subtree_control), "%s/cgroup.subtree_control", MCTR_CGROUP_PARENT);

    /* Enabling a controller that's already enabled is harmless, so we
     * don't bother checking current state first. */
    if (write_file(subtree_control, "+memory") != 0) return -1;
    if (write_file(subtree_control, "+cpu") != 0) return -1;

    return 0;
}

int cgroup_create_for_pid(pid_t pid, container_config_t *cfg) {
    snprintf(cfg->cgroup_path, sizeof(cfg->cgroup_path), "%s/%d", MCTR_CGROUP_PARENT, (int)pid);

    if (mkdir(cfg->cgroup_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[mctr cgroups] mkdir(%s): %s\n", cfg->cgroup_path, strerror(errno));
        return -1;
    }

    cfg->cgroup_active = 1;
    return 0;
}

int cgroup_apply_limits(container_config_t *cfg) {
    if (!cfg->cgroup_active) return -1;

    char path[320];
    char value[64];

    if (cfg->memory_limit_bytes > 0) {
        snprintf(path, sizeof(path), "%s/memory.max", cfg->cgroup_path);
        snprintf(value, sizeof(value), "%ld", cfg->memory_limit_bytes);
        if (write_file(path, value) != 0) return -1;
        printf("[mctr] memory limit set: %ld bytes\n", cfg->memory_limit_bytes);
    }

    if (cfg->cpu_limit_cores > 0.0) {
        /* cpu.max takes "<quota> <period>" in microseconds: the cgroup
         * may run for `quota` out of every `period` microseconds (per
         * core it could use). A fixed 100ms period is the conventional
         * choice (it's what Docker/runc use); quota scales with the
         * requested fraction of a core. */
        long period_us = 100000;
        long quota_us = (long)(period_us * cfg->cpu_limit_cores);
        if (quota_us < 1000) quota_us = 1000; /* avoid a degenerate near-zero quota */

        snprintf(path, sizeof(path), "%s/cpu.max", cfg->cgroup_path);
        snprintf(value, sizeof(value), "%ld %ld", quota_us, period_us);
        if (write_file(path, value) != 0) return -1;
        printf("[mctr] cpu limit set: %.2f cores (quota=%ld period=%ld us)\n",
               cfg->cpu_limit_cores, quota_us, period_us);
    }

    return 0;
}

int cgroup_add_pid(container_config_t *cfg, pid_t pid) {
    if (!cfg->cgroup_active) return -1;

    char path[320];
    char value[32];
    snprintf(path, sizeof(path), "%s/cgroup.procs", cfg->cgroup_path);
    snprintf(value, sizeof(value), "%d", (int)pid);

    if (write_file(path, value) != 0) return -1;
    return 0;
}

int cgroup_cleanup(container_config_t *cfg) {
    if (!cfg->cgroup_active) return 0;

    /* The cgroup must have no processes left in it before rmdir will
     * succeed; by the time we call this the container has already
     * exited (we call it after waitpid), so this is normally just
     * housekeeping. A transient EBUSY here usually means a stray
     * grandchild process outlived the container -- worth surfacing
     * rather than silently retrying forever. */
    if (rmdir(cfg->cgroup_path) != 0) {
        fprintf(stderr, "[mctr cgroups] rmdir(%s): %s\n", cfg->cgroup_path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * parse_size: "100m" -> 104857600, "512k" -> 524288, "1g" -> 1073741824,
 * "2048" (no suffix) -> 2048. Uses 1024-based (binary) multipliers to
 * match what memory.max expects (bytes), and what users mean by "100m"
 * the same way `docker run --memory 100m` does.
 * --------------------------------------------------------------------- */

long parse_size(const char *s) {
    if (!s || !*s) return -1;

    char *end = NULL;
    double val = strtod(s, &end);
    if (end == s || val < 0) return -1;

    long mult = 1;
    if (*end != '\0') {
        switch (tolower((unsigned char)*end)) {
            case 'k': mult = 1024L; break;
            case 'm': mult = 1024L * 1024L; break;
            case 'g': mult = 1024L * 1024L * 1024L; break;
            default: return -1; /* unrecognized suffix */
        }
        /* only a single suffix character is accepted, e.g. "100m" not "100mb" */
        if (end[1] != '\0') return -1;
    }

    return (long)(val * (double)mult);
}
