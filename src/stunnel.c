/*
 *   stunnel       TLS offloading and load-balancing proxy
 *   Copyright (C) 1998-2019 Michal Trojnara <Michal.Trojnara@stunnel.org>
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by the
 *   Free Software Foundation; either version 2 of the License, or (at your
 *   option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *   See the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, see <http://www.gnu.org/licenses>.
 *
 *   Linking stunnel statically or dynamically with other modules is making
 *   a combined work based on stunnel. Thus, the terms and conditions of
 *   the GNU General Public License cover the whole combination.
 *
 *   In addition, as a special exception, the copyright holder of stunnel
 *   gives you permission to combine stunnel with free software programs or
 *   libraries that are released under the GNU LGPL and with code included
 *   in the standard release of OpenSSL under the OpenSSL License (or
 *   modified versions of such code, with unchanged license). You may copy
 *   and distribute such a system following the terms of the GNU GPL for
 *   stunnel and the licenses of the other code concerned.
 *
 *   Note that people who make modified versions of stunnel are not obligated
 *   to grant this special exception for their modified versions; it is their
 *   choice whether to do so. The GNU General Public License gives permission
 *   to release a modified version without this exception; this exception
 *   also makes it possible to release a modified version which carries
 *   forward this exception.
 */

#include "common.h"
#include "prototypes.h"

/* http://www.openssl.org/support/faq.html#PROG2 */
#ifdef USE_WIN32

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic push
#endif /* __GNUC__>=4.6 */
#pragma GCC diagnostic ignored "-Wpedantic"
#endif /* __GNUC__ */

#include <openssl/applink.c>

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic pop
#endif /* __GNUC__>=4.6 */
#endif /* __GNUC__ */

#endif /* USE_WIN32 */

/**************************************** prototypes */

#ifdef __INNOTEK_LIBC__
struct sockaddr_un {
    u_char  sun_len;             /* sockaddr len including null */
    u_char  sun_family;          /* AF_OS2 or AF_UNIX */
    char    sun_path[108];       /* path name */
};
#endif

#if !defined(USE_WIN32) && !defined(USE_OS2)
NOEXPORT void pid_status_nohang(const char *);
NOEXPORT void status_info(int, int, const char *);
#endif
NOEXPORT int accept_connection(SERVICE_OPTIONS *, unsigned);
NOEXPORT int exec_connect_start(void);
NOEXPORT void unbind_port(SERVICE_OPTIONS *, unsigned);
NOEXPORT SOCKET bind_port(SERVICE_OPTIONS *, int, unsigned);
#ifdef HAVE_CHROOT
NOEXPORT int change_root(void);
#endif
NOEXPORT int signal_pipe_init(void);
NOEXPORT int signal_pipe_dispatch(void);
NOEXPORT char *signal_name(int);

/**************************************** global variables */

static SOCKET signal_pipe[2]={INVALID_SOCKET, INVALID_SOCKET};

#ifndef USE_FORK
int max_clients=0;
/* -1 before a valid config is loaded, then the current number of clients */
int num_clients=-1;
#endif
s_poll_set *fds; /* file descriptors of listening sockets */
int systemd_fds; /* number of file descriptors passed by systemd */
int listen_fds_start; /* base for systemd-provided file descriptors */

/**************************************** startup */

void main_init() { /* one-time initialization */
#ifdef USE_SYSTEMD
    int i;

    systemd_fds=sd_listen_fds(1);
    if(systemd_fds<0)
        fatal("systemd initialization failed");
    listen_fds_start=SD_LISTEN_FDS_START;
    /* set non-blocking mode on systemd file descriptors */
    for(i=0; i<systemd_fds; ++i)
        set_nonblock(listen_fds_start+i, 1);
#else
    systemd_fds=0; /* no descriptors received */
    listen_fds_start=3; /* the value is not really important */
#endif
    /* basic initialization contains essential functions required for logging
     * subsystem to function properly, thus all errors here are fatal */
    if(ssl_init()) /* initialize TLS library */
        fatal("TLS initialization failed");
    if(sthreads_init()) /* initialize critical sections & TLS callbacks */
        fatal("Threads initialization failed");
    options_defaults();
    options_apply();
#ifndef USE_FORK
    get_limits(); /* required by setup_fd() */
#endif
    fds=s_poll_alloc();
    if(signal_pipe_init())
        fatal("Signal pipe initialization failed: "
            "check your personal firewall");
    stunnel_info(LOG_NOTICE);
    if(systemd_fds>0)
        s_log(LOG_INFO, "Systemd socket activation: %d descriptors received",
            systemd_fds);
}

