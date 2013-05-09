/* -*- mode: C; c-basic-offset: 4 -*- */
/* ex: set shiftwidth=4 tabstop=4 expandtab: */
/*
 * Copyright (c) 2013, Georgia Tech Research Corporation
 * All rights reserved.
 *
 * Author(s): Neil T. Dantam <ntd@gatech.edu>
 * Georgia Tech Humanoid Robotics Lab
 * Under Direction of Prof. Mike Stilman <mstilman@cc.gatech.edu>
 *
 *
 * This file is provided under the following "BSD-style" License:
 *
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 *
 */


/*
 * achcop: Watchdog process for ach-using daemons
 */


/* TODO: don't give up for non-catastrophic errors
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/wait.h>
#include "ach.h"
#include "achutil.h"


/* EVENTS:
 *   Message Timeout
 *   Invalid Message
 *   Process Exits
 */

/* Avoding signal races:
 *
 * This programs needs to wait for SIGTERM and child termination.
 * Waiting for children to terminate with wait() would create a race
 * condition on detection of SIGTERMs.  If SIGTERM is received after
 * we check the flag, but before we enter wait(), we would never
 * realize it.  Instead, we block the signals, check the condition,
 * and sigsupend (unblocking the signals).
 */


/*
 * If child returns 0: exit normally
 * If child returns !0: restart it
 * If child terminated by signal: restart it
 * If sigterm received, signal child and wait for child to exit
 */

enum wait_action {
    WAIT_RESTART,   ///< restart the child
    WAIT_SUCCESS,   ///< end successfully
    WAIT_FAIL,      ///< end with failure
    WAIT_TERM,      ///< terminate the child
};


/* CLI options */

static void detach(void);
static void redirect(int fd, const char *file);
static void lock_pid(const char *name, int *fd, FILE **fp);
static void write_pid(FILE *fp, pid_t pid);
static void child_arg( const char ***args, const char *arg, size_t *n);
static void run(FILE *fp_pid, pid_t *pid, const char *file, const char ** args);

static void waitloop( pid_t pid, int *status, int *signalled );

static void start_child(FILE *fp_pid, pid_t *pid, const char *file, const char ** args);


/* Wait for signal to be received, taking care to avoid races
 * Returns the received signal*/
static int wait_for_signal(void);
/* Check flags to see if signal received */
static int check_signal(void);



#ifdef __GNUC__
#define ACHD_ATTR_PRINTF(m,n) __attribute__((format(printf, m, n)))
#else
#define ACHD_ATTR_PRINTF(m,n)
#endif

sig_atomic_t achcop_sigchild_received = 0;


int main( int argc, char **argv ) {
    static struct {
        const char *file_cop_pid;
        const char *file_child_pid;
        const char *file_stderr;
        const char *file_stdout;
        const char **child_args;
        size_t n_child_args;
        int detach;
    } opt = {0};
    static struct {
        pid_t pid_cop;
        pid_t pid_child;
        int fd_cop_pid;
        int fd_child_pid;
        FILE *fp_cop_pid;
        FILE *fp_child_pid;
    } cx = {0};

    /* Parse Options */
    int c;
    opterr = 0;
    while( (c = getopt( argc, argv, "p:p:o:e:vdhH?V")) != -1 ) {
        switch(c) {
        case 'P': opt.file_cop_pid = optarg; break;
        case 'p': opt.file_child_pid = optarg; break;
        case 'o': opt.file_stdout = optarg; break;
        case 'e': opt.file_stderr = optarg; break;
        case 'd': opt.detach = 1; break;
        case 'V':   /* version     */
            ach_print_version("achcop");
            exit(EXIT_SUCCESS);
        case 'v': ach_verbosity++; break;
        case '?':   /* help     */
        case 'h':
        case 'H':
            puts( "Usage: achcop [OPTIONS...] -- child-name [CHILD-OPTIONS]\n"
                  "Watchdog to run and restart ach child processes\n"
                  "\n"
                  "Options:\n"
                  "  -P,                       File for pid of cop process\n"
                  "  -p,                       File for pid of child process\n"
                  "  -d,                       Detach and run in background\n"
                  //"  -s,                       Wait for SIGUSR1 to redirect output and restart child\n"
                  "  -o,                       Redirect stdout to this file\n"
                  "  -e,                       Redirect stderr to this file\n"
                  "  -v,                       Make output more verbose\n"
                  "  -?,                       Give program help list\n"
                  "  -V,                       Print program version\n"
                  "\n"
                  "Examples:\n"
                  "  achcop -P /var/run/myppid -p /var/run/mypid -o /var/log/myout -- my-daemon -xyz"
                  "\n"
                  "Report bugs to <ntd@gatech.edu>"
                );
            exit(EXIT_SUCCESS);
        default:
            child_arg(&opt.child_args, optarg, &opt.n_child_args);
        }
    }
    while( optind < argc ) {
        child_arg(&opt.child_args, argv[optind++], &opt.n_child_args);
    }
    /* Check args */
    if( 0 == opt.n_child_args || NULL == opt.child_args ) {
        ACH_DIE("No child process given\n");
    }
    child_arg(&opt.child_args, NULL, &opt.n_child_args); /* Null-terminate child arg array */

    /* Detach */
    if( opt.detach ) detach();

    /* Open and Lock PID files */
    lock_pid( opt.file_cop_pid, &cx.fd_cop_pid, &cx.fp_cop_pid );
    lock_pid( opt.file_child_pid, &cx.fd_child_pid, &cx.fp_child_pid );

    /* Write parent pid */
    write_pid( cx.fp_cop_pid, cx.pid_cop );

    /* Redirect */
    redirect(STDOUT_FILENO, opt.file_stdout);
    redirect(STDERR_FILENO, opt.file_stderr);

    /* TODO: fork a second child to monitor an ach channel.  Restart
     * first child if second child signals or exits */

    /* Install signal handlers */
    ach_install_sigflag( SIGTERM );
    ach_install_sigflag( SIGINT );
    ach_install_sigflag( SIGCHLD );

    /* Fork child */
    run( cx.fp_child_pid, &cx.pid_child, opt.child_args[0], opt.child_args );

    return 0;
}


