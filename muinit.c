#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static struct {
    char* proc_children_path;
    int termination_stage;
    int timeout;
    int* termination_signals;
    int termination_signals_count;
    sigset_t set;
} conf;

static int debug(char* args, ...);
static void print_usage(const char* name, int show_full_help);
static int read_signals_array(char* s, int* count, int** signals);
static int register_signal_handler(int sig);
static void send_signal_to_children(int sig);
static void signal_handler(int sig);
static void spawn(char* const args[]);
static int spawn_children(char* argv[]);
static void terminate_children();

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

static void print_usage(const char* name, int show_full_help) {
    if (show_full_help) {
        printf(
            "muinit -- lightweight subprocess supervisor\n"
            "           - minimal 'init', e.g. for docker containers\n"
            "           - forwards signals to subprocesses\n"
            "           - reaps zombie subprocesses\n"
            "           - gracefully terminates all subprocesses after one exited\n"
            "\n");
    }

    printf(
        "Usage:\n"
        "  %s [OPTIONS] --- COMMANDS\n"
        "\n"
        "OPTIONS\n"
        "  -h           show help message\n"
        "  -k SIGNALS   signals to iterate over in subprocess termination\n"
        "               (comma-separated list of their numbers)\n"
        "               default: SIGTERM,SIGKILL\n"
        "  -s SIGNALS   signals to forward to subprocesses (comma-separated numbers)\n"
        "               default: SIGINT\n"
        "  -t TIMEOUT   set subprocess termination stage timeout in seconds\n"
        "               default: 2s\n",
        name);

    if (show_full_help) {
        printf(
            "\n"
            "COMMANDS\n"
            "     Subprocesses to be spawned and their arguments are given after the\n"
            "     first '---' and are separated by '---' (do not include quotation marks).\n"
            "     Though muinit emulates an init session, try not to have subprocesses go\n"
            "     into background ('daemonize') if possible.\n"
            "\n"
            "SIGNAL FORWARDING\n"
            "     Signals given via the `-s' option (and that can be caught) are forwarded\n"
            "     to subprocesses. Special cases are SIGALRM, which is used by muinit itself,\n"
            "     and SIGTERM, which resets the termination steps and is then forwarded.\n"
            "     The SIGNALS option values must be lists of comma-separated numbers of the\n"
            "     signals (run `kill -L' to see a list)\n"
            "\n"
            "SUBPROCESS TERMINATION\n"
            "     Once a subprocess terminates (failing or successfully), muinit tries to\n"
            "     gracefully terminate the other subprocesses. This is done in several\n"
            "     successive steps until all children have terminated. The steps are defined\n"
            "     by the signal send in each respective step as given via the `-k' option\n"
            "     (default: SIGTERM,SIGKILL). The timeout to wait after each step before\n"
            "     trying the next one can be given via the `-t' option (default: 2s)\n"
            "\n"
            "EXIT STATUS\n"
            "    Internal errors cause an exit status of 1. Otherwise the exit status equals\n"
            "    that of the first failed subprocess or 0 if all subprocesses succeed.\n");
    }
}

static int read_signals_array(char* s, int* count, int** signals) { /* reads a comma-separated list of signal numbers from string */
    if (!s || s[0] == '\0') {
        fprintf(stderr, "no signals given\n");
        return 1;
    }
    long val;
    char* buf = s;
    char* next = buf;
    while (next[0] != '\0') {
        val = strtol(buf, &next, 10);
        if (next == buf || (next[0] != ',' && next[0] != '\0')) {
            fprintf(stderr, "unexpected value in %s\n", s);
            return 1;
        }
        if (val < 0 || val > SIGRTMAX) {
            fprintf(stderr, "invalid signal number %ld\n", val);
            return 1;
        }
        ++(*count);
        *signals = realloc(*signals, (*count) * sizeof(int));
        if (!(*signals)) {
            fprintf(stderr, "can't allocate memory: %m\n");
            return 1;
        }
        (*signals)[(*count) - 1] = val;
        if (next[0] == ',') {
            ++next;
        }
        buf = next;
    }
    return 0;
}