/* return values:
   0 - configuration accepted
   1 - error
   2 - information printed
*/

    /* configuration-dependent initialization */
int main_configure(char *arg1, char *arg2) {
    int cmdline_status;

    cmdline_status=options_cmdline(arg1, arg2);
    if(cmdline_status) /* cannot proceed */
        return cmdline_status;
    options_apply();
    str_canary_init(); /* needs prng initialization from options_cmdline */
    /* log_open(SINK_SYSLOG) must be called before change_root()
     * to be able to access /dev/log socket */
    log_open(SINK_SYSLOG);
    if(bind_ports())
        return 1;

#ifdef HAVE_CHROOT
    /* change_root() must be called before drop_privileges()
     * since chroot() needs root privileges */
    if(change_root())
        return 1;
#endif /* HAVE_CHROOT */

    if(drop_privileges(1))
        return 1;

    /* log_open(SINK_OUTFILE) must be called after drop_privileges()
     * or logfile rotation won't be possible */
    if(log_open(SINK_OUTFILE))
        return 1;
#ifndef USE_FORK
    num_clients=0; /* the first valid config */
#endif
    /* log_flush(LOG_MODE_CONFIGURED) must be called before daemonize()
     * since daemonize() invalidates stderr */
    log_flush(LOG_MODE_CONFIGURED);
    return 0;
}

int drop_privileges(int critical) {
#if defined(USE_WIN32) || defined(__vms) || defined(USE_OS2)
    (void)critical; /* squash the unused parameter warning */
#else
#ifdef HAVE_SETGROUPS
    gid_t gr_list[1];
#endif

    /* set uid and gid */
    if(service_options.gid) {
        if(setgid(service_options.gid) && critical) {
            sockerror("setgid");
            return 1;
        }
#ifdef HAVE_SETGROUPS
        gr_list[0]=service_options.gid;
        if(setgroups(1, gr_list) && critical) {
            sockerror("setgroups");
            return 1;
        }
#endif
    }
    if(service_options.uid) {
        if(setuid(service_options.uid) && critical) {
            sockerror("setuid");
            return 1;
        }
    }
#endif /* standard Unix */
    return 0;
}

void main_cleanup() {
    unbind_ports();
    s_poll_free(fds);
    fds=NULL;
#if 0
    str_stats(); /* main thread allocation tracking */
#endif
    log_flush(LOG_MODE_ERROR);
    log_close(SINK_SYSLOG|SINK_OUTFILE);
}

/**************************************** Unix-specific initialization */

#if !defined(USE_WIN32) && !defined(USE_OS2)

NOEXPORT void pid_status_nohang(const char *info) {
    int pid, status;

#ifdef HAVE_WAITPID /* POSIX.1 */
    s_log(LOG_DEBUG, "Retrieving pid statuses with waitpid()");
    while((pid=waitpid(-1, &status, WNOHANG))>0)
        status_info(pid, status, info);
#elif defined(HAVE_WAIT4) /* 4.3BSD */
    s_log(LOG_DEBUG, "Retrieving pid statuses with wait4()");
    while((pid=wait4(-1, &status, WNOHANG, NULL))>0)
        status_info(pid, status, info);
#else /* no support for WNOHANG */
    pid_status_hang(info);
#endif
}

void pid_status_hang(const char *info) {
    int pid, status;

    s_log(LOG_DEBUG, "Retrieving a pid status with wait()");
    if((pid=wait(&status))>0)
        status_info(pid, status, info);
}

NOEXPORT void status_info(int pid, int status, const char *info) {
#ifdef WIFSIGNALED
    if(WIFSIGNALED(status)) {
        char *sig_name=signal_name(WTERMSIG(status));
        s_log(LOG_INFO, "%s %d terminated on %s", info, pid, sig_name);
        str_free(sig_name);
    } else {
        s_log(LOG_INFO, "%s %d finished with code %d",
            info, pid, WEXITSTATUS(status));
    }
#else
    s_log(LOG_INFO, "%s %d finished with status %d", info, pid, status);
#endif
}