static void child_arg(const char ***args, const char *arg, size_t *n) {
    *args = (const char **)realloc( *args, (*n+1)*sizeof(arg) );
    (*args)[(*n)++] = arg;
}

static void detach(void) {
    /* open syslog */
    openlog("achcop", LOG_PID, LOG_DAEMON);

    /* fork */
    pid_t pid1 = fork();
    if( pid1 < 0 ) {
        ACH_DIE( "First fork failed: %s\n", strerror(errno) );
    } else if ( pid1 ) { /* parent */
        exit(EXIT_SUCCESS);
    } /* else child */

    /* set session id to lose our controlling terminal */
    if( setsid() < 0 ) {
        ACH_LOG( LOG_ERR, "Couldn't set sid: %s\n", strerror(errno) );
    }

    /* refork to prevent future controlling ttys */
    pid_t pid2 = fork();
    if( pid2 < 0 ) {
        ACH_LOG( LOG_ERR, "Second fork failed: %s\n", strerror(errno) );
        /* Don't give up */
    } else if ( pid2 ) { /* parent */
        exit(EXIT_SUCCESS);
    } /* else child */

    /* ignore sighup */
    if( SIG_ERR == signal(SIGHUP, SIG_IGN) ) {
        ACH_LOG( LOG_ERR, "Couldn't ignore SIGHUP: %s", strerror(errno) );
    }

    /* cd to root */
    if( chdir("/") ) {
        ACH_LOG( LOG_ERR, "Couldn't cd to /: %s", strerror(errno) );
    }

    /* close stdin */
    if( close(STDIN_FILENO) ) {
        ACH_LOG( LOG_ERR, "Couldn't close stdin: %s", strerror(errno) );
    }
}

static void redirect(int fd, const char *name) {
    if( NULL == name ) return;
    /* open */
    int fd2 = open( name, O_RDWR|O_CREAT, 0664 );
    if( fd2 < 0 ) {
        ACH_LOG( LOG_ERR, "Could not open file %s: %s\n", name, strerror(errno) );
    }

    /* dup */
    if( dup2(fd2, fd) ) {
        ACH_LOG( LOG_ERR, "Could not dup output to %s: %s\n", name, strerror(errno) );
    }
}

static void lock_pid(const char *name, int *fd, FILE **fp) {
    *fd = -1;
    *fp = NULL;
    if( NULL == name ) return;
    /* open */
    *fd = open( name, O_RDWR|O_CREAT, 0664 );
    if( *fd < 0 ) {
        ACH_DIE( "Could not open pid file %s: %s\n", name, strerror(errno) );
    }
    /* lock */
    if( lockf(*fd, F_TLOCK, 0) ) {
        ACH_DIE( "Could not lock pid file %s: %s\n", name, strerror(errno) );
    }
    /* lock FILE */
    *fp = fdopen(*fd, "w");
    if( NULL == *fp ) {
        ACH_DIE( "Could not open FILE pointer for %s: %s\n", name, strerror(errno) );
    }
}

static void write_pid( FILE *fp, pid_t pid ) {
    if( NULL == fp ) return;
    /* seek */
    if( fseek(fp, 0, SEEK_SET) ) {
        ACH_LOG( LOG_ERR, "Could seek pid file\n");
    }
    /* print */
    if( 0 < fprintf(fp, "%d", pid) ) {
        ACH_LOG( LOG_ERR, "Could not write pid\n");
    }
    /* flush */
    int r;
    do{ r = fflush(fp); }
    while( 0 != r && EINTR == errno );
    if( r ) {
        ACH_LOG( LOG_ERR,  "Could not flush pid file: %s\n", strerror(errno) );
    }
}


/* Now it gets hairy... */