static int register_signal_handler(int sig) {
    if (signal(sig, signal_handler) == SIG_ERR) {
        fprintf(stderr, "registering signal %d failed: %m\n", sig);
        return 1;
    }
    return 0;
}

static void send_signal_to_children(int sig) {
    sigprocmask(SIG_BLOCK, &conf.set, 0);

    FILE* f = fopen(conf.proc_children_path, "r");
    if (!f) {
        fprintf(stderr, "can't open `%s': %m\n", conf.proc_children_path);
        exit(1);
    }

    pid_t pid;
    int n;
    while (1) {
        n = fscanf(f, "%d", &pid);
        if (n != 1) {
            if (errno != 0) {
                fprintf(stderr, "fscanf on `%s' failed: %m\n", conf.proc_children_path);
            } else if (n != EOF) {
                fprintf(stderr, "unexpected value in `%s'\n", conf.proc_children_path);
            }
            break;
        }
        if (pid > 0) {
            debug("sending signal %d to child %d\n", sig, pid);
            kill(pid, sig);
        }
    }

    fclose(f);

    sigprocmask(SIG_UNBLOCK, &conf.set, 0);
}

static void signal_handler(int sig) {
    debug("received signal %d\n", sig);
    switch (sig) {
        case SIGALRM:
            terminate_children();
            break;
        case SIGTERM:
            alarm(0);
            conf.termination_stage = 0;
            terminate_children();
            break;
        default:
            send_signal_to_children(sig);
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
        sigprocmask(SIG_UNBLOCK, &conf.set, 0);
        execvp(args[0], args);
        fprintf(stderr, "execvp %s failed: %m\n", args[0]);
        exit(1);
    }
    debug("child spawned: %d\n", pid);
}

static int spawn_children(char* argv[]) {
    char* tmp;
    char** child_argv = argv;
    int spawn_count = 0;
    for (int i = 0; argv[i]; ++i) {
        tmp = argv[i];
        if (tmp[0] == '-' && tmp[1] == '-' && tmp[2] == '-' && tmp[3] == '\0') {
            if (child_argv != argv + i) {
                argv[i] = NULL;
                ++spawn_count;
                spawn(child_argv);
                argv[i] = tmp;
            }
            child_argv = argv + i + 1;
        }
    }
    if (child_argv[0]) {
        ++spawn_count;
        spawn(child_argv);
    }
    return spawn_count;
}

