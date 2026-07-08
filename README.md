# mctr — a Docker-like container runtime in C

Milestone 1: process isolation via Linux namespaces (PID, UTS, Mount). Milestone 2: resource limits via cgroups v2 (memory, CPU). Milestone 3: copy-on-write images via OverlayFS. Milestone 4: container networking via veth pairs, a Linux bridge, and NAT. See `THEORY.md` for the underlying concepts. This README covers building and testing.

## Why you must test this on a real Linux machine

`clone()` with `CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS` requires `CAP_SYS_ADMIN` (i.e. root). It will fail with `Operation not permitted` inside any unprivileged sandbox/container, including the one this code was written in. You need a real Linux box or VM — Ubuntu 22.04 is assumed below — where you have `sudo`.

## Build

```
make
```

Produces `./mctr`. `make clean` removes build artifacts.

## Get a minimal rootfs (base image) to run containers from

`mctr run` needs a directory that looks like a Linux filesystem (a `/bin/sh`, basic libs, etc.). As of milestone 3 this directory is treated as a **shared, read-only base image** — `mctr` never writes into it directly. Each container gets its own private writable layer on top via OverlayFS, so you can point multiple containers at the very same rootfs directory simultaneously without them stepping on each other. The quickest source is a BusyBox static rootfs:

```
mkdir -p rootfs
cd rootfs
curl -LO https://github.com/docker-library/busybox/raw/dist-amd64/stable/glibc/busybox.tar.xz
tar xf busybox.tar.xz
rm busybox.tar.xz
cd ..
```

If that mirror is unavailable, any extracted Alpine or BusyBox minirootfs tarball works the same way — you just need a directory containing `/bin/sh` (or similar) before pivoting into it. Docker Hub's `alpine` image filesystem (exported via `docker export`) is another easy source if you happen to have Docker installed on the test machine.

## Run

```
sudo ./mctr run ./rootfs /bin/sh
sudo ./mctr run --memory 100m --cpus 0.5 ./rootfs /bin/sh
```

What you should observe once inside the shell:

- `hostname` prints `mctr-container`, not the host's hostname.
- `echo $$` or `ps` shows the shell as PID 1 (or very close to it) — a fresh, near-empty process table, not the host's hundreds of processes. (BusyBox's `ps` needs `/proc` mounted, which `mctr` already does — confirm with `cat /proc/1/comm`.)
- `ls /` shows only the rootfs you pointed it at, not your host's real `/`.
- In another terminal on the host, `hostname` still shows the real machine name — confirming UTS isolation didn't leak.
- `exit` the shell and `mctr` prints the container's exit status and returns to your host prompt normally.

## Verifying resource limits (cgroups v2)

With `--memory`/`--cpus` set, `mctr` creates `/sys/fs/cgroup/mctr/<pid>` on the host and writes `memory.max`/`cpu.max` into it before the container's command runs. You can confirm this from a second host terminal while the container is running:

```
cat /sys/fs/cgroup/mctr/<pid>/memory.max
cat /sys/fs/cgroup/mctr/<pid>/cpu.max
cat /sys/fs/cgroup/mctr/<pid>/cgroup.procs   # should list the container's pid(s)
```

**To see the memory limit actually trigger an OOM kill**, run a container with a tight limit and have it allocate past it:

```
sudo ./mctr run --memory 20m ./rootfs /bin/sh -c "yes | head -c 200000000 | wc -c"
```

(or any small C program that `malloc`s and writes to memory in a growing loop). Watch `cat /sys/fs/cgroup/mctr/<pid>/memory.events` from the other terminal — its `oom_kill` counter increments the moment the kernel kills the offending process, and only that process; the host stays unaffected.

**To see the CPU limit throttle**, run something CPU-bound and compare wall-clock time with and without `--cpus`:

```
sudo ./mctr run --cpus 0.2 ./rootfs /bin/sh -c "time (i=0; while [ \$i -lt 5000000 ]; do i=\$((i+1)); done)"
```

A `0.2` limit (a fifth of a core) should take roughly 5x longer wall-clock than the same loop run with no `--cpus` flag at all, even though the CPU is otherwise idle — that's the quota/period throttling described in `THEORY.md`, not contention with other processes.

## Verifying copy-on-write isolation (OverlayFS)

Every `mctr run` creates `/run/mctr/containers/<id>/{upper,work,merged}` on the host, mounts an overlay (`lowerdir=<your rootfs>,upperdir=upper,workdir=work`) at `merged`, and pivot_roots the container into `merged` instead of your rootfs directly. While a container is running, a second host terminal can inspect this:

