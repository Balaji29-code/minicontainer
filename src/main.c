#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/mctr.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s run [--memory SIZE] [--cpus N] <rootfs-dir> <command> [args...]\n"
        "\n"
        "  --memory SIZE   memory limit, e.g. 100m, 512k, 1g (cgroup memory.max)\n"
        "  --cpus N        fractional CPU cores, e.g. 0.5 (cgroup cpu.max)\n"
        "  rootfs-dir      path to an extracted root filesystem (e.g. busybox/alpine)\n"
        "  command         program to run as PID 1 inside the container\n"
        "\n"
        "Examples:\n"
        "  sudo %s run ./rootfs /bin/sh\n"
        "  sudo %s run --memory 100m --cpus 0.5 ./rootfs /bin/sh\n",
        prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "run") != 0) {
        usage(argv[0]);
        return 1;
    }

    container_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.hostname = "mctr-container";

    /* Flags may appear in any order, but must come before rootfs-dir.
     * The first argument that isn't a recognized flag is taken to be
     * rootfs-dir, and everything after it is the command + its args. */
    int i = 2;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--memory") == 0) {
            if (++i >= argc) { usage(argv[0]); return 1; }
            long bytes = parse_size(argv[i]);
            if (bytes <= 0) {
                fprintf(stderr, "[mctr] invalid --memory value: %s\n", argv[i]);
                return 1;
            }
            cfg.memory_limit_bytes = bytes;
        } else if (strcmp(argv[i], "--cpus") == 0) {
            if (++i >= argc) { usage(argv[0]); return 1; }
            double cpus = atof(argv[i]);
            if (cpus <= 0.0) {
                fprintf(stderr, "[mctr] invalid --cpus value: %s\n", argv[i]);
                return 1;
            }
            cfg.cpu_limit_cores = cpus;
        } else {
            break; /* argv[i] is rootfs-dir */
        }
    }

    if (i + 1 >= argc) { /* need at least rootfs-dir and command */
        usage(argv[0]);
        return 1;
    }

    cfg.rootfs = argv[i];
    cfg.argv = &argv[i + 1]; /* command + args + NULL terminator, courtesy of the OS */

    printf("[mctr] starting container: rootfs=%s cmd=%s", cfg.rootfs, cfg.argv[0]);
    if (cfg.memory_limit_bytes > 0) printf(" memory=%ldB", cfg.memory_limit_bytes);
    if (cfg.cpu_limit_cores > 0.0) printf(" cpus=%.2f", cfg.cpu_limit_cores);
    printf("\n");

    int rc = run_container(&cfg);

    printf("[mctr] container exited with status %d\n", rc);
    return rc;
}