static void terminate_children() {
    if (conf.termination_stage >= conf.termination_signals_count) {
        fprintf(stderr, "not all children terminated in time, exiting\n");
        exit(1);
    }
    debug("terminating children (try %d/%d)\n", conf.termination_stage + 1, conf.termination_signals_count);
    alarm(conf.timeout);
    send_signal_to_children(conf.termination_signals[conf.termination_stage]);
    ++conf.termination_stage;
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

    conf.proc_children_path = NULL;
    conf.termination_signals = NULL;
    conf.termination_signals_count = 0;
    conf.termination_stage = 0;
    conf.timeout = 2;
    sigfillset(&conf.set);

    int forward_signals_count = 0;
    int* forward_signals = NULL;
    char** first_child_argv = argv + argc;

    /* parse command line arguments */
    char* arg;
    for (int i = 1; i < argc; ++i) {
        arg = argv[i];
        if (arg[0] == '-') {
            if (arg[1] != '\0' && arg[2] == '\0') {
                switch (arg[1]) {
                    case 'h':
                        print_usage(argv[0], 1);
                        return 0;
                    case 'k': {
                        ++i;
                        if (!argv[i] || argv[i][0] == '\0') {
                            fprintf(stderr, "no termination step list given\n");
                            print_usage(argv[0], 0);
                            return 1;
                        }
                        if (read_signals_array(argv[i], &conf.termination_signals_count, &conf.termination_signals)) {
                            return 1;
                        }
                        break;
                    }
                    case 's': {
                        ++i;
                        if (!argv[i] || argv[i][0] == '\0') {
                            fprintf(stderr, "no signals to forward given\n");
                            print_usage(argv[0], 0);
                            return 1;
                        }
                        if (read_signals_array(argv[i], &forward_signals_count, &forward_signals)) {
                            return 1;
                        }
                        break;
                    }
                    case 't':
                        ++i;
                        if (!argv[i] || argv[i][0] == '\0') {
                            fprintf(stderr, "no timeout given\n");
                            print_usage(argv[0], 0);
                            return 1;
                        }
                        conf.timeout = strtol(argv[i], &arg, 10);
                        if (!arg || arg[0] != '\0' || conf.timeout < 0) {
                            fprintf(stderr, "invalid timeout: %s\n", argv[i]);
                            return 1;
                        }
                        break;
                    default:
                        fprintf(stderr, "unexpected argument %s\n", arg);
                        print_usage(argv[0], 0);
                        return 1;
                }
                continue;
            } else if (arg[1] == '-' && arg[2] == '-' && arg[3] == '\0') {
                first_child_argv = argv + i + 1;
                break;
            }
        }
        fprintf(stderr, "unexpected argument %s\n", arg);
        print_usage(argv[0], 0);
        return 1;
    }

    /* default termination signal sequence */
    if (!conf.termination_signals_count) {
        conf.termination_signals_count = 2;
        conf.termination_signals = malloc(conf.termination_signals_count * sizeof(int));
        if (!conf.termination_signals) {
            fprintf(stderr, "can't allocate memory: %m\n");
            return 1;
        }
        conf.termination_signals[0] = SIGTERM;
        conf.termination_signals[1] = SIGKILL;
    }

    /* default signals to forward */
    if (!forward_signals_count) {
        forward_signals_count = 1;
        forward_signals = malloc(forward_signals_count * sizeof(int));
        if (!forward_signals) {
            fprintf(stderr, "can't allocate memory: %m\n");
            return 1;
        }
        forward_signals[0] = SIGINT;
    }

    /* block signals during signal handler registration and child spawning */
    sigprocmask(SIG_BLOCK, &conf.set, 0);

    /* register signals to forward */
    for (int i = 0; i < forward_signals_count; ++i) {
        if (register_signal_handler(forward_signals[i])) {
            return 1;
        }
    }
    free(forward_signals); /* not needed anymore */

    /* SIGALRM needed for termination stages */
    if (register_signal_handler(SIGALRM)) {
        return 1;
    }

    /* SIGTERM starts termination chain */
    if (register_signal_handler(SIGTERM)) {
        return 1;
    }

    /* get and test procfs-file to read children from */
    const char* proc_children_format = "/proc/%d/task/%d/children";
    int n = snprintf(NULL, 0, proc_children_format, pid, pid);
    if (n >= 0) {
        conf.proc_children_path = malloc(n * sizeof(char));
        if (!conf.proc_children_path) {
            fprintf(stderr, "can't allocate memory: %m\n");
            return 1;
        }
        n = snprintf(conf.proc_children_path, n + 1, proc_children_format, pid, pid);
    }
    if (n < 0) {
        fprintf(stderr, "snprintf failed: %m\n");
        return 1;
    }
    FILE* f = fopen(conf.proc_children_path, "r");
    if (!f) {
        fprintf(stderr, "can't open `%s': %m\n", conf.proc_children_path);
        return 1;
    }
    fclose(f);

    /* everything ok so far, now spawn the children */
    if (!spawn_children(first_child_argv)) {
        fprintf(stderr, "no children to spawn\n");
        return 1;
    }

    /* unblock signals after signal handler registration and child spawning */
    sigprocmask(SIG_UNBLOCK, &conf.set, 0);

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
                if (!conf.termination_stage) {
                    terminate_children();
                }
            }
        } else if (pid > 0 && (WIFEXITED(stat) | WIFSIGNALED(stat))) {
            int child_rc;
            if (WIFSIGNALED(stat)) {
                child_rc = 128 + WTERMSIG(stat);
            } else {
                child_rc = WEXITSTATUS(stat);
            }
            debug("process %d exited with %d\n", pid, child_rc);
            if (!rc) {
                rc = child_rc;
            }
            if (!conf.termination_stage) {
                terminate_children();
            }
        }
    }

    free(conf.proc_children_path);

    return rc;
}
