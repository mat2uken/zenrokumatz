/*
 * su_shim.c - minimal `su` that speaks both calling conventions in use on
 * these X96 boxes.
 *
 * The stock Amlogic /system/xbin/su understands only
 *
 *     su [UID[,GID[,GID2]...]] [COMMAND [ARG...]]
 *
 * while the primary box runs a SuperSU build that additionally understands
 *
 *     su -c COMMAND
 *
 * and the app hardcodes the latter (BootReceiver runs
 * `/system/xbin/su -c "nohup sh <watchdog> ..."`). The stock binary treats
 * `-c` as a uid spec, fails with `invalid uid/gid '-c'`, and its file mode
 * (4750 root:shell) is not even executable by the app's uid.
 *
 * This shim accepts both forms so the app runs unmodified on either box, and
 * keeps the stock uid/gid-list semantics for everything else.
 *
 * Installed as /system/xbin/su with mode 4755 root:root and the su_exec
 * SELinux label (see tools/install-su-shim.sh). Both boxes run SELinux in
 * permissive mode.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SHELL "/system/bin/sh"
#define MAX_GROUPS 16
#define GID_UNSET ((gid_t)-1)

static void usage(void)
{
    fputs("usage: su [-c COMMAND] | su [UID[,GID[,GID2]...]] [COMMAND [ARG...]]\n"
          "Switch to UID (default root) and run COMMAND (default "
          DEFAULT_SHELL ").\n",
          stderr);
}

/*
 * Diagnostics for the boot-time failure path: the app's Runtime.exec() cannot
 * see our stderr, so failures are appended here. Disabled by setting
 * PIX_SU_SHIM_LOG to an empty value is not possible in a setuid binary's
 * environment, so this stays compiled in but only writes on failure.
 */
#define SHIM_LOG "/data/local/tmp/pix-stream/su_shim.log"

static void note(const char *what)
{
    int fd = open(SHIM_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        dprintf(fd, "%ld %s\n", (long)time(NULL), what);
        close(fd);
    }
}

/* Distinct exit codes so a caller that only sees waitFor() can report where
 * the shim stopped (the app logs nothing but the number). */
#define EXIT_NOT_SETUID   126
#define EXIT_SETGROUPS    125
#define EXIT_SETGID       124
#define EXIT_SETUID       123
#define EXIT_EXEC         122

static int fail_code(const char *what, int code)
{
    char msg[128];

    snprintf(msg, sizeof(msg), "%s: %s (errno %d)", what, strerror(errno), errno);
    note(msg);
    fprintf(stderr, "su: %s\n", msg);
    return code;
}

static int fail(const char *what)
{
    return fail_code(what, EXIT_EXEC);
}

/*
 * setgroups must precede setgid/setuid: both need the privilege we drop.
 *
 * An app process that executes this setuid binary ends up with uid 0 but *no
 * capabilities*, so setgroups() fails with EPERM even though the uid is
 * already the one we want (observed at boot: "setgroups: Operation not
 * permitted"). Getting the uid right is what callers need, so when we already
 * hold the target uid the group bookkeeping is best effort instead of fatal.
 */
static int become(uid_t uid, gid_t gid, gid_t *groups, size_t ngroups)
{
    int same_uid = (geteuid() == uid);

    if (setgroups(ngroups, groups) != 0 && !same_uid) {
        return fail_code("setgroups", EXIT_SETGROUPS);
    }
    if (setgid(gid) != 0 && !same_uid) {
        return fail_code("setgid", EXIT_SETGID);
    }
    /*
     * setuid() needs CAP_SETUID to rewrite the *real* uid as well, so it fails
     * for a plain setuid exec without capabilities. That case is only made to
     * look successful by a user namespace (uid 0 inside, the app's uid
     * outside), which grants no real privilege -- verified on the second unit:
     * "enter uid=10059 euid=0 caps=0". Callers that need root must therefore
     * be started by init, not from an app.
     */
    if (setuid(uid) != 0 && getuid() != uid) {
        /* An effective-only root is not usable: the kernel resets the
         * effective uid as soon as the next non-setuid program is exec'd, so
         * the command would silently run unprivileged. Refuse instead. */
        return fail_code("setuid (real uid unchanged)", EXIT_SETUID);
    }
    return 0;
}