#endif /* !defined(USE_WIN32) && !defined(USE_OS2) */

/**************************************** main loop accepting connections */

void daemon_loop(void) {
    if(cron_init()) { /* initialize periodic events */
        s_log(LOG_CRIT, "Cron initialization failed");
        exit(1);
    }
    if(exec_connect_start()) {
        s_log(LOG_CRIT, "Failed to start exec+connect services");
        exit(1);
    }
    while(1) {
        int temporary_lack_of_resources=0;
        int num=s_poll_wait(fds, -1, -1);
        if(num>=0) {
            SERVICE_OPTIONS *opt;
            s_log(LOG_DEBUG, "Found %d ready file descriptor(s)", num);
            if(service_options.log_level>=LOG_DEBUG) /* performance optimization */
                s_poll_dump(fds, LOG_DEBUG);
            if(s_poll_canread(fds, signal_pipe[0]))
                if(signal_pipe_dispatch()) /* SIGNAL_TERMINATE or error */
                    break; /* terminate daemon_loop */
            for(opt=service_options.next; opt; opt=opt->next) {
                unsigned i;
                for(i=0; i<opt->local_addr.num; ++i) {
                    SOCKET fd=opt->local_fd[i];
                    if(fd!=INVALID_SOCKET &&
                            s_poll_canread(fds, fd) &&
                            accept_connection(opt, i))
                        temporary_lack_of_resources=1;
                }
            }
        } else {
            log_error(LOG_NOTICE, get_last_socket_error(),
                "daemon_loop: s_poll_wait");
            temporary_lack_of_resources=1;
        }
        if(temporary_lack_of_resources) {
            s_log(LOG_NOTICE,
                "Accepting new connections suspended for 1 second");
            sleep(1); /* to avoid log trashing */
        }
    }
    leak_table_utilization();
}

    /* return 1 when a short delay is needed before another try */
NOEXPORT int accept_connection(SERVICE_OPTIONS *opt, unsigned i) {
    SOCKADDR_UNION addr;
    char *from_address;
    SOCKET s, fd=opt->local_fd[i];
    socklen_t addrlen;

    addrlen=sizeof addr;
    for(;;) {
        s=s_accept(fd, &addr.sa, &addrlen, 1, "local socket");
        if(s!=INVALID_SOCKET) /* success! */
            break;
        switch(get_last_socket_error()) {
            case S_EINTR: /* interrupted by a signal */
                break; /* retry now */
            case S_EMFILE:
#ifdef S_ENFILE
            case S_ENFILE:
#endif
#ifdef S_ENOBUFS
            case S_ENOBUFS:
#endif
#ifdef S_ENOMEM
            case S_ENOMEM:
#endif
                return 1; /* temporary lack of resources */
            default:
                return 0; /* any other error */
        }
    }
    from_address=s_ntop(&addr, addrlen);
    s_log(LOG_DEBUG, "Service [%s] accepted (FD=%ld) from %s",
        opt->servname, (long)s, from_address);
    str_free(from_address);
#ifdef USE_FORK
    RAND_add("", 1, 0.0); /* each child needs a unique entropy pool */
#else
    if(max_clients && num_clients>=max_clients) {
        s_log(LOG_WARNING, "Connection rejected: too many clients (>=%d)",
            max_clients);
        closesocket(s);
        return 0;
    }
#endif
#ifndef USE_FORK
    service_up_ref(opt);
#endif
    if(create_client(fd, s, alloc_client_session(opt, s, s))) {
        s_log(LOG_ERR, "Connection rejected: create_client failed");
        closesocket(s);
#ifndef USE_FORK
        service_free(opt);
#endif
        return 0;
    }
    return 0;
}

/**************************************** initialization helpers */

NOEXPORT int exec_connect_start(void) {
    SERVICE_OPTIONS *opt;

    for(opt=service_options.next; opt; opt=opt->next) {
        if(opt->exec_name && opt->connect_addr.names) {
            s_log(LOG_DEBUG, "Starting exec+connect service [%s]",
                opt->servname);
#ifndef USE_FORK
            service_up_ref(opt);
#endif
            if(create_client(INVALID_SOCKET, INVALID_SOCKET,
                    alloc_client_session(opt, INVALID_SOCKET, INVALID_SOCKET))) {
                s_log(LOG_ERR, "Failed to start exec+connect service [%s]",
                    opt->servname);
#ifndef USE_FORK
                service_free(opt);
#endif
                return 1; /* fatal error */
            }
        }
    }
    return 0; /* OK */
}

