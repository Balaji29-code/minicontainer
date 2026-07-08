#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "../include/mctr.h"

/* ---------------------------------------------------------------------
 * We build veth pairs, the bridge, and the NAT rule by shelling out to
 * the `ip` (iproute2) and `iptables` command-line tools rather than
 * talking raw rtnetlink ourselves. Driving these via fork()+execvp() of
 * fixed argv arrays (never a shell string) gets us the real kernel
 * operations -- the same ones `docker0` and container veths rely on --
 * without re-implementing a netlink client from scratch, which is a
 * substantial project on its own and orthogonal to what this milestone
 * is meant to demonstrate (namespaces + a virtual switch + NAT).
 *
 * run_argv_ex() is the one place we fork/exec/wait; everything else is
 * just building argv arrays for it. `quiet` redirects the child's
 * stdout/stderr to /dev/null -- used for "does this already exist?"
 * checks where a non-zero exit is an expected, normal outcome, not an
 * error worth printing.
 * --------------------------------------------------------------------- */

static int run_argv_ex(char *const argv[], int quiet) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[mctr net] fork");
        return -1;
    }

    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(argv[0], argv);
        if (!quiet) {
            fprintf(stderr, "[mctr net] execvp(%s): %s\n", argv[0], strerror(errno));
        }
        _exit(127);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("[mctr net] waitpid");
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    return -1;
}

static int run_argv(char *const argv[]) {
    return run_argv_ex(argv, 0);
}

static int run_argv_quiet(char *const argv[]) {
    return run_argv_ex(argv, 1);
}

