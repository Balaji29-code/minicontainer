#ifndef MCTR_H
#define MCTR_H

#include <sys/types.h> /* pid_t */

/*
 * Shared types for the mctr container runtime.
 *
 * Milestone 1 only fills in the fields needed for namespace isolation
 * (rootfs, hostname, command argv, sync pipe). Later milestones extend
 * this struct with cgroup limits and network config rather than
 * replacing it, so existing code keeps working as we add features.
 */

#define STACK_SIZE (1024 * 1024) /* 1 MB stack for the cloned child */

/* Root of mctr's cgroup v2 tree. A single parent cgroup is created here
 * once (with memory+cpu controllers enabled for its children via
 * cgroup.subtree_control); each container gets its own sub-cgroup under it,
 * named after the container's PID. */
#define MCTR_CGROUP_PARENT "/sys/fs/cgroup/mctr"

/* Where per-container OverlayFS scratch directories (upper/work/merged)
 * live. /run is tmpfs and meant for exactly this kind of transient
 * runtime state -- it's gone on reboot, which is fine since it holds no
 * data that should outlive the container. The shared base image
 * (cfg->rootfs, used as the overlay's lowerdir) is never written here
 * and is untouched by cleanup. */
#define MCTR_RUN_DIR "/run/mctr/containers"

/* Networking (milestone 4). mctr0 is a single shared Linux bridge that
 * every container's veth pair attaches to -- it plays the role of a
 * virtual switch, and also serves as each container's default gateway
 * at MCTR_BRIDGE_IP. The /16 means containers get addresses in
 * 172.18.0.2 - 172.18.255.254. */
#define MCTR_BRIDGE_NAME  "mctr0"
#define MCTR_BRIDGE_IP    "172.18.0.1"
#define MCTR_BRIDGE_CIDR  "172.18.0.1/16"
#define MCTR_SUBNET_CIDR  "172.18.0.0/16"

typedef struct {
    char *rootfs;     /* path to the read-only base image (OverlayFS lowerdir) */
    char *hostname;   /* UTS namespace hostname for the container */
    char **argv;      /* command + args to execve(), NULL-terminated */

    int sync_pipe[2]; /* [0] = read end (child blocks here until parent is ready)
                          [1] = write end (parent signals "go" once setup is done) */

    /* Resource limits (milestone 2). 0 / 0.0 means "not requested". */
    long memory_limit_bytes;  /* maps to cgroup memory.max */
    double cpu_limit_cores;   /* maps to cgroup cpu.max, e.g. 0.5 = half a core */

    char cgroup_path[256];    /* filled in by cgroup_create_for_pid() once created */
    int cgroup_active;        /* 1 once cgroup_path is a real, created cgroup */

    /* Copy-on-write filesystem (milestone 3). container_id and
     * overlay_base/overlay_merged are computed and created by the
     * PARENT before clone() -- so the child's copy of this struct
     * (taken at clone() time) already has them, with no extra IPC
     * needed. The child performs the actual overlay mount() itself,
     * inside its own private mount namespace, and pivot_roots into
     * overlay_merged instead of rootfs directly. */
    char container_id[64];     /* unique per `mctr run` invocation */
    char overlay_base[300];    /* MCTR_RUN_DIR/<container_id> */
    char overlay_merged[340];  /* overlay_base/merged -- what gets pivot_root'd into */
    int overlay_active;        /* 1 once overlay_base/dirs exist and are ready to mount */

    /* Networking (milestone 4). host_pid/veth_host/veth_cont/container_ip
     * are pure computation (no syscalls), filled in by network_prepare()
     * BEFORE clone() -- same reasoning as container_id above: the
     * child's copy of cfg needs these names already present at clone()
     * time, with no extra IPC, since the child configures its own
     * interface using them directly. The one thing that genuinely can't
     * be known before clone() -- the real host pid the kernel assigns
     * the new process -- is only ever needed transiently, as a
     * parameter to the "move this veth into that pid's netns" call, not
     * as a cfg field. */
    int host_pid;              /* mctr runtime's own pid; used only to derive the unique names below */
    char veth_host[16];        /* host-side veth end, e.g. "veth1234h"; attached to MCTR_BRIDGE_NAME */
    char veth_cont[16];        /* container-side veth end, e.g. "veth1234c"; moved into the child's netns */
    char container_ip[20];     /* e.g. "172.18.12.34", assigned to veth_cont inside the container */
} container_config_t;

/* Runs a container per cfg. Blocks until it exits. Returns its exit status,
 * or -1 on internal setup failure. */
int run_container(container_config_t *cfg);