```
ls /run/mctr/containers/                 # one directory per running (or just-exited) container
cat /run/mctr/containers/<id>/merged/... # the container's view, from the host
ls /run/mctr/containers/<id>/upper       # files the container has actually written/changed so far
```

**To see the copy-on-write behavior directly**, run a container, write a file, and check where it landed:

```
sudo ./mctr run ./rootfs /bin/sh
# inside the container:
echo "hello" > /tmp/test.txt
exit
```

Then on the host: `/tmp/test.txt` does **not** appear under `./rootfs/tmp/` (the shared image was never touched), but does appear under `/run/mctr/containers/<id>/upper/tmp/test.txt` — that's the copy-up. After the container exits, `mctr` removes the whole `<id>` directory (upper/work/merged); the change is gone, the base image is exactly as it was before.

**To see image sharing across containers**, run two containers off the same rootfs at once (two terminals):

```
sudo ./mctr run ./rootfs /bin/sh   # terminal A
sudo ./mctr run ./rootfs /bin/sh   # terminal B
```

Each gets its own `<id>` directory and its own `upper/`. Writing a file in A's shell does not appear in B's shell (`ls /tmp` in B won't show it) — each container's merged view only combines the *shared* lowerdir with its *own* upperdir, never another container's. This is the same mechanism that lets ten real Docker containers share one Alpine image on disk while keeping their changes private.

**If overlay setup fails** (e.g. not running as root, so `/run/mctr` can't be created, or the `overlay` kernel module isn't available), `mctr` prints a warning and falls back to pivoting straight into your rootfs directory — meaning the container's writes go directly into the shared image with no isolation and no cleanup. This is intentional graceful degradation rather than a hard failure, but it means you've lost the property this milestone is about; fix the underlying cause (usually: use `sudo`) rather than relying on the fallback.

## Verifying container networking (veth + bridge + NAT)

Every `mctr run` ensures a shared bridge `mctr0` exists on the host (`172.18.0.1/16`, created once, reused across runs — same lifecycle as `/sys/fs/cgroup/mctr`), creates a fresh veth pair for this container, attaches the host end to `mctr0`, and moves the container end into the container's own network namespace. On success, `mctr` prints the container's assigned IP right before handing control to your command:

```
[mctr] container network: 172.18.4.212/16 via mctr0 (gateway 172.18.0.1, host veth veth1234h)
```

**To see the host-side setup**, from a second terminal while a container is running:

```
ip link show mctr0                 # the bridge, should be UP
bridge link show                   # lists veth-host ends attached as bridge ports
ip addr show veth1234h              # host end of this container's veth pair (name from the printed line)
iptables -t nat -L POSTROUTING -n   # should show a MASQUERADE rule for 172.18.0.0/16
cat /proc/sys/net/ipv4/ip_forward   # should read 1
```

**To verify connectivity from inside the container**, in the container's shell:

```
ip addr show          # veth-cont should be UP with the printed 172.18.x.x/16 address
ip route               # default route via 172.18.0.1
ping -c 3 172.18.0.1   # the bridge / gateway -- proves the veth + bridge path works
ping -c 3 8.8.8.8       # the real internet -- proves NAT/MASQUERADE + ip_forward work
```

**To verify two containers can reach each other**, run two (two terminals, same rootfs is fine — they're filesystem-isolated by the overlay from milestone 3 but share the same `mctr0` bridge):

```
sudo ./mctr run ./rootfs /bin/sh   # terminal A — note its printed IP, e.g. 172.18.4.212
sudo ./mctr run ./rootfs /bin/sh   # terminal B
# from B:
ping -c 3 172.18.4.212
```

This should succeed — both containers' veth-host ends are ports on the same bridge, so the bridge switches the ICMP packets directly between them without ever leaving the host.

**If network setup fails** (no root/`CAP_NET_ADMIN`, or `ip`/`iptables` aren't installed), `mctr` prints a warning at each failed step (bridge creation, veth creation, or the in-container `ip addr`/`route` commands) and the container still runs — just with no network. This is the same non-fatal degradation as cgroups/overlay: fix the root cause (usually `sudo`, or `apt install iproute2 iptables`) rather than relying on the fallback.

## Common errors and what they mean

`clone: Operation not permitted` — not running as root, or running inside an already-sandboxed/restricted environment (containers, some CI runners, this dev sandbox). Use `sudo` on a real machine/VM.

`pivot_root_into: bind-mount new_root onto itself: ...` — the rootfs path doesn't exist or isn't a directory; double check the path you passed.

`mount /proc: ...` (non-fatal, printed but execution continues) — usually harmless on first run; if `ps`/`/proc` look wrong inside the container, check that `/proc` doesn't already have something mounted there from a previous failed run — remove and recreate the rootfs dir if state gets weird.

If the container hangs after `exit` instead of returning — that points to an orphaned process still alive in the PID namespace (the namespace can't fully tear down). Check `ps aux | grep mctr` on the host and kill stragglers; this is an indicator of a future milestone bug (zombie reaping in our "init"), not normal for milestone 1's single-shell use case.

`mkdir(/sys/fs/cgroup/mctr): Operation not permitted` — not running as root; cgroup v2 directory creation under `/sys/fs/cgroup` needs it (same requirement as `clone()`).

`[mctr] invalid --memory value` / `invalid --cpus value` — check the format: `--memory` wants a number plus an optional single `k`/`m`/`g` suffix (e.g. `100m`, not `100mb` or `100 MB`); `--cpus` wants a positive number (e.g. `0.5`).

`rmdir(/sys/fs/cgroup/mctr/<pid>): Device or resource busy` (printed after the container exits) — a process is still alive inside that cgroup. Usually a background/orphaned grandchild the container's shell spawned and didn't reap; same root cause as the PID-namespace hang case above. Check with `cat /sys/fs/cgroup/mctr/<pid>/cgroup.procs`.

`[mctr overlay] mkdir(/run/mctr): Permission denied` — not running as root; `/run` is root-owned. Use `sudo`.

`mount overlay (lowerdir=...): Invalid argument` — usually means the rootfs path passed to `mctr run` doesn't exist, or the `overlay` kernel module isn't loaded (`sudo modprobe overlay` on most distros; it's built into the kernel on most modern ones already).

`[mctr overlay] warning: failed to fully clean up ...` (printed after the container exits) — `rm -rf`-equivalent cleanup hit a file it couldn't remove, often because something still has it open. Safe to manually `sudo rm -rf /run/mctr/containers/<id>` once you've confirmed no related process is still running.

`RTNETLINK answers: Operation not permitted` / `[mctr net] failed to create bridge mctr0` — not running as root; creating bridges, veth pairs, and moving interfaces between netns all need `CAP_NET_ADMIN`. Use `sudo`.

`execvp(ip): No such file or directory` (from `[mctr net]`) — the `ip` (iproute2) or `iptables` binary isn't installed. `sudo apt install iproute2 iptables` on Debian/Ubuntu.

`Cannot find device "veth1234c"` (seen inside the container, from the in-container `ip addr`/`ip link set` commands) — the parent's `network_create_veth()` never ran or failed (check the host terminal for an earlier `[mctr net]` warning), so the container-side veth end was never moved into this netns. The container still runs; it just has no network.

`iptables: No chain/target/match by that name` — usually means the `nat` table isn't available (missing `iptable_nat` kernel module on some minimal/older kernels); `sudo modprobe iptable_nat` and retry.

If two containers can't `ping` each other despite both printing a network line — confirm both are on the bridge with `bridge link show`; if a host end is missing, that container's `network_create_veth()` step failed silently-ish (check for its `[mctr] warning: failed to set up networking` line, easy to miss among other output).

## Project layout

```
include/mctr.h     shared struct + function declarations
src/main.c         CLI parsing (run, --memory, --cpus), dispatch to run_container()
src/namespaces.c   clone(), sync pipe, pivot_root, hostname, /proc mount, exec
src/cgroups.c      cgroup v2 creation, memory.max/cpu.max, pid enrollment, cleanup
src/overlay.c      OverlayFS dir prep, mount, copy-on-write cleanup
src/network.c      veth pair + bridge + NAT setup, in-container interface config
Makefile           build rules
THEORY.md          full conceptual writeup (namespaces, cgroups, overlayfs, networking)
```

All four milestones from the original project scope are implemented: namespace isolation (PID/UTS/Mount), cgroups v2 resource limits, OverlayFS copy-on-write images, and veth/bridge/NAT networking. Each one degrades gracefully (warns and falls back) rather than aborting if its underlying kernel feature or privilege isn't available — see the "Common errors" sections above for each milestone's specific failure modes.