/* clear fds, close old ports */
void unbind_ports(void) {
    SERVICE_OPTIONS *opt;

    s_poll_init(fds);
    s_poll_add(fds, signal_pipe[0], 1, 0);

    CRYPTO_THREAD_write_lock(stunnel_locks[LOCK_SECTIONS]);

    opt=service_options.next;
    service_options.next=NULL;
    service_free(&service_options);

    while(opt) {
        unsigned i;
        s_log(LOG_DEBUG, "Unbinding service [%s]", opt->servname);
        for(i=0; i<opt->local_addr.num; ++i)
            unbind_port(opt, i);
        /* exec+connect service */
        if(opt->exec_name && opt->connect_addr.names) {
            /* create exec+connect services             */
            /* FIXME: this is just a crude workaround   */
            /*        is it better to kill the service? */
            /* FIXME: this won't work with FORK threads */
            opt->option.retry=0;
        }
        /* purge session cache of the old SSL_CTX object */
        /* this workaround won't be needed anymore after */
        /* delayed deallocation calls SSL_CTX_free()     */
        if(opt->ctx)
            SSL_CTX_flush_sessions(opt->ctx,
                (long)time(NULL)+opt->session_timeout+1);
        s_log(LOG_DEBUG, "Service [%s] closed", opt->servname);

        {
            SERVICE_OPTIONS *garbage=opt;
            opt=opt->next;
            garbage->next=NULL;
            service_free(garbage);
        }
    }

    CRYPTO_THREAD_unlock(stunnel_locks[LOCK_SECTIONS]);
}

NOEXPORT void unbind_port(SERVICE_OPTIONS *opt, unsigned i) {
    SOCKET fd=opt->local_fd[i];
#ifdef HAVE_STRUCT_SOCKADDR_UN
    SOCKADDR_UNION *addr=opt->local_addr.addr+i;
    struct stat sb; /* buffer for lstat() */
#endif

    if(fd==INVALID_SOCKET)
        return;
    opt->local_fd[i]=INVALID_SOCKET;

    if(fd<(SOCKET)listen_fds_start ||
            fd>=(SOCKET)(listen_fds_start+systemd_fds))
        closesocket(fd);
    s_log(LOG_DEBUG, "Service [%s] closed (FD=%ld)",
        opt->servname, (long)fd);

#ifdef HAVE_STRUCT_SOCKADDR_UN
    if(addr->sa.sa_family==AF_UNIX) {
        if(lstat(addr->un.sun_path, &sb))
            sockerror(addr->un.sun_path);
        else if(!S_ISSOCK(sb.st_mode))
            s_log(LOG_ERR, "Not a socket: %s",
                addr->un.sun_path);
        else if(unlink(addr->un.sun_path))
            sockerror(addr->un.sun_path);
        else
            s_log(LOG_DEBUG, "Socket removed: %s",
                addr->un.sun_path);
    }
#endif
}

/* open new ports, update fds */
int bind_ports(void) {
    SERVICE_OPTIONS *opt;
    int listening_section;

#ifdef USE_LIBWRAP
    /* execute after options_cmdline() to know service_options.next,
     * but as early as possible to avoid leaking file descriptors */
    /* retry on each bind_ports() in case stunnel.conf was reloaded
       without "libwrap = no" */
    libwrap_init();
#endif /* USE_LIBWRAP */

    s_poll_init(fds);
    s_poll_add(fds, signal_pipe[0], 1, 0);

    /* allow clean unbind_ports() even though
       bind_ports() was not fully performed */
    for(opt=service_options.next; opt; opt=opt->next) {
        unsigned i;
        for(i=0; i<opt->local_addr.num; ++i)
            opt->local_fd[i]=INVALID_SOCKET;
    }

    listening_section=0;
    for(opt=service_options.next; opt; opt=opt->next) {
        opt->bound_ports=0;
        if(opt->local_addr.num) { /* ports to bind for this service */
            unsigned i;
            s_log(LOG_DEBUG, "Binding service [%s]", opt->servname);
            for(i=0; i<opt->local_addr.num; ++i) {
                SOCKET fd;
                fd=bind_port(opt, listening_section, i);
                opt->local_fd[i]=fd;
                if(fd!=INVALID_SOCKET) {
                    s_poll_add(fds, fd, 1, 0);
                    ++opt->bound_ports;
                }
            }
            if(!opt->bound_ports) {
                s_log(LOG_ERR, "Binding service [%s] failed", opt->servname);
                return 1;
            }
            ++listening_section;
        } else if(opt->exec_name && opt->connect_addr.names) {
            s_log(LOG_DEBUG, "Skipped exec+connect service [%s]", opt->servname);
#ifndef OPENSSL_NO_TLSEXT
        } else if(!opt->option.client && opt->sni) {
            s_log(LOG_DEBUG, "Skipped SNI slave service [%s]", opt->servname);
#endif
        } else { /* each service must define two endpoints */
            s_log(LOG_ERR, "Invalid service [%s]", opt->servname);
            return 1;
        }
    }
    if(listening_section<systemd_fds) {
        s_log(LOG_ERR,
            "Too many listening file descriptors received from systemd, got %d",
            systemd_fds);
        return 1;
    }
    return 0; /* OK */
}