/* ---------------------------------------------------------------------
 * write_file: same single-write()-syscall pattern as cgroups.c, kept
 * local here rather than shared since /proc/sys/net/ipv4/ip_forward is
 * the only file network.c ever writes directly (everything else goes
 * through `ip`/`iptables`).
 * --------------------------------------------------------------------- */

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "[mctr net] open(%s): %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    if (n < 0 || (size_t)n != len) {
        fprintf(stderr, "[mctr net] write(%s): %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * network_prepare: PARENT-side, BEFORE clone(). Pure computation --
 * picks the veth interface names and the container's IP address, all
 * derived from this mctr process's own pid (host_pid), so both the
 * parent and the about-to-be-cloned child agree on them with no further
 * IPC, exactly like overlay_make_id()'s container_id.
 *
 * IFNAMSIZ is 16 bytes including the NUL, so names must stay short:
 * "veth" + up to 7 pid digits + "h"/"c" comfortably fits.
 *
 * The IP is derived from host_pid mod 65000 (+2, to skip the network
 * address .0.0 and the bridge's own .0.1), split into the low two
 * octets of the /16 subnet. Good enough to avoid collisions across
 * back-to-back test runs on one machine; not a real allocator.
 * --------------------------------------------------------------------- */

int network_prepare(container_config_t *cfg) {
    cfg->host_pid = (int)getpid();

    snprintf(cfg->veth_host, sizeof(cfg->veth_host), "veth%dh", cfg->host_pid);
    snprintf(cfg->veth_cont, sizeof(cfg->veth_cont), "veth%dc", cfg->host_pid);

    int host_part = (cfg->host_pid % 65000) + 2;
    int hi = (host_part >> 8) & 0xFF;
    int lo = host_part & 0xFF;
    snprintf(cfg->container_ip, sizeof(cfg->container_ip), "172.18.%d.%d", hi, lo);

    return 0;
}

/* ---------------------------------------------------------------------
 * network_init_bridge: idempotent, host-side. Plays the role of
 * `docker0` -- a single software switch every container's veth
 * attaches to, which also doubles as the container subnet's default
 * gateway (MCTR_BRIDGE_IP). Safe to call on every `mctr run`; only
 * actually creates anything the first time.
 * --------------------------------------------------------------------- */

int network_init_bridge(void) {
    char *check[] = {"ip", "link", "show", MCTR_BRIDGE_NAME, NULL};
    if (run_argv_quiet(check) != 0) {
        char *add[] = {"ip", "link", "add", "name", MCTR_BRIDGE_NAME, "type", "bridge", NULL};
        if (run_argv(add) != 0) {
            fprintf(stderr, "[mctr net] failed to create bridge %s\n", MCTR_BRIDGE_NAME);
            return -1;
        }
        char *addr[] = {"ip", "addr", "add", MCTR_BRIDGE_CIDR, "dev", MCTR_BRIDGE_NAME, NULL};
        if (run_argv(addr) != 0) {
            fprintf(stderr, "[mctr net] failed to assign %s to %s\n", MCTR_BRIDGE_CIDR, MCTR_BRIDGE_NAME);
            return -1;
        }
    }

    char *up[] = {"ip", "link", "set", MCTR_BRIDGE_NAME, "up", NULL};
    if (run_argv(up) != 0) {
        fprintf(stderr, "[mctr net] failed to bring %s up\n", MCTR_BRIDGE_NAME);
        return -1;
    }

    /* Without this, the kernel will never forward packets from the
     * container subnet out through the host's real interface -- NAT
     * rules alone aren't enough. */
    if (write_file("/proc/sys/net/ipv4/ip_forward", "1") != 0) {
        fprintf(stderr, "[mctr net] failed to enable ip_forward\n");
        return -1;
    }

    /* iptables has no "add if missing" mode, so we check first (-C) and
     * only append (-A) if the check fails -- otherwise re-running mctr
     * would stack up duplicate identical rules over time. */
    char *check_rule[] = {
        "iptables", "-t", "nat", "-C", "POSTROUTING",
        "-s", MCTR_SUBNET_CIDR, "!", "-o", MCTR_BRIDGE_NAME, "-j", "MASQUERADE", NULL
    };
    if (run_argv_quiet(check_rule) != 0) {
        char *add_rule[] = {
            "iptables", "-t", "nat", "-A", "POSTROUTING",
            "-s", MCTR_SUBNET_CIDR, "!", "-o", MCTR_BRIDGE_NAME, "-j", "MASQUERADE", NULL
        };
        if (run_argv(add_rule) != 0) {
            fprintf(stderr, "[mctr net] failed to add MASQUERADE rule for %s\n", MCTR_SUBNET_CIDR);
            return -1;
        }
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * network_create_veth: PARENT-side, AFTER clone(). child_pid is the
 * real host pid the kernel just handed back -- the only piece of
 * information this whole milestone needs that couldn't be computed
 * before clone(). Everything else (the names) came from cfg, already
 * agreed with the child via network_prepare().
 * --------------------------------------------------------------------- */

int network_create_veth(pid_t child_pid, container_config_t *cfg) {
    char *add[] = {
        "ip", "link", "add", cfg->veth_host, "type", "veth",
        "peer", "name", cfg->veth_cont, NULL
    };
    if (run_argv(add) != 0) {
        fprintf(stderr, "[mctr net] failed to create veth pair %s/%s\n", cfg->veth_host, cfg->veth_cont);
        return -1;
    }

    char *master[] = {"ip", "link", "set", cfg->veth_host, "master", MCTR_BRIDGE_NAME, NULL};
    if (run_argv(master) != 0) {
        fprintf(stderr, "[mctr net] failed to attach %s to %s\n", cfg->veth_host, MCTR_BRIDGE_NAME);
        return -1;
    }

    char *up[] = {"ip", "link", "set", cfg->veth_host, "up", NULL};
    if (run_argv(up) != 0) {
        fprintf(stderr, "[mctr net] failed to bring %s up\n", cfg->veth_host);
        return -1;
    }

    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d", (int)child_pid);
    char *move[] = {"ip", "link", "set", cfg->veth_cont, "netns", pidbuf, NULL};
    if (run_argv(move) != 0) {
        fprintf(stderr, "[mctr net] failed to move %s into pid %d's netns\n", cfg->veth_cont, (int)child_pid);
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------
 * network_configure_container_side: CHILD-side. By the time the child
 * is signaled to run this, it is already living inside its own network
 * namespace (CLONE_NEWNET, set at clone() time) and -- if the parent's
 * network_create_veth() succeeded -- cfg->veth_cont already exists in
 * that namespace, moved in from the host. Interface names are NOT
 * translated by namespaces, so the child can refer to it by the exact
 * same name the parent used.
 *
 * If the parent's step never ran or failed, these `ip` invocations
 * simply fail (e.g. "Cannot find device veth1234c") and we warn and
 * continue -- the container still runs, just with no network, the same
 * graceful-degradation pattern as cgroups/overlay.
 * --------------------------------------------------------------------- */

int network_configure_container_side(container_config_t *cfg) {
    int rc = 0;

    char *lo_up[] = {"ip", "link", "set", "lo", "up", NULL};
    if (run_argv(lo_up) != 0) rc = -1;

    char *cont_up[] = {"ip", "link", "set", cfg->veth_cont, "up", NULL};
    if (run_argv(cont_up) != 0) rc = -1;

    char addr_cidr[40];
    snprintf(addr_cidr, sizeof(addr_cidr), "%s/16", cfg->container_ip);
    char *addr[] = {"ip", "addr", "add", addr_cidr, "dev", cfg->veth_cont, NULL};
    if (run_argv(addr) != 0) rc = -1;

    char *route[] = {"ip", "route", "add", "default", "via", MCTR_BRIDGE_IP, NULL};
    if (run_argv(route) != 0) rc = -1;

    return rc;
}

/* ---------------------------------------------------------------------
 * network_cleanup: PARENT-side, after waitpid(). Removing one end of a
 * veth pair removes both, and the kernel already does that automatically
 * when the container's network namespace (holding veth_cont) is torn
 * down on process exit -- so in the common case there's nothing left
 * here at all. This is purely defensive, same role as overlay_cleanup()'s
 * umount2() call; failure is expected and ignored.
 * --------------------------------------------------------------------- */

int network_cleanup(container_config_t *cfg) {
    char *del[] = {"ip", "link", "del", cfg->veth_host, NULL};
    run_argv_quiet(del);
    return 0;
}
