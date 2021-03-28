#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static const int signals_to_handle[] = {SIGALRM, SIGINT, SIGQUIT, SIGTERM};
static struct {
    pid_t* children;
    int children_count;
    int children_alive;
    int watch_all_children;
    int termination_stage;
    int timeout;
} conf;

static int debug(char* args, ...) {
#ifdef DEBUG
    va_list vargs;
    va_start(vargs, args);
    int rc = vfprintf(stderr, args, vargs);
    va_end(vargs);
    return rc;
#else
    (void)args;
    return 0;
#endif
}

static void send_signal_to_children(int sig, int use_groups) {
    debug("sending signal %d to children: %s\n", sig, strsignal(sig));
    for (int i = 0; i < conf.children_count; ++i) {
        if (conf.children[i] >= 0 || use_groups) {
            kill(use_groups && conf.children[i] > 0 ? -conf.children[i] : conf.children[i], sig);
        }
    }
}

static void terminate_children(int stage) {
    if (conf.termination_stage > stage) {
        return;
    }
    switch (conf.termination_stage) {
        case 0:
            debug("terminating children (try 1: SIGTERM to original children)\n");
            alarm(conf.timeout);
            send_signal_to_children(SIGTERM, 0);
            break;
        case 1:
            debug("terminating children (try 2: SIGTERM to original children groups)\n");
            alarm(conf.timeout);
            send_signal_to_children(SIGTERM, 1);
            break;
        case 2:
            debug("terminating children (try 3: SIGKILL to original children)\n");
            alarm(conf.timeout);
            send_signal_to_children(SIGKILL, 0);
            break;
        case 3:
            debug("terminating children (try 4: SIGKILL to original children groups)\n");
            alarm(conf.timeout);
            send_signal_to_children(SIGKILL, 1);
            break;
        default:
            fprintf(stderr, "not all children terminated in time, exiting\n");
            exit(1);
    }
    ++conf.termination_stage;
}

static void signal_handler(int sig) {
    debug("received signal %d: %s\n", sig, strsignal(sig));
    switch (sig) {
        case SIGALRM:
            terminate_children(1000);
            break;
        case SIGTERM:
            terminate_children(2);
            break;
        default:
            send_signal_to_children(sig, 0);
            break;
    }
}

static void spawn(char* const args[]) {
#ifdef DEBUG
    fprintf(stderr, "spawning:");
    for (int i = 0; args[i]; ++i) {
        fprintf(stderr, " %s", args[i]);
    }
    fprintf(stderr, "\n");
#endif
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %m\n");
        exit(1);
    }
    if (pid == 0) {
        setpgid(0, 0);
        sigset_t set;
        sigfillset(&set);
        sigprocmask(SIG_UNBLOCK, &set, 0);
        execvp(args[0], args);
        fprintf(stderr, "execvp %s failed: %m\n", args[0]);
        exit(1);
    }
    debug("child pid: %d\n", pid);
    ++conf.children_count;
    ++conf.children_alive;
    conf.children = realloc(conf.children, conf.children_count * sizeof(conf.children[0]));
    if (!conf.children) {
        fprintf(stderr, "realloc failed: %m\n");
        exit(1);
    }
    conf.children[conf.children_count - 1] = pid;
}

static void spawn_children(int argc, char* argv[]) {
    int last_i = 0;
    char* tmp;
    for (int i = 0; i < argc; ++i) {
        tmp = argv[i];
        if (tmp[0] == '-' && tmp[1] == '-' && tmp[2] == '-' && tmp[3] == '\0') {
            argv[i] = NULL;
            spawn(&argv[last_i]);
            argv[i] = tmp;
            last_i = i + 1;
        }
    }
    if (last_i < argc) {
        spawn(&argv[last_i]);
    }
}