NOEXPORT SOCKET bind_port(SERVICE_OPTIONS *opt, int listening_section, unsigned i) {
    SOCKET fd;
    SOCKADDR_UNION *addr=opt->local_addr.addr+i;
    char *local_address;
#ifdef HAVE_STRUCT_SOCKADDR_UN
    struct stat sb; /* buffer for lstat() */
#endif

    if(listening_section<systemd_fds) {
        fd=(SOCKET)(listen_fds_start+listening_section);
        s_log(LOG_DEBUG,
            "Listening file descriptor received from systemd (FD=%ld)",
            (long)fd);
    } else {
        fd=s_socket(addr->sa.sa_family, SOCK_STREAM, 0, 1, "accept socket");
        if(fd==INVALID_SOCKET)
            return INVALID_SOCKET;
        s_log(LOG_DEBUG, "Listening file descriptor created (FD=%ld)",
            (long)fd);
    }

    if(socket_options_set(opt, fd, 0)<0) {
        closesocket(fd);
        return INVALID_SOCKET;
    }

    /* local socket can't be unnamed */
    local_address=s_ntop(addr, addr_len(addr));
    /* we don't bind or listen on a socket inherited from systemd */
    if(listening_section>=systemd_fds) {
        if(bind(fd, &addr->sa, addr_len(addr))) {
            int err=get_last_socket_error();
            s_log(LOG_NOTICE, "Binding service [%s] to %s: %s (%d)",
                opt->servname, local_address, s_strerror(err), err);
            str_free(local_address);
            closesocket(fd);
            return INVALID_SOCKET;
        }
        if(listen(fd, SOMAXCONN)) {
            sockerror("listen");
            str_free(local_address);
            closesocket(fd);
            return INVALID_SOCKET;
        }
    }

#ifdef HAVE_STRUCT_SOCKADDR_UN
    /* chown the UNIX socket, errors are ignored */
    if(addr->sa.sa_family==AF_UNIX &&
            (opt->uid || opt->gid)) {
        /* fchown() does *not* work on UNIX sockets */
        if(!lchown(addr->un.sun_path, opt->uid, opt->gid))
            s_log(LOG_DEBUG,
                "Socket chown succeeded: %s, UID=%u, GID=%u",
                addr->un.sun_path,
                (unsigned)opt->uid, (unsigned)opt->gid);
        else if(lstat(addr->un.sun_path, &sb))
            sockerror(addr->un.sun_path);
        else if(sb.st_uid==opt->uid && sb.st_gid==opt->gid)
            s_log(LOG_DEBUG,
                "Socket chown unneeded: %s, UID=%u, GID=%u",
                addr->un.sun_path,
                (unsigned)opt->uid, (unsigned)opt->gid);
        else
            s_log(LOG_ERR, "Socket chown failed: %s, UID=%u, GID=%u",
                addr->un.sun_path,
                (unsigned)opt->uid, (unsigned)opt->gid);
    }
#endif

    s_log(LOG_INFO, "Service [%s] (FD=%ld) bound to %s",
        opt->servname, (long)fd, local_address);
    str_free(local_address);
    return fd;
}

