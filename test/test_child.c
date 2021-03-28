#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static sigset_t set;

static void spawn(char* const args[]) {
    pid_t mypid = getpid();
    int res = fork();
    if (res < 0) {
        fprintf(stderr, "child %d: fork failed: %m\n", mypid);
        exit(1);
    }
    if (res == 0) {
        sigprocmask(SIG_UNBLOCK, &set, NULL);
        execvp(args[0], args);
        fprintf(stderr, "child %d: execvp %s failed: %m\n", mypid, args[0]);
        exit(1);
    }
    fprintf(stderr, "child %d: spawned pid: %d\n", mypid, res);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "child %d: wrong number of arguments\n", getpid());
        return 1;
    }

    int timeout = 0;
    int rc = 0;
    int ignore_sigterm = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rc") == 0) {
            if (i >= argc - 1) {
                fprintf(stderr, "child %d: wrong number of arguments\n", getpid());
            }
            rc = atoi(argv[i + 1]);
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--timeout") == 0) {
            if (i >= argc - 1) {
                fprintf(stderr, "child %d: wrong number of arguments\n", getpid());
            }
            timeout = atoi(argv[i + 1]);
            ++i;
            continue;
        }
        if (strcmp(argv[i], "--ignore-sigterm") == 0) {
            ignore_sigterm = 1;
            continue;
        }
        if (strcmp(argv[i], "--exec") == 0) {
            spawn(&argv[i + 1]);
            return 0;
        }
        if (strcmp(argv[i], "--call") == 0) {
            spawn(&argv[i + 1]);
            break;
        }
        fprintf(stderr, "child %d: unexpected argument\n", getpid());
        return 1;
    }

    sigfillset(&set);
    sigprocmask(SIG_BLOCK, &set, NULL);

    int sig;
    alarm(timeout);
    while (1) {
        if (sigwait(&set, &sig) != 0) {
            fprintf(stderr, "child %d: sigwait failed: %m\n", getpid());
            return -1;
        }
        fprintf(stderr, "child %d: received signal %d: %s\n", getpid(), sig, strsignal(sig));
        switch (sig) {
            case SIGALRM:
                return rc;
            case SIGTERM:
                if (!ignore_sigterm) {
                    return rc;
                }
        }
    }

    return rc;
}