static void print_usage(const char* name) {
    printf(
        "Usage:\n"
        "  %s [OPTIONS] --- COMMANDS\n"
        "\n"
        "OPTIONS\n"
        "  -a           also exit when subprocesses terminate that have not been spawned\n"
        "               by muinit directly\n"
        "  -h           show this help message\n"
        "  -t TIMEOUT   set subprocess termination timeout in seconds (default: 2)\n"
        "\n"
        "COMMANDS\n"
        "     Subprocesses to be spawned and their arguments are given after the\n"
        "     first '---' and are separated by '---' (do not include quotation marks).\n"
        "     Though muinit emulates an init session, try not to have subprocesses go\n"
        "     into background ('daemonize') if possible.\n"
        "\n"
        "SUBPROCESS TERMINATION\n"
        "     Once a subprocess terminates (failing or successfully, either only\n"
        "     'original' subprocesses that were spawned by muinit directly or any\n"
        "     subprocess when the -a parameter is set), muinit tries to gracefully\n"
        "     terminate the other subprocesses. This is done in several successive\n"
        "     steps until all children have terminated:\n"
        "       1. Send SIGTERM signal to original subprocesses\n"
        "       2. Send SIGTERM signal to original subprocesses' process groups\n"
        "       3. Send SIGKILL signal to original subprocesses\n"
        "       4. Send SIGKILL signal to original subprocesses' process groups\n"
        "       5. Terminating muinit itself\n"
        "     After each step, muinit waits for TIMEOUT seconds (default: 2) before\n"
        "     trying the next termination step.\n",
        name);
}

int main(int argc, char* argv[]) {
    pid_t pid = getpid();
    debug("running with pid %d\n", pid);

    if (pid != 1) {
        debug("registering as subreaper\n");
        if (prctl(PR_SET_CHILD_SUBREAPER, 1)) {
            fprintf(stderr, "prctl failed: %m\n");
            return 1;
        }
    }

    setsid();

    conf.children = NULL;
    conf.children_count = 0;
    conf.termination_stage = 0;
    conf.timeout = 2;
    conf.watch_all_children = 0;

    for (unsigned int i = 0; i < sizeof(signals_to_handle) / sizeof(signals_to_handle[0]); ++i) {
        int sig = signals_to_handle[i];
        if (signal(sig, signal_handler) == SIG_ERR) {
            fprintf(stderr, "registering signal %d (%s) failed: %m\n", sig, strsignal(sig));
            return 1;
        }
    }

    sigset_t set;
    sigfillset(&set);
    sigprocmask(SIG_BLOCK, &set, 0);

    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            if (argv[i][1] != '\0' && argv[i][2] == '\0') {
                switch (argv[i][1]) {
                    case 'a':
                        conf.watch_all_children = 1;
                        break;
                    case 'h':
                        print_usage(argv[0]);
                        return 0;
                    case 't':
                        ++i;
                        conf.timeout = atoi(argv[i]);
                        if (conf.timeout <= 0) {
                            fprintf(stderr, "invalid timeout %s\n", argv[i]);
                            return 1;
                        }
                        break;
                    default:
                        fprintf(stderr, "unexpected argument %s\n", argv[i]);
                        print_usage(argv[0]);
                        return 1;
                }
                continue;
            } else if (argv[i][1] == '-' && argv[i][2] == '-' && argv[i][3] == '\0') {
                spawn_children(argc - i - 1, &argv[i + 1]);
                break;
            }
        }
        fprintf(stderr, "unexpected argument %s\n", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    if (conf.children_count < 1) {
        fprintf(stderr, "no children spawned, exiting\n");
        return 1;
    }

    sigprocmask(SIG_UNBLOCK, &set, 0);

    int rc = 0;
    int stat;
    while (1) {
        pid = wait(&stat);
        if (pid < 0) {
            if (errno == EINTR) {
                debug("wait interrupted by signal\n");
            } else if (errno == ECHILD) {
                debug("no child left, exiting\n");
                break;
            } else {
                debug("wait: other error: %m\n");
                rc = 1;
                terminate_children(2);
            }
        } else if (pid > 0 && (WIFEXITED(stat) | WIFSIGNALED(stat))) {
            int child_rc;
            if (WIFSIGNALED(stat)) {
                child_rc = 128 + WTERMSIG(stat);
            } else {
                child_rc = WEXITSTATUS(stat);
            }
            debug("process %d exited with %d\n", pid, child_rc);
            if (child_rc) {
                rc = child_rc;
            }
            int i;
            for (i = 0; i < conf.children_count; ++i) {
                if (conf.children[i] == pid) {
                    break;
                }
            }
            if (i < conf.children_count) {
                conf.children[i] = -conf.children[i];
                --conf.children_alive;
                if (conf.children_alive <= 0) {
                    debug("no child left, exiting\n");
                    break;
                }
                terminate_children(0);
            } else if (conf.watch_all_children) {
                terminate_children(0);
            }
        }
    }

    free(conf.children);

    return rc;
}