static void run(FILE *fp_pid, pid_t *pid_ptr, const char *file, const char **args) {
    while(1) {
        /* start */
        start_child( fp_pid, pid_ptr, file, args );
        /* wait for something */
        int sig = wait_for_signal();
        int status, signal;
        /* do something */
        switch( sig ) {
        case SIGTERM:
        case SIGINT:
            ACH_LOG(LOG_DEBUG, "Killing child\n");
            /* Kill Child */
            if( kill(*pid_ptr, SIGTERM) ) ACH_DIE( "Couldn't kill child: %s\n", strerror(errno) );
            /* Wait Child */
            waitloop(*pid_ptr, &status, &signal);
            /* TODO: timeout and SIGKILL child */
            /* Exit */
            exit(status);
        case SIGCHLD:
            /* Get child status and restart or exit */
            waitloop(*pid_ptr, &status, &signal);
            if( 0 == signal && EXIT_SUCCESS == status ) {
                ACH_LOG(LOG_DEBUG, "Child returned success, exiting\n");
                exit(EXIT_SUCCESS);
            }
            ACH_LOG(LOG_DEBUG, "Restarting child\n");
            /* else restart */
            break;
        default:
            ACH_DIE("Unexpected signal: %d\n", sig);
        }
    }
}

static void start_child(FILE *fp_pid, pid_t *pid_ptr, const char *file, const char **args) {
    pid_t pid = fork();

    if( 0 == pid ) { /* child: exec */
        execvp( file, (char *const*)args );
        ACH_DIE( "Could not exec: %s\n", strerror(errno) );
    } else if ( pid > 0 ) { /* parent: record child */
        *pid_ptr = pid;
        write_pid( fp_pid, pid );
    } else {
        /* TODO: handle EAGAIN */
        ACH_DIE( "Could not fork child: %s\n", strerror(errno) );
    }
}

static void waitloop( pid_t pid, int *exit_status, int *signal ) {
    assert( pid > 0 );
    *exit_status = 0;
    *signal = 0;
    while(1)
    {
        int status;
        pid_t wpid = wait( &status );
        if( wpid == pid ) {
            /* Child did something */
            if( WIFEXITED(status) ) {
                ACH_LOG(LOG_DEBUG, "child exited with %d\n", WEXITSTATUS(status));
                *exit_status = WEXITSTATUS(status);
                return;
            } else if ( WIFSIGNALED(status) ) {
                ACH_LOG(LOG_DEBUG, "child signalled with %d\n", WTERMSIG(status));
                *signal = WTERMSIG(status);
                return;
            } else {
                ACH_LOG(LOG_WARNING, "Unexpected wait result %d\n", status);
                /* I guess we keep waiting then */
            }
        } else if ( wpid < 0 ) {
            /* Wait failed */
            if( EINTR == errno ) {
                ACH_LOG(LOG_DEBUG, "wait interrupted\n");
            } else if (ECHILD == errno) {
                ACH_DIE("unexpected ECHILD\n");
            } else { /* something bad */
                ACH_DIE( "Couldn't wait for child: %s\n", strerror(errno));
            }
        } else {
            /* Wrong child somehow */
            ACH_LOG( LOG_ERR, "Got unexpected PID, child %d, wait %d\n", pid, wpid);
        }
    }
}

static int wait_for_signal() {
    ACH_LOG( LOG_DEBUG, "waiting for signal\n" );
    /* block signals */
    sigset_t oldmask, blockmask;
    if( sigemptyset(&blockmask) ) ACH_DIE("sigemptyset failed: %s\n", strerror(errno));
    if( sigaddset(&blockmask, SIGCHLD) ) ACH_DIE("sigaddset failed: %s\n", strerror(errno));
    if( sigaddset(&blockmask, SIGTERM) ) ACH_DIE("sigaddset failed: %s\n", strerror(errno));
    if( sigaddset(&blockmask, SIGINT) ) ACH_DIE("sigaddset failed: %s\n", strerror(errno));

    if( sigprocmask(SIG_BLOCK, &blockmask, &oldmask) ) {
        ACH_DIE( "sigprocmask failed: %s\n", strerror(errno) );
    }

    /* check flags */
    int r = check_signal();
    if( 0 == r ) {
        /* suspend */
        ACH_LOG( LOG_DEBUG, "suspending\n" );
        if( -1 != sigsuspend(&oldmask) ) {
            ACH_DIE("sigsupend failed: %s\n", strerror(errno));
        }
        ACH_LOG( LOG_DEBUG, "suspend returned\n" );
        /* check flags */
        r = check_signal();
    }

    /* restore sigmask */
    if( sigprocmask(SIG_SETMASK, &oldmask, NULL) ) {
        ACH_DIE( "sigprocmask failed: %s\n", strerror(errno) );
    }

    ACH_LOG( LOG_DEBUG, "Signalled: %d\n", r );

    assert( 0 != r ); /* We better have a signal now */

    return r;
}

static int check_signal() {
    if( ach_got_sigterm ) {
        return SIGTERM;
    } else if( ach_got_sigint ) {
        return SIGINT;
    } else if( ach_got_sigchild ) {
        /* Signal is currently blocked */
        ach_got_sigchild--;
        return SIGCHLD;
    } else {
        return 0;
    }
}
