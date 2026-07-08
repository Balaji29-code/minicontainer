#define _GNU_SOURCE
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/limits.h>

#include "../include/mctr.h"

/* ---------------------------------------------------------------------
 * Parent <-> child synchronization
 *
 * clone() returns to the PARENT immediately with the child's pid; the
 * child starts running container_child() concurrently. We don't want
 * the child to touch mounts/hostname/exec until the parent has finished
 * any host-side setup for this child (in later milestones: creating the
 * cgroup and moving network devices). A pipe gives us a simple one-shot
 * gate: the child blocks on read() until the parent writes a byte.
 * --------------------------------------------------------------------- */

static int wait_for_signal(int read_fd) {
    char buf;
    ssize_t n = read(read_fd, &buf, 1);
    close(read_fd);
    return (n == 1) ? 0 : -1;
}

static void signal_child(int write_fd) {
    char buf = 'X';
    if (write(write_fd, &buf, 1) != 1) {
        perror("write sync pipe");
    }
    close(write_fd);
}

/* ---------------------------------------------------------------------
 * pivot_root: swap the mount namespace's root from the host's "/" to
 * new_root, then detach and discard the old root entirely.
 *
 * Why not chroot()? chroot() only changes what a process *resolves* "/"
 * to -- the old root mount is still present in the mount table and can
 * be escaped (e.g. via a leaked fd or ".."  tricks combined with other
 * mounts). pivot_root() actually moves the root of the mount tree, so
 * once the old root is unmounted there is no path back to it at all.
 *
 * pivot_root(2) requires its first argument to BE a mount point (not
 * just a directory), so we bind-mount new_root onto itself first --
 * a bind mount of a directory onto itself is the standard trick to
 * satisfy that requirement without needing a separate filesystem.
 * --------------------------------------------------------------------- */

static int pivot_root_into(const char *new_root) {
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("pivot_root_into: bind-mount new_root onto itself");
        return -1;
    }

    char old_root_path[PATH_MAX];
    snprintf(old_root_path, sizeof(old_root_path), "%s/.old_root", new_root);
    if (mkdir(old_root_path, 0700) != 0 && errno != EEXIST) {
        perror("pivot_root_into: mkdir .old_root");
        return -1;
    }

    if (chdir(new_root) != 0) {
        perror("pivot_root_into: chdir(new_root)");
        return -1;
    }

    /* pivot_root has no glibc wrapper on most systems; call the raw syscall. */
    if (syscall(SYS_pivot_root, ".", ".old_root") != 0) {
        perror("pivot_root_into: pivot_root syscall");
        return -1;
    }

    if (chdir("/") != 0) {
        perror("pivot_root_into: chdir(/) after pivot_root");
        return -1;
    }

    /* The host's old root is now mounted at /.old_root inside the new
     * namespace. Detach it (MNT_DETACH = lazy unmount, fine since
     * nothing should still be using it) and remove the mountpoint dir. */
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        perror("pivot_root_into: umount2(.old_root)");
        /* not fatal -- continue, but the host root remains reachable,
         * which is a real isolation gap worth flagging in test notes */
    }
    if (rmdir("/.old_root") != 0) {
        perror("pivot_root_into: rmdir .old_root");
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * Runs inside the new PID/UTS/Mount namespaces (clone() already placed
 * us there before this function starts). This is effectively the
 * container's "init" -- PID 1 inside its own PID namespace.
 * --------------------------------------------------------------------- */

static int container_child(void *arg) {
    container_config_t *cfg = (container_config_t *)arg;

    close(cfg->sync_pipe[1]); /* child only reads */
    if (wait_for_signal(cfg->sync_pipe[0]) != 0) {
        fprintf(stderr, "[mctr child] sync with parent failed\n");
        _exit(1);
    }

    /* UTS namespace: this only affects us, the host hostname is untouched. */
    if (sethostname(cfg->hostname, strlen(cfg->hostname)) != 0) {
        perror("[mctr child] sethostname");
    }

    /* Mount namespace: cut mount propagation to/from the host BEFORE we
     * create any new mounts, recursively (MS_REC) so it applies to
     * every mount under "/", not just the top-level one. Skipping this
     * would leak our pivot_root and /proc mount back into the host's
     * mount table. */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("[mctr child] mount MS_PRIVATE");
        _exit(1);
    }

    /* Copy-on-write filesystem: if the parent successfully prepared
     * overlay scratch dirs (cfg->overlay_active, set before clone()),
     * mount the actual overlay here -- inside our own private mount
     * namespace, so it's invisible to the host and to any other
     * container sharing the same base image -- and pivot into the
     * merged view instead of the raw image. If overlay setup wasn't
     * available, fall back to pivoting straight into the image (the
     * milestone-1 behavior); this means writes would land directly in
     * the shared image with no isolation, which is exactly the
     * trade-off OverlayFS exists to avoid, so we warn loudly. */
    const char *pivot_target = cfg->rootfs;
    if (cfg->overlay_active) {
        if (overlay_mount(cfg) == 0) {
            pivot_target = cfg->overlay_merged;
        } else {
            fprintf(stderr,
                "[mctr child] overlay mount failed -- falling back to the base "
                "image directly (no copy-on-write; writes are NOT isolated)\n");
        }
    }

    if (pivot_root_into(pivot_target) != 0) {
        _exit(1);
    }

    /* /proc reflects the PID namespace of whoever mounts it, so it must
     * be (re)mounted fresh here -- otherwise tools like ps inside the
     * container would show the host's process tree. */
    if (mkdir("/proc", 0555) != 0 && errno != EEXIST) {
        perror("[mctr child] mkdir /proc");
    }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("[mctr child] mount /proc");
    }

    /* Networking: by now we're inside our own net namespace (CLONE_NEWNET,
     * set at clone() time) and -- if the parent's network_create_veth()
     * succeeded -- cfg->veth_cont already exists in it, moved in from the
     * host. Bring up lo + veth_cont, assign our IP, add a default route
     * via the bridge. Non-fatal: a container with no network still runs. */
    if (network_configure_container_side(cfg) != 0) {
        fprintf(stderr, "[mctr child] warning: network configuration failed -- "
                "container will have no network connectivity\n");
    }

    execvp(cfg->argv[0], cfg->argv);
    /* execvp only returns on failure */
    perror("[mctr child] execvp");
    _exit(127);
}