#ifdef HAVE_CHROOT
NOEXPORT int change_root(void) {
    if(!global_options.chroot_dir)
        return 0;
    if(chroot(global_options.chroot_dir)) {
        sockerror("chroot");
        return 1;
    }
    if(chdir("/")) {
        sockerror("chdir");
        return 1;
    }
    s_log(LOG_NOTICE, "Switched to chroot directory: %s", global_options.chroot_dir);
    return 0;
}
#endif /* HAVE_CHROOT */

/**************************************** signal pipe handling */

NOEXPORT int signal_pipe_init(void) {
#ifdef USE_WIN32
    if(make_sockets(signal_pipe))
        return 1;
#elif defined(__INNOTEK_LIBC__)
    /* Innotek port of GCC can not use select on a pipe:
     * use local socket instead */
    struct sockaddr_un un;
    fd_set set_pipe;
    int pipe_in;

    FD_ZERO(&set_pipe);
    signal_pipe[0]=s_socket(PF_OS2, SOCK_STREAM, 0, 0, "socket#1");
    signal_pipe[1]=s_socket(PF_OS2, SOCK_STREAM, 0, 0, "socket#2");

    /* connect the two endpoints */
    memset(&un, 0, sizeof un);
    un.sun_len=sizeof un;
    un.sun_family=AF_OS2;
    sprintf(un.sun_path, "\\socket\\stunnel-%u", getpid());
    /* make the first endpoint listen */
    bind(signal_pipe[0], (struct sockaddr *)&un, sizeof un);
    listen(signal_pipe[0], 1);
    connect(signal_pipe[1], (struct sockaddr *)&un, sizeof un);
    FD_SET(signal_pipe[0], &set_pipe);
    if(select(signal_pipe[0]+1, &set_pipe, NULL, NULL, NULL)>0) {
        pipe_in=signal_pipe[0];
        signal_pipe[0]=s_accept(signal_pipe[0], NULL, 0, 0, "accept");
        closesocket(pipe_in);
    } else {
        sockerror("select");
        return 1;
    }
#else /* Unix */
    if(s_pipe(signal_pipe, 1, "signal_pipe"))
        return 1;
#endif /* USE_WIN32 */
    return 0;
}

#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif /* __GNUC__>=4.6 */
#endif /* __GNUC__ */
void signal_post(uint8_t sig) {
    /* no meaningful way here to handle the result */
    writesocket(signal_pipe[1], (char *)&sig, 1);
}
#ifdef __GNUC__
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic pop
#endif /* __GNUC__>=4.6 */
#endif /* __GNUC__ */