/* --- cgroups.c ---
 *
 * cgroup_init() ensures the shared mctr parent cgroup exists and has the
 * memory/cpu controllers enabled for its children. Call once per run.
 *
 * cgroup_create_for_pid() makes a fresh sub-cgroup named after `pid` and
 * records its path in cfg->cgroup_path.
 *
 * cgroup_apply_limits() writes memory.max / cpu.max based on cfg's limits
 * (skips any limit left at 0/0.0).
 *
 * cgroup_add_pid() enrolls `pid` into cfg->cgroup_path by writing it to
 * cgroup.procs -- must be called before the process execve()s its real
 * workload, so it's never unconstrained even briefly.
 *
 * cgroup_cleanup() removes the per-container cgroup directory after the
 * container has exited (the cgroup must be empty of processes first).
 *
 * All return 0 on success, -1 on failure (and print an explanatory
 * message); failures here are treated as non-fatal warnings by the
 * caller so a container can still run without resource limits if
 * cgroups aren't available (e.g. no root, or this sandbox).
 */
int cgroup_init(void);
int cgroup_create_for_pid(pid_t pid, container_config_t *cfg);
int cgroup_apply_limits(container_config_t *cfg);
int cgroup_add_pid(container_config_t *cfg, pid_t pid);
int cgroup_cleanup(container_config_t *cfg);

/* Parses sizes like "100m", "512k", "1g", or a plain byte count into a
 * byte count. Returns -1 on malformed input. */
long parse_size(const char *s);

/* --- overlay.c ---
 *
 * overlay_make_id() fills in cfg->container_id with something unique to
 * this run (host pid of the mctr process + timestamp). Must be called
 * BEFORE clone(), so the value exists in cfg before the child's memory
 * is copied away from the parent's.
 *
 * overlay_prepare_dirs() creates MCTR_RUN_DIR/<id>/{upper,work,merged}
 * on disk and fills in cfg->overlay_base / cfg->overlay_merged. Also
 * called before clone(), parent-side -- ordinary mkdir() calls, no
 * special capability needed beyond filesystem permissions. Sets
 * cfg->overlay_active = 1 on success.
 *
 * overlay_mount() performs the actual `mount("overlay", ...)` syscall
 * (requires CAP_SYS_ADMIN). Called by the CHILD, after it has its own
 * private mount namespace and after the "/" MS_PRIVATE remount, so the
 * mount is invisible to the host and to any other container. On
 * success, cfg->overlay_merged is ready to pivot_root into.
 *
 * overlay_cleanup() is called by the PARENT after waitpid(). The actual
 * mount normally vanishes on its own once the child's mount namespace
 * is destroyed, but this defensively attempts an unmount anyway before
 * recursively removing overlay_base (upper/work/merged) from disk. The
 * shared base image at cfg->rootfs is never touched.
 */
int overlay_make_id(container_config_t *cfg);
int overlay_prepare_dirs(container_config_t *cfg);
int overlay_mount(container_config_t *cfg);
int overlay_cleanup(container_config_t *cfg);

/* --- network.c ---
 *
 * network_prepare() computes host_pid/veth_host/veth_cont/container_ip
 * (pure computation, no syscalls). Must be called BEFORE clone(), same
 * reasoning as overlay_make_id().
 *
 * network_init_bridge() idempotently ensures the shared mctr0 bridge
 * exists, is up, has MCTR_BRIDGE_IP assigned, that IPv4 forwarding is
 * on, and that an iptables MASQUERADE rule exists for outbound NAT from
 * the container subnet. Safe to call on every run. PARENT-side, no pid
 * needed -- called alongside network_prepare(), before clone().
 *
 * network_create_veth() is PARENT-side, AFTER clone() (it needs the
 * real host pid clone() returned, to move the container-side veth end
 * into that pid's network namespace). Creates the veth pair using the
 * names network_prepare() already chose, attaches the host end to
 * mctr0, brings it up, then moves the container end into child_pid's
 * netns. Must run before signal_child(), same reasoning as the cgroup
 * enrollment step.
 *
 * network_configure_container_side() is CHILD-side, runs inside the
 * container's own network namespace (active since clone() included
 * CLONE_NEWNET) after the parent has signaled it's done. Brings up lo
 * and veth_cont, assigns container_ip to veth_cont, and adds a default
 * route via MCTR_BRIDGE_IP. If the parent's network_create_veth() never
 * ran (or failed), these commands simply fail cleanly (e.g. "Cannot
 * find device") and are treated as a non-fatal warning -- the container
 * still runs, just without network connectivity.
 *
 * network_cleanup() is PARENT-side, after waitpid(). Best-effort
 * removal of the host-side veth end; in the common case the kernel has
 * already removed both ends of the pair automatically once the
 * container's network namespace was destroyed, so this is mostly
 * defensive, mirroring overlay_cleanup()'s umount2() call. The shared
 * mctr0 bridge itself is never removed -- it's long-lived host state,
 * reused across runs, like MCTR_CGROUP_PARENT.
 */
int network_prepare(container_config_t *cfg);
int network_init_bridge(void);
int network_create_veth(pid_t child_pid, container_config_t *cfg);
int network_configure_container_side(container_config_t *cfg);
int network_cleanup(container_config_t *cfg);

#endif /* MCTR_H */
