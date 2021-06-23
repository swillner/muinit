# muinit

Lightweight subprocess supervisor:

- minimal 'init', e.g. for docker containers
- forwards signals to subprocesses
- reaps zombie subprocesses
- gracefully terminates all subprocesses after one exited

Though several sophisticated subprocess supervisors exist (e.g.
[supervisord](http://supervisord.org/), these are often heavy and
based on scripting languages. More lightweight variants of miss some
features useful for Docker containers. See the great blog article
["Choosing an init process for multi-process
containers"](https://ahmet.im/blog/minimal-init-process-for-containers/)
for what such a supervisor should implement.

## Installation

Download the `muinit` binary from the [releases
page](https://github.com/swillner/muinit/releases/latest) or see below
to build yourself.

### Building

Just build the `muinit` binary using

```
make
```

## Usage

Just call the `muinit` binary with the subprocess commands and their
arguments separated by `---`, e.g. `muinit --- dnsmasq -k --- sshd
-D`.

```
Usage:
  muinit [OPTIONS] --- COMMANDS

OPTIONS
  -h           show help message
  -k SIGNALS   signals to iterate over in subprocess termination
               (comma-separated list of their numbers)
               default: SIGTERM,SIGKILL
  -s SIGNALS   signals to forward to subprocesses (comma-separated numbers)
               default: SIGINT
  -t TIMEOUT   set subprocess termination stage timeout in seconds
               default: 2s

COMMANDS
     Subprocesses to be spawned and their arguments are given after the
     first '---' and are separated by '---' (do not include quotation marks).
     Though muinit emulates an init session, try not to have subprocesses go
     into background ('daemonize') if possible.

SIGNAL FORWARDING
     Signals given via the `-s' option (and that can be caught) are forwarded
     to subprocesses. Special cases are SIGALRM, which is used by muinit itself,
     and SIGTERM, which resets the termination steps and is then forwarded.
     The SIGNALS option values must be lists of comma-separated numbers of the
     signals (run `kill -L' to see a list)

SUBPROCESS TERMINATION
     Once a subprocess terminates (failing or successfully), muinit tries to
     gracefully terminate the other subprocesses. This is done in several
     successive steps until all children have terminated. The steps are defined
     by the signal send in each respective step as given via the `-k' option
     (default: SIGTERM,SIGKILL). The timeout to wait after each step before
     trying the next one can be given via the `-t' option (default: 2s)

EXIT STATUS
    Internal errors cause an exit status of 1. Otherwise the exit status equals
    that of the first failed subprocess or 0 if all subprocesses succeed.
```

### Example Dockerfile snippet

I use `muinit` as a lightweight supervisor in Docker containers to
logically boundly processes into the same container. For example, for
setting up a simple container with a static website and SFTP access to
edit it, I include in the respective Dockerfile:

```
ENTRYPOINT [ "/usr/sbin/muinit" ]

CMD [ \
    "---", "/usr/sbin/lighttpd", "-D", "-f", "/etc/lighttpd/lighttpd.conf", \
    "---", "/usr/sbin/dropbear", "-EFjksw", "-I", "120", "-G", "user", "-c", "/usr/lib/ssh/sftp-server" \
]
```
