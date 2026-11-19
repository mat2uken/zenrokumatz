/* Exercise the real vendor fork-writer signal/END-ACK seam, without USB. */
#define _POSIX_C_SOURCE 200809L
#include "ts_output.h"

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

static void on_signal(int number) {
    stopped = number;
}

/* Model a real pipe consumer that, like FFmpeg, stops on a group signal.
 * An extra readiness pipe makes the negative INT/EPIPE test deterministic. */
static pid_t pipe_consumer(const char *path, int *output) {
    int data[2], ready[2];
    if (pipe(data) || pipe(ready))
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (!child) {
        close(data[1]);
        close(ready[0]);
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGTERM, SIG_DFL);
        FILE *file = fopen(path, "w");
        if (!file || write(ready[1], "!", 1) != 1)
            _exit(2);
        close(ready[1]);
        char buffer[4096];
        ssize_t size;
        while ((size = read(data[0], buffer, sizeof(buffer))) > 0)
            if (fwrite(buffer, 1, (size_t)size, file) != (size_t)size || fflush(file))
                _exit(3);
        _exit(fclose(file) ? 3 : 0);
    }
    close(data[0]);
    close(ready[1]);
    char byte;
    if (read(ready[0], &byte, 1) != 1)
        return -1;
    close(ready[0]);
    *output = data[1];
    return child;
}

int main(int argc, char **argv) {
    int piped = argc == 3 && !strcmp(argv[1], "--pipe");
    if (argc != 2 && !piped)
        return 2;
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
        return 2;
    (void)signal(SIGPIPE, SIG_IGN);

    int pipe_fd = -1;
    pid_t reader = piped ? pipe_consumer(argv[2], &pipe_fd) : 0;
    if (reader < 0)
        return 2;
    ts_output *writer = NULL;
    if (ts_output_open(piped ? "-" : argv[1], pipe_fd, NULL, &writer))
        return 2;
    /* Complete a real write/ACK before announcing readiness. In particular,
     * this proves child_loop installed its distinct INT/TERM dispositions. */
    const char begin[] = "writer-ready\n";
    int error = ts_output_submit(writer, begin, sizeof(begin) - 1, NULL);
    ts_output_stats stats = {0};
    for (unsigned attempts = 0; !error && attempts < 100; ++attempts) {
        error = ts_output_poll(writer, 10);
        ts_output_snapshot(writer, &stats);
        if (!stats.pending_bytes)
            break;
    }
    if (error || stats.pending_bytes) {
        (void)ts_output_destroy(writer);
        return 2;
    }
    const char *trace = getenv("TRACE");
    if (trace) {
        FILE *file = fopen(trace, "a");
        if (file) {
            fputs("capture-ready\n", file);
            fclose(file);
        }
    }
    printf("{\"ready\":true,\"parent\":%ld,\"writer\":%" PRId64 "}\n",
           (long)getpid(), stats.child_pid);
    fflush(stdout);
    while (!stopped) {
        struct timespec pause = {0, 10000000L};
        nanosleep(&pause, NULL);
    }

    int reader_status = 0;
    int reader_reaped = 0;
    if (piped && stopped == SIGINT) {
        /* Negative ordering control: let the signaled reader actually close
         * its FD before the terminal write (real native has USB teardown here).
         * Otherwise the kernel may still accept a write into its dying pipe. */
        reader_reaped = waitpid(reader, &reader_status, 0) == reader;
        if (!reader_reaped)
            return 2;
    }
    const char marker[] = "writer-end\n";
    error = ts_output_submit(writer, marker, sizeof(marker) - 1, NULL);
    if (!error)
        error = ts_output_finish(writer, 1000);
    ts_output_snapshot(writer, &stats);
    int destroyed = ts_output_destroy(writer);
    if (piped) {
        close(pipe_fd);
        if (!reader_reaped && waitpid(reader, &reader_status, 0) != reader)
            return 2;
    }
    printf("{\"signal\":%d,\"error\":%d,\"destroy\":%d,\"child_status\":%d,"
           "\"child_term\":%d,\"confirmed\":%" PRIu64 ",\"pending\":%" PRIu64 ","
           "\"reader_term\":%d}\n",
           (int)stopped, error, destroyed, stats.child_status,
           WIFSIGNALED(stats.child_status) ? WTERMSIG(stats.child_status) : 0,
           stats.confirmed_bytes, stats.pending_bytes,
           WIFSIGNALED(reader_status) ? WTERMSIG(reader_status) : 0);
    return error || destroyed || reader_status || stats.pending_bytes ||
                   stats.confirmed_bytes != sizeof(begin) + sizeof(marker) - 2 ||
                   stats.child_status != 0
               ? 1
               : 0;
}