/* make a single attempt to dispatch a signal from the signal pipe */
/* return 1 on SIGNAL_TERMINATE or a fatal error, 0 otherwise */
NOEXPORT int signal_pipe_dispatch(void) {
    uint8_t sig=0xff;
    ssize_t num;
    char *sig_name;

    s_log(LOG_DEBUG, "Dispatching a signal from the signal pipe");
    num=readsocket(signal_pipe[0], (char *)&sig, 1);
    if(num!=1) {
        if(num) {
            if(get_last_socket_error()==S_EWOULDBLOCK) {
                s_log(LOG_DEBUG, "Signal pipe is empty");
                return 0;
            }
            sockerror("signal pipe read");
        } else {
            s_log(LOG_ERR, "Signal pipe closed");
        }
        s_poll_remove(fds, signal_pipe[0]);
        closesocket(signal_pipe[0]);
        closesocket(signal_pipe[1]);
        if(signal_pipe_init()) {
            s_log(LOG_ERR,
                "Signal pipe reinitialization failed; terminating");
            return 1;
        }
        s_poll_add(fds, signal_pipe[0], 1, 0);
        s_log(LOG_ERR, "Signal pipe reinitialized");
        return 0;
    }

    switch(sig) {
#ifndef USE_WIN32
    case SIGCHLD:
        s_log(LOG_DEBUG, "Processing SIGCHLD");
#ifdef USE_FORK
        pid_status_nohang("Process"); /* client process */
#else /* USE_UCONTEXT || USE_PTHREAD */
        pid_status_nohang("Child process"); /* 'exec' process */
#endif /* defined USE_FORK */
        return 0;
#endif /* !defind USE_WIN32 */
    case SIGNAL_RELOAD_CONFIG:
        s_log(LOG_DEBUG, "Processing SIGNAL_RELOAD_CONFIG");
        if(options_parse(CONF_RELOAD)) {
            s_log(LOG_ERR, "Failed to reload the configuration file");
        } else {
#ifdef HAVE_CHROOT
            struct stat sb;
#endif /* HAVE_CHROOT */
            unbind_ports();
            log_flush(LOG_MODE_BUFFER);
#ifdef HAVE_CHROOT
            /* we don't close SINK_SYSLOG if chroot is enabled and
             * there is no /dev/log inside it, which could allow
             * openlog(3) to reopen the syslog socket later */
            if(global_options.chroot_dir && stat("/dev/log", &sb))
                log_close(SINK_OUTFILE);
            else
#endif /* HAVE_CHROOT */
                log_close(SINK_SYSLOG|SINK_OUTFILE);
            /* there is no race condition here:
             * client threads are not allowed to use global options */
            options_free();
            options_apply();
            /* we hope that a sane openlog(3) implementation won't
             * attempt to reopen /dev/log if it's already open */
            log_open(SINK_SYSLOG|SINK_OUTFILE);
            log_flush(LOG_MODE_CONFIGURED);
            ui_config_reloaded();
            if(bind_ports()) {
                /* FIXME: handle the error */
            }
            if(exec_connect_start()) {
                /* FIXME: handle the error */
            }
        }
        return 0;
    case SIGNAL_REOPEN_LOG:
        s_log(LOG_DEBUG, "Processing SIGNAL_REOPEN_LOG");
        log_flush(LOG_MODE_BUFFER);
        log_close(SINK_OUTFILE);
        log_open(SINK_OUTFILE);
        log_flush(LOG_MODE_CONFIGURED);
        s_log(LOG_NOTICE, "Log file reopened");
        return 0;
    case SIGNAL_TERMINATE:
        s_log(LOG_DEBUG, "Processing SIGNAL_TERMINATE");
        s_log(LOG_NOTICE, "Terminated");
        return 1;
    default:
        sig_name=signal_name(sig);
        s_log(LOG_ERR, "Received %s; terminating", sig_name);
        str_free(sig_name);
        return 1;
    }
}

/**************************************** signal name decoding */

#define check_signal(s) if(signum==s) return str_dup(#s);

NOEXPORT char *signal_name(int signum) {
#ifdef SIGHUP
    check_signal(SIGHUP)
#endif
#ifdef SIGINT
    check_signal(SIGINT)
#endif
#ifdef SIGQUIT
    check_signal(SIGQUIT)
#endif
#ifdef SIGILL
    check_signal(SIGILL)
#endif
#ifdef SIGTRAP
    check_signal(SIGTRAP)
#endif
#ifdef SIGABRT
    check_signal(SIGABRT)
#endif
#ifdef SIGIOT
    check_signal(SIGIOT)
#endif
#ifdef SIGBUS
    check_signal(SIGBUS)
#endif
#ifdef SIGFPE
    check_signal(SIGFPE)
#endif
#ifdef SIGKILL
    check_signal(SIGKILL)
#endif
#ifdef SIGUSR1
    check_signal(SIGUSR1)
#endif
#ifdef SIGSEGV
    check_signal(SIGSEGV)
#endif
#ifdef SIGUSR2
    check_signal(SIGUSR2)
#endif
#ifdef SIGPIPE
    check_signal(SIGPIPE)
#endif
#ifdef SIGALRM
    check_signal(SIGALRM)
#endif
#ifdef SIGTERM
    check_signal(SIGTERM)
#endif
#ifdef SIGSTKFLT
    check_signal(SIGSTKFLT)
#endif
#ifdef SIGCHLD
    check_signal(SIGCHLD)
#endif
#ifdef SIGCONT
    check_signal(SIGCONT)
#endif
#ifdef SIGSTOP
    check_signal(SIGSTOP)
#endif
#ifdef SIGTSTP
    check_signal(SIGTSTP)
#endif
#ifdef SIGTTIN
    check_signal(SIGTTIN)
#endif
#ifdef SIGTTOU
    check_signal(SIGTTOU)
#endif
#ifdef SIGURG
    check_signal(SIGURG)
#endif
#ifdef SIGXCPU
    check_signal(SIGXCPU)
#endif
#ifdef SIGXFSZ
    check_signal(SIGXFSZ)
#endif
#ifdef SIGVTALRM
    check_signal(SIGVTALRM)
#endif
#ifdef SIGPROF
    check_signal(SIGPROF)
#endif
#ifdef SIGWINCH
    check_signal(SIGWINCH)
#endif
#ifdef SIGIO
    check_signal(SIGIO)
#endif
#ifdef SIGPOLL
    check_signal(SIGPOLL)
#endif
#ifdef SIGLOST
    check_signal(SIGLOST)
#endif
#ifdef SIGPWR
    check_signal(SIGPWR)
#endif
#ifdef SIGSYS
    check_signal(SIGSYS)
#endif
#ifdef SIGUNUSED
    check_signal(SIGUNUSED)
#endif
    return str_printf("signal %d", signum);
}