int run_container(container_config_t *cfg) {
    /* Copy-on-write filesystem setup happens here, BEFORE clone(), so
     * the container_id and overlay paths computed by the parent are
     * already present in the struct the child receives a copy of at
     * clone() time -- no further parent->child communication needed
     * for the child to later mount the overlay itself. A failure here
     * is non-fatal: the container still runs, just pivoted directly
     * into the shared base image instead of a private overlay. */
    overlay_make_id(cfg);
    if (overlay_prepare_dirs(cfg) != 0) {
        fprintf(stderr,
            "[mctr] warning: overlay setup failed -- running directly against "
            "the base image (no copy-on-write isolation)\n");
    }

    /* Networking: name/IP computation only, no syscalls yet -- must happen
     * before clone() so the child's copy of cfg already has the names it
     * needs to configure its own interface later. network_init_bridge()
     * is the one side-effecting call here; it touches shared host state
     * (the mctr0 bridge), not anything specific to this container, so it
     * doesn't need to be ordered relative to clone() at all -- doing it
     * here just keeps all pre-flight setup in one place. */
    network_prepare(cfg);
    if (network_init_bridge() != 0) {
        fprintf(stderr, "[mctr] warning: bridge setup failed -- network may not work\n");
    }

    if (pipe(cfg->sync_pipe) != 0) {
        perror("run_container: pipe");
        return -1;
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("run_container: malloc stack");
        return -1;
    }
    char *stack_top = stack + STACK_SIZE; /* stack grows down */

    /* CLONE_NEWPID  -- new, independent process-ID space; child becomes PID 1 in it
     * CLONE_NEWUTS  -- new hostname/domainname
     * CLONE_NEWNS   -- new mount table (a copy of the parent's, then made private)
     * CLONE_NEWNET  -- new network stack: its own interfaces (just lo, until we move
     *                  a veth in), routing table, iptables rules
     * SIGCHLD       -- clone() requires an explicit exit signal; SIGCHLD matches fork() semantics
     */
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD;

    pid_t pid = clone(container_child, stack_top, flags, cfg);
    if (pid == -1) {
        perror("run_container: clone");
        free(stack);
        close(cfg->sync_pipe[0]);
        close(cfg->sync_pipe[1]);
        return -1;
    }

    close(cfg->sync_pipe[0]); /* parent only writes */

    /* Resource limits, host side: create this container's cgroup, write
     * memory.max / cpu.max, then enroll the child's pid -- all BEFORE
     * signal_child() releases it to exec. If we enrolled the pid after
     * letting the child run, there'd be a window where it executes
     * completely unconstrained; doing it here closes that window. A
     * failure here is treated as a warning, not fatal -- the container
     * still runs, just without enforced limits (e.g. if cgroups v2
     * isn't available or we're not root). */
    if (cgroup_init() == 0 && cgroup_create_for_pid(pid, cfg) == 0) {
        if (cgroup_apply_limits(cfg) != 0) {
            fprintf(stderr, "[mctr] warning: failed to apply one or more resource limits\n");
        }
        if (cgroup_add_pid(cfg, pid) != 0) {
            fprintf(stderr, "[mctr] warning: failed to enroll container into its cgroup\n");
        }
    } else if (cfg->memory_limit_bytes > 0 || cfg->cpu_limit_cores > 0.0) {
        fprintf(stderr, "[mctr] warning: cgroup setup failed -- running WITHOUT resource limits\n");
    }

    /* Networking, host side: create the veth pair, attach the host end to
     * the bridge, and move the container end into the child's netns -- all
     * using the real pid clone() just returned. Must happen BEFORE
     * signal_child(), same reasoning as cgroup enrollment above: otherwise
     * there's a window where the child could exec before its network
     * interface exists. A failure here is a warning, not fatal -- the
     * container still runs, just without connectivity. */
    if (network_create_veth(pid, cfg) != 0) {
        fprintf(stderr, "[mctr] warning: failed to set up networking for this container\n");
    } else {
        /* Printed now, before signal_child() hands control to the
         * container's command, so it's visible right as an interactive
         * session starts -- giving the user something to ping/curl from a
         * second host terminal while this shell is still running. */
        printf("[mctr] container network: %s/16 via %s (gateway %s, host veth %s)\n",
               cfg->container_ip, MCTR_BRIDGE_NAME, MCTR_BRIDGE_IP, cfg->veth_host);
    }

    signal_child(cfg->sync_pipe[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        perror("run_container: waitpid");
        free(stack);
        return -1;
    }

    /* The container has fully exited, so its cgroup is now empty and
     * safe to remove, and its mount namespace (and the overlay mount
     * inside it) is already gone -- this just clears the scratch dirs
     * left behind on disk. */
    cgroup_cleanup(cfg);
    overlay_cleanup(cfg);
    network_cleanup(cfg);

    free(stack);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "[mctr] container killed by signal %d\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return -1;
}