/* su -c COMMAND: hand the string to the shell as root. */
static int run_shell(const char *shell, const char *cmd, char **rest, int nrest)
{
    char **av = calloc((size_t)nrest + 4, sizeof(*av));
    int i;

    if (av == NULL) {
        return fail("calloc");
    }
    av[0] = (char *)shell;
    av[1] = (char *)"-c";
    av[2] = (char *)cmd;
    for (i = 0; i < nrest; i++) {
        av[3 + i] = rest[i];
    }
    execv(shell, av);
    return fail("execv");
}

/*
 * Parse "uid[,gid[,gid2]...]". Returns 0 when the whole string is a numeric
 * list, -1 otherwise (the caller then treats the argument as a command).
 * *gid stays GID_UNSET when the list carried no group.
 */
static int parse_ids(const char *spec, uid_t *uid, gid_t *gid,
                     gid_t *extra, size_t *nextra)
{
    const char *p = spec;
    long value;
    char *end;
    int first = 1;

    *nextra = 0;
    if (*p == '\0') {
        return -1;
    }
    while (*p != '\0') {
        errno = 0;
        value = strtol(p, &end, 10);
        if (end == p || errno != 0 || value < 0) {
            return -1;
        }
        if (first) {
            *uid = (uid_t)value;
            first = 0;
        } else if (*gid == GID_UNSET) {
            *gid = (gid_t)value;
        } else if (*nextra < MAX_GROUPS) {
            extra[(*nextra)++] = (gid_t)value;
        }
        p = end;
        if (*p == ',') {
            p++;
            if (*p == '\0') {
                return -1;
            }
            continue;
        }
        if (*p != '\0') {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *shell = DEFAULT_SHELL;
    const char *cmd;
    uid_t uid = 0;
    gid_t gid = 0;
    gid_t extra[MAX_GROUPS];
    size_t ngroup = 0;
    int rc;
    int i = 1;

    /* Without the setuid bit we cannot switch users; say so instead of
     * silently running the command unprivileged. */
    if (geteuid() != 0) {
        note("not setuid root");
        fputs("su: not setuid root (check the file mode)\n", stderr);
        return EXIT_NOT_SETUID;
    }

    while (i < argc) {
        const char *a = argv[i];

        if (a[0] != '-' || a[1] == '\0') {
            break; /* not an option: uid spec or command */
        }
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 2;
        }
        if (strncmp(a, "-s", 2) == 0) {
            if (a[2] != '\0') {
                shell = a + 2;
                i++;
            } else if (i + 1 < argc) {
                shell = argv[i + 1];
                i += 2;
            } else {
                usage();
                return 2;
            }
            continue;
        }
        if (strncmp(a, "-c", 2) == 0) {
            if (a[2] != '\0') {
                cmd = a + 2;
                i++;
            } else if (i + 1 < argc) {
                cmd = argv[i + 1];
                i += 2;
            } else {
                usage();
                return 2;
            }
            rc = become(0, 0, NULL, 0);
            if (rc != 0) {
                return rc;
            }
            return run_shell(shell, cmd, &argv[i], argc - i);
        }
        fprintf(stderr, "su: unknown option: %s\n", a);
        usage();
        return 2;
    }

    /* su [UID[,GID...]] [COMMAND [ARG...]] */
    gid = GID_UNSET;
    if (i < argc && parse_ids(argv[i], &uid, &gid, extra, &ngroup) == 0) {
        i++;
    }
    /* Like login su, fall back to the target uid as gid when none is given. */
    if (gid == GID_UNSET) {
        gid = (gid_t)uid;
    }

    rc = become(uid, gid, ngroup > 0 ? extra : NULL, ngroup);
    if (rc != 0) {
        return rc;
    }

    if (i >= argc) {
        char *av[2];

        av[0] = (char *)shell;
        av[1] = NULL;
        execv(shell, av);
        return fail("execv");
    }

    execvp(argv[i], &argv[i]);
    return fail("execvp");
}