/**************************************** log build details */

void stunnel_info(int level) {
    s_log(level, "stunnel " STUNNEL_VERSION " on " HOST " platform");
    if(strcmp(OPENSSL_VERSION_TEXT, OpenSSL_version(OPENSSL_VERSION))) {
        s_log(level, "Compiled with " OPENSSL_VERSION_TEXT);
        s_log(level, "Running  with %s", OpenSSL_version(OPENSSL_VERSION));
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        if((OPENSSL_version_major()<<8 | OPENSSL_version_minor()) !=
                OPENSSL_VERSION_NUMBER>>20)
#else /* OpenSSL version < 3.0.0 */
        if(OpenSSL_version_num()>>12 != OPENSSL_VERSION_NUMBER>>12)
#endif /* OpenSSL version >= 3.0.0 */
            s_log(level, "Update OpenSSL shared libraries or rebuild stunnel");
    } else {
        s_log(level, "Compiled/running with " OPENSSL_VERSION_TEXT);
    }

    s_log(level,

        "Threading:"
#ifdef USE_UCONTEXT
        "UCONTEXT"
#endif
#ifdef USE_PTHREAD
        "PTHREAD"
#endif
#ifdef USE_WIN32
        "WIN32"
#endif
#ifdef USE_FORK
        "FORK"
#endif

        " Sockets:"
#ifdef USE_POLL
        "POLL"
#else /* defined(USE_POLL) */
        "SELECT"
#endif /* defined(USE_POLL) */
        ",IPv%c"
#ifdef USE_SYSTEMD
        ",SYSTEMD"
#endif /* defined(USE_SYSTEMD) */

        " TLS:"
#ifndef OPENSSL_NO_ENGINE
#define TLS_FEATURE_FOUND
        "ENGINE"
#endif /* !defined(OPENSSL_NO_ENGINE) */
#ifdef USE_FIPS
#ifdef TLS_FEATURE_FOUND
        ","
#else
#define TLS_FEATURE_FOUND
#endif
        "FIPS"
#endif /* defined(USE_FIPS) */
#ifndef OPENSSL_NO_OCSP
#ifdef TLS_FEATURE_FOUND
        ","
#else
#define TLS_FEATURE_FOUND
#endif
        "OCSP"
#endif /* !defined(OPENSSL_NO_OCSP) */
#ifndef OPENSSL_NO_PSK
#ifdef TLS_FEATURE_FOUND
        ","
#else
#define TLS_FEATURE_FOUND
#endif
        "PSK"
#endif /* !defined(OPENSSL_NO_PSK) */
#ifndef OPENSSL_NO_TLSEXT
#ifdef TLS_FEATURE_FOUND
        ","
#else
#define TLS_FEATURE_FOUND
#endif
        "SNI"
#endif /* !defined(OPENSSL_NO_TLSEXT) */
#ifndef TLS_FEATURE_FOUND
        "NONE"
#endif /* !defined(TLS_FEATURE_FOUND) */

#ifdef USE_LIBWRAP
        " Auth:LIBWRAP"
#endif

        , /* supported IP version parameter */
#if defined(USE_WIN32) && !defined(_WIN32_WCE)
        s_getaddrinfo ? '6' : '4'
#else /* defined(USE_WIN32) */
#if defined(USE_IPv6)
        '6'
#else /* defined(USE_IPv6) */
        '4'
#endif /* defined(USE_IPv6) */
#endif /* defined(USE_WIN32) */
        );
#ifdef errno
#define xstr(a) str(a)
#define str(a) #a
    s_log(LOG_DEBUG, "errno: " xstr(errno));
#endif /* errno */
}

/* end of stunnel.c */
