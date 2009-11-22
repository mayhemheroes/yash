/* Yash: yet another shell */
/* redir.c: manages file descriptors and provides functions for redirections */
/* (C) 2007-2009 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#if HAVE_GETTEXT
# include <libintl.h>
#endif
#if YASH_ENABLE_SOCKET
# include <netdb.h>
# include <sys/socket.h>
#endif
#include "exec.h"
#include "expand.h"
#include "input.h"
#include "option.h"
#include "parser.h"
#include "path.h"
#include "redir.h"
#include "sig.h"
#include "strbuf.h"
#include "util.h"
#include "yash.h"


/********** Utilities **********/

/* Closes a file descriptor surely.
 * If `close' returns EINTR, tries again.
 * If `close' returns EBADF, it is considered successful and silently ignored.
 * If `close' returns an error other than EINTR/EBADF, a message is printed. */
int xclose(int fd)
{
    while (close(fd) < 0) {
	switch (errno) {
	case EINTR:
	    continue;
	case EBADF:
	    return 0;
	default:
	    xerror(errno, Ngt("error in closing file descriptor %d"), fd);
	    return -1;
	}
    }
    return 0;
}

/* Performs `dup2' surely.
 * If `dup2' returns EINTR, tries again.
 * If `dup2' returns an error other than EINTR, an error message is printed.
 * `xclose' is called before `dup2'. */
int xdup2(int oldfd, int newfd)
{
    xclose(newfd);
    while (dup2(oldfd, newfd) < 0) {
	switch (errno) {
	case EINTR:
	    continue;
	default:
	    xerror(errno,
		    Ngt("cannot copy file descriptor %d to %d"),
		    oldfd, newfd);
	    return -1;
	}
    }
    return newfd;
}

/* Repeatedly calls `write' until all `data' is written.
 * Returns true iff successful. On error, false is returned with `errno' set. */
/* Note that this function returns a bool value, not ssize_t. */
bool write_all(int fd, const void *data, size_t size)
{
    size_t done = 0;

    while (done < size) {
	ssize_t s = write(fd, (const char *) data + done, size - done);
	if (s < 0)
	    return false;
	done += s;
    }
    return true;
}

/********** Shell FD **********/

static void reset_shellfdmin(void);


/* true iff stdin is redirected */
static bool is_stdin_redirected = false;

/* set of file descriptors used by the shell.
 * These file descriptors cannot be used by the user. */
static fd_set shellfds;
/* the minimum file descriptor that can be used for shell FD. */
static int shellfdmin;
/* the maximum file descriptor in `shellfds'.
 * `shellfdmax' is -1 if `shellfds' is empty. */
static int shellfdmax;

#ifndef SHELLFDMINMAX
#define SHELLFDMINMAX 100  /* maximum for `shellfdmin' */
#elif SHELLFDMINMAX < 10
#error SHELLFDMINMAX too little
#endif

/* file descriptor associated with the controlling terminal */
int ttyfd = -1;


/* Initializes shell FDs. */
void init_shellfds(void)
{
#ifndef NDEBUG
    static bool initialized = false;
    assert(!initialized);
    initialized = true;
#endif

    FD_ZERO(&shellfds);
    reset_shellfdmin();
    shellfdmax = -1;
}

/* Recomputes `shellfdmin'. */
void reset_shellfdmin(void)
{
    errno = 0;
    shellfdmin = sysconf(_SC_OPEN_MAX);
    if (shellfdmin == -1) {
	if (errno)
	    shellfdmin = 10;
	else
	    shellfdmin = SHELLFDMINMAX;
    } else {
	shellfdmin /= 2;
	if (shellfdmin > SHELLFDMINMAX)
	    shellfdmin = SHELLFDMINMAX;
	else if (shellfdmin < 10)
	    shellfdmin = 10;
    }
}

/* Adds the specified file descriptor (>= `shellfdmin') to `shellfds'. */
void add_shellfd(int fd)
{
    assert(fd >= shellfdmin);
    if (fd < FD_SETSIZE)
	FD_SET(fd, &shellfds);
    if (shellfdmax < fd)
	shellfdmax = fd;
}

/* Removes the specified file descriptor from `shellfds'.
 * Must be called BEFORE `xclose(fd)'. */
void remove_shellfd(int fd)
{
    if (0 <= fd && fd < FD_SETSIZE)
	FD_CLR(fd, &shellfds);
    if (fd == shellfdmax) {
	shellfdmax = fd - 1;
	while (shellfdmax >= 0 && !FD_ISSET(shellfdmax, &shellfds))
	    shellfdmax--;
    }
}
/* The argument to `FD_CLR' must be a valid (open) file descriptor. This is why
 * `remove_shellfd' must be called before closing the file descriptor. */

/* Checks if the specified file descriptor is in `shellfds'. */
bool is_shellfd(int fd)
{
    return fd >= FD_SETSIZE || (fd >= 0 && FD_ISSET(fd, &shellfds));
}

/* Closes all file descriptors in `shellfds' and empties it.
 * If `leavefds' is true, the file descriptors are actually not closed. */
void clear_shellfds(bool leavefds)
{
    if (!leavefds) {
	for (int fd = 0; fd <= shellfdmax; fd++)
	    if (FD_ISSET(fd, &shellfds))
		xclose(fd);
	FD_ZERO(&shellfds);
	shellfdmax = -1;
    }
    ttyfd = -1;
}

/* Duplicates the specified file descriptor as a new shell FD.
 * The new FD is added to `shellfds'.
 * On error, `errno' is set and -1 is returned. */
int copy_as_shellfd(int fd)
{
#ifdef F_DUPFD_CLOEXEC
    int newfd = fcntl(fd, F_DUPFD_CLOEXEC, shellfdmin);
#else
    int newfd = fcntl(fd, F_DUPFD, shellfdmin);
#endif
    if (newfd >= 0) {
#ifndef F_DUPFD_CLOEXEC
	fcntl(newfd, F_SETFD, FD_CLOEXEC);
#endif
	add_shellfd(newfd);
    }
    return newfd;
}

/* Duplicates the underlining file descriptor of the specified stream.
 * The original stream is closed whether successful or not.
 * A new stream is open with the new FD using `fdopen' and returned.
 * The new stream's underlining file descriptor is registered as a shell FD and,
 * if `nonblock' is true, set to non-blocking.
 * If NULL is given, this function just returns NULL without doing anything. */
FILE *reopen_with_shellfd(FILE *f, const char *mode, bool nonblock)
{
    if (!f)
	return NULL;

    int newfd = copy_as_shellfd(fileno(f));
    fclose(f);
    if (newfd < 0)
	return NULL;
    if (nonblock && !set_nonblocking(newfd)) {
	xclose(newfd);
	return NULL;
    }
    return fdopen(newfd, mode);
}

/* Opens `ttyfd'.
 * On failure, an error message is printed and `do_job_control' is set to false.
 */
void open_ttyfd(void)
{
    if (ttyfd < 0) {
	int fd = open("/dev/tty", O_RDWR);
	if (fd >= 0) {
	    ttyfd = copy_as_shellfd(fd);
	    xclose(fd);
	}
	if (ttyfd < 0) {
	    xerror(errno, Ngt("cannot open `%s'"), "/dev/tty");
	    xerror(0, Ngt("job control disabled"));
	    do_job_control = false;
	}
    }
}


/********** Redirections **********/

/* info used to undo redirection */
struct savefd_T {
    struct savefd_T *next;
    int  sf_origfd;            /* original file descriptor */
    int  sf_copyfd;            /* copied file descriptor */
    bool sf_stdin_redirected;  /* original `is_stdin_redirected' */
};

static char *expand_redir_filename(const struct wordunit_T *filename)
    __attribute__((malloc,warn_unused_result));
static void save_fd(int oldfd, savefd_T **save);
static int open_file(const char *path, int oflag)
    __attribute__((nonnull));
#if YASH_ENABLE_SOCKET
static int open_socket(const char *hostandport, int socktype)
    __attribute__((nonnull));
#endif
static int parse_and_check_dup(char *num, redirtype_T type)
    __attribute__((nonnull));
static int parse_and_exec_pipe(int outputfd, char *num, savefd_T **save)
    __attribute__((nonnull));
static int open_heredocument(const struct wordunit_T *content);
static int open_herestring(char *s, bool appendnewline)
    __attribute__((nonnull));
static int open_process_redirection(const embedcmd_T *command, redirtype_T type)
    __attribute__((nonnull));


/* Opens a redirection.
 * If `save' is non-NULL, the original FD is saved and a pointer to the info is
 * assigned to `*save' (whether successful or not).
 * Returns true iff successful. */
bool open_redirections(const redir_T *r, savefd_T **save)
{
    if (save)
	*save = NULL;

    while (r) {
	if (r->rd_fd < 0 || is_shellfd(r->rd_fd)) {
	    xerror(0, Ngt("redirection: file descriptor %d unavailable"),
		    r->rd_fd);
	    return false;
	}

	/* expand rd_filename */
#ifdef NDEBUG
	char *filename;
#else
	char *filename = filename;
#endif
	switch (r->rd_type) {
	    case RT_INPUT:  case RT_OUTPUT:  case RT_CLOBBER:  case RT_APPEND:
	    case RT_INOUT:  case RT_DUPIN:   case RT_DUPOUT:   case RT_PIPE:
	    case RT_HERESTR:
		filename = expand_redir_filename(r->rd_filename);
		if (!filename)
		    return false;
		break;
	    default:
		break;
	}

	/* save original FD */
	save_fd(r->rd_fd, save);

	/* now, open redirection */
	int fd;
	int flags;
	bool keepopen;
	switch (r->rd_type) {
	case RT_INPUT:
	    flags = O_RDONLY;
	    goto openwithflags;
	case RT_OUTPUT:
	    if (shopt_noclobber && !is_irregular_file(filename)) {
		flags = O_WRONLY | O_CREAT | O_EXCL;
	    } else {
	case RT_CLOBBER:
		flags = O_WRONLY | O_CREAT | O_TRUNC;
	    }
	    goto openwithflags;
	case RT_APPEND:
	    flags = O_WRONLY | O_CREAT | O_APPEND;
	    goto openwithflags;
	case RT_INOUT:
	    flags = O_RDWR | O_CREAT;
	    goto openwithflags;
openwithflags:
	    keepopen = false;
	    fd = open_file(filename, flags);
	    if (fd < 0) {
		xerror(errno, Ngt("redirection: cannot open `%s'"), filename);
		free(filename);
		return false;
	    }
	    free(filename);
	    break;
	case RT_DUPIN:
	case RT_DUPOUT:
	    keepopen = true;
	    fd = parse_and_check_dup(filename, r->rd_type);
	    if (fd < -1)
		return false;
	    break;
	case RT_PIPE:
	    keepopen = false;
	    fd = parse_and_exec_pipe(r->rd_fd, filename, save);
	    if (fd < -1)
		return false;
	    break;
	case RT_HERE:
	case RT_HERERT:
	    keepopen = false;
	    fd = open_heredocument(r->rd_herecontent);
	    if (fd < 0)
		return false;
	    break;
	case RT_HERESTR:
	    keepopen = false;
	    fd = open_herestring(filename, true);
	    if (fd < 0)
		return false;
	    break;
	case RT_PROCIN:
	case RT_PROCOUT:
	    keepopen = false;
	    fd = open_process_redirection(&r->rd_command, r->rd_type);
	    if (fd < 0)
		return false;
	    break;
	default:
	    assert(false);
	}

	/* move the new FD to `r->rd_fd' */
	if (fd != r->rd_fd) {
	    if (fd >= 0) {
		if (xdup2(fd, r->rd_fd) < 0)
		    return false;
		if (!keepopen)
		    xclose(fd);
	    } else {
		xclose(r->rd_fd);
	    }
	}

	if (r->rd_fd == STDIN_FILENO)
	    is_stdin_redirected = true;

	r = r->next;
    }
    return true;
}

/* Expands the filename for a redirection. */
char *expand_redir_filename(const struct wordunit_T *filename)
{
    if (is_interactive) {
	return expand_single_with_glob(filename, tt_single);
    } else {
	wchar_t *result = expand_single(filename, tt_single);
	if (result == NULL)
	    return NULL;
	char *mbsresult = realloc_wcstombs(unescapefree(result));
	if (!mbsresult)
	    xerror(EILSEQ, Ngt("redirection"));
	return mbsresult;
    }
}

/* Saves the specified file descriptor if `save' is non-NULL. */
void save_fd(int fd, savefd_T **save)
{
    assert(0 <= fd);
    if (!save)
	return;

    int copyfd = copy_as_shellfd(fd);
    if (copyfd < 0 && errno != EBADF) {
	xerror(errno, Ngt("cannot save file descriptor %d"), fd);
	return;
    }

    savefd_T *s = xmalloc(sizeof *s);
    s->next = *save;
    s->sf_origfd = fd;
    s->sf_copyfd = copyfd;
    s->sf_stdin_redirected = is_stdin_redirected;
    /* note: if `fd' is formerly unused, `sf_copyfd' is -1. */
    *save = s;
}

/* Opens the file for a redirection.
 * `path' and `oflag' are the first and second argument to the `open' function.
 * If the socket redirection feature is enabled and `path' begins with
 * "/dev/tcp/" or "/dev/udp/", then a socket is opened.
 * Returns a new file descriptor if successful. Otherwise `errno' is set and
 * -1 is returned. */
int open_file(const char *path, int oflag)
{
    int fd;

    fd = open(path, oflag,
	    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
#if YASH_ENABLE_SOCKET
    if (fd < 0) {
	const char *hostandport = matchstrprefix(path, "/dev/tcp/");
	if (hostandport)
	    fd = open_socket(hostandport, SOCK_STREAM);
    }
    if (fd < 0) {
	const char *hostandport = matchstrprefix(path, "/dev/udp/");
	if (hostandport)
	    fd = open_socket(hostandport, SOCK_DGRAM);
    }
#endif /* YASH_ENABLE_SOCKET */
    return fd;
}

#if YASH_ENABLE_SOCKET

/* Opens a socket.
 * `hostandport' is the name and the port of the host to connect, concatenated
 * by a slash. `socktype' specifies the type of the socket, which should be
 * `SOCK_STREAM' for TCP or `SOCK_DGRAM' for UDP. */
int open_socket(const char *hostandport, int socktype)
{
    struct addrinfo hints, *ai;
    int err, saveerrno;
    char *hostname, *port;
    int fd;

    saveerrno = errno;

    /* decompose `hostandport' into `hostname' and `port' */
    {
	wchar_t *whostandport;
	const wchar_t *wport;

	whostandport = malloc_mbstowcs(hostandport);
	if (!whostandport) {
	    errno = saveerrno;
	    return -1;
	}
	wport = wcschr(whostandport, L'/');
	if (wport) {
	    hostname = malloc_wcsntombs(whostandport, wport - whostandport);
	    port = malloc_wcstombs(wport + 1);
	} else {
	    hostname = xstrdup(hostandport);
	    port = NULL;
	}
	free(whostandport);
    }

    set_interruptible_by_sigint(true);

    hints.ai_flags = 0;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_protocol = 0;
    hints.ai_addrlen = 0;
    hints.ai_addr = NULL;
    hints.ai_canonname = NULL;
    hints.ai_next = NULL;
    err = getaddrinfo(hostname, port, &hints, &ai);
    free(hostname);
    free(port);
    if (err != 0) {
	xerror(0, Ngt("socket redirection: cannot resolve address of %s: %s"),
		hostandport, gai_strerror(err));
	set_interruptible_by_sigint(false);
	errno = saveerrno;
	return -1;
    }

    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
	xclose(fd);
	fd = -1;
    }
    saveerrno = errno;
    freeaddrinfo(ai);
    set_interruptible_by_sigint(false);
    errno = saveerrno;
    return fd;
}

#endif /* YASH_ENABLE_SOCKET */

/* Parses the argument to `RT_DUPIN'/`RT_DUPOUT'.
 * `num' is the argument to parse, which is expected to be "-" or numeric.
 * `num' is freed in this function.
 * If `num' is a positive integer, the value is returned.
 * If `num' is "-", -1 is returned.
 * Otherwise, a negative value other than -1 is returned.
 * `type' must be either RT_DUPIN or RT_DUPOUT. */
int parse_and_check_dup(char *const num, redirtype_T type)
{
    int fd;

    if (strcmp(num, "-") == 0) {
	fd = -1;
    } else {
	if (!xisxdigit(num[0])) {
	    errno = EINVAL;
	} else {
	    if (xstrtoi(num, 10, &fd) && fd < 0)
		errno = ERANGE;
	}
	if (errno) {
	    xerror(errno, Ngt("redirection: %s"), num);
	    fd = -2;
	} else {
	    if (is_shellfd(fd)) {
		xerror(0, Ngt("redirection: file descriptor %d unavailable"),
			fd);
		fd = -2;
	    } else if (posixly_correct) {
		/* check the read/write permission */
		int flags = fcntl(fd, F_GETFL);
		if (flags < 0) {
		    xerror(errno, Ngt("redirection: %d"), fd);
		    fd = -2;
		} else if (type == RT_DUPIN) {
		    switch (flags & O_ACCMODE) {
			case O_RDONLY:  case O_RDWR:
			    /* ok */
			    break;
			default:
			    xerror(0, Ngt("redirection: %d: not readable"), fd);
			    fd = -2;
			    break;
		    }
		} else {
		    assert(type == RT_DUPOUT);
		    switch (flags & O_ACCMODE) {
			case O_WRONLY:  case O_RDWR:
			    /* ok */
			    break;
			default:
			    xerror(0, Ngt("redirection: %d: not writable"), fd);
			    fd = -2;
			    break;
		    }
		}
	    }
	}
    }
    free(num);
    return fd;
}

/* Parses the argument to `RT_PIPE' and opens a pipe.
 * `outputfd' is the file descriptor of the output side of the pipe.
 * `num' is the argument to parse, which is expected to be a positive integer
 * that is the file descriptor of the input side of the pipe.
 * `num' is freed in this function.
 * If successful, the actual file descriptor of the output side of the pipe is
 * returned, which may differ from `outputfd'. Otherwise, a negative value other
 * than -1 is returned. */
/* The input side FD is saved in this function. */
int parse_and_exec_pipe(int outputfd, char *num, savefd_T **save)
{
    int fd, inputfd;
    int pipefd[2];

    assert(outputfd >= 0);

    if (!xisxdigit(num[0])) {
	errno = EINVAL;
    } else {
	if (xstrtoi(num, 10, &inputfd) && inputfd < 0)
	    errno = ERANGE;
    }
    if (errno) {
	xerror(errno, Ngt("redirection: %s"), num);
	fd = -2;
    } else if (outputfd == inputfd) {
	xerror(0, Ngt("redirection: %d>>|%d: "
		    "same input and output file descriptor"),
		outputfd, inputfd);
	fd = -2;
    } else if (is_shellfd(inputfd)) {
	xerror(0, Ngt("redirection: file descriptor %d unavailable"),
		inputfd);
	fd = -2;
    } else {
	/* ok, save inputfd and open the pipe */
	save_fd(inputfd, save);
	if (pipe(pipefd) < 0)
	    goto error;

	/* move the output side from what is to be the input side. */
	if (pipefd[PIDX_OUT] == inputfd) {
	    int newfd = dup(pipefd[PIDX_OUT]);
	    if (newfd < 0)
		goto error2;
	    xclose(pipefd[PIDX_OUT]);
	    pipefd[PIDX_OUT] = newfd;
	}

	/* move the input side to where it should be. */
	if (pipefd[PIDX_IN] != inputfd) {
	    if (xdup2(pipefd[PIDX_IN], inputfd) < 0)
		goto error2;
	    xclose(pipefd[PIDX_IN]);
	    // pipefd[PIDX_IN] = inputfd;
	}

	/* The output side is not moved in this function. */
	fd = pipefd[PIDX_OUT];
    }
end:
    free(num);
    return fd;

error2:
    xclose(pipefd[PIDX_IN]);
    xclose(pipefd[PIDX_OUT]);
error:
    xerror(errno, Ngt("redirection: %d>>|%d"), outputfd, inputfd);
    fd = -2;
    goto end;
}

/* Opens a here-document whose contents is specified by the argument.
 * Returns a newly opened file descriptor if successful, or -1 number on error.
 */
/* The contents of the here-document is passed either through a pipe or in a
 * temporary file. */
int open_heredocument(const wordunit_T *contents)
{
    wchar_t *wcontents = expand_string(contents, true);
    if (!wcontents)
	return -1;

    char *mcontents = realloc_wcstombs(wcontents);
    if (!mcontents) {
	xerror(EILSEQ, Ngt("cannot write here-document contents"));
	return -1;
    }

    return open_herestring(mcontents, false);
}

/* Opens a here-string whose contents is specified by the argument.
 * If `appendnewline' is true, a newline is appended to the value of `s'.
 * Returns a newly opened file descriptor if successful, or -1 number on error.
 * `s' is freed in this function. */
/* The contents of the here-document is passed either through a pipe or in a
 * temporary file. */
int open_herestring(char *s, bool appendnewline)
{
    int fd;

    /* if contents is empty */
    if (s[0] == '\0' && !appendnewline) {
	fd = open("/dev/null", O_RDONLY);
	if (fd >= 0) {
	    free(s);
	    return fd;
	}
    }

    size_t len = strlen(s);
    if (appendnewline)
	s[len++] = '\n';

#ifdef PIPE_BUF
    /* use a pipe if the contents is short enough */
    if (len <= PIPE_BUF) {
	int pipefd[2];

	if (pipe(pipefd) >= 0) {
	    /* It is guaranteed that all the contents is written to the pipe
	     * at once, so we don't have to use `write_all' here. */
	    if (write(pipefd[PIDX_OUT], s, len) < 0)
		xerror(errno, Ngt("cannot write here-document contents"));
	    xclose(pipefd[PIDX_OUT]);
	    free(s);
	    return pipefd[PIDX_IN];
	}
    }
#endif /* defined(PIPE_BUF) */

    char *tempfile;
    fd = create_temporary_file(&tempfile, 0);
    if (fd < 0) {
	xerror(errno, Ngt("cannot create temporary file for here-document"));
	free(s);
	return -1;
    }
    if (unlink(tempfile) < 0)
	xerror(errno, Ngt("failed to remove temporary file `%s'"), tempfile);
    free(tempfile);
    if (!write_all(fd, s, len))
	xerror(errno, Ngt("cannot write here-document contents"));
    free(s);
    if (lseek(fd, 0, SEEK_SET) != 0)
	xerror(errno, Ngt("cannot seek temporary file for here-document"));
    return fd;
}

/* Opens a process redirection and returns a file descriptor for it.
 * `type' must be RT_PROCIN or RT_PROCOUT.
 * The return value is -1 if failed. */
int open_process_redirection(const embedcmd_T *command, redirtype_T type)
{
    int pipefd[2];
    pid_t cpid;

    assert(type == RT_PROCIN || type == RT_PROCOUT);
    if (pipe(pipefd) < 0) {
	xerror(errno,
		Ngt("redirection: cannot open pipe for command redirection"));
	return -1;
    }
    cpid = fork_and_reset(-1, false, 0);
    if (cpid < 0) {
	/* fork failure */
	xclose(pipefd[PIDX_IN]);
	xclose(pipefd[PIDX_OUT]);
	return -1;
    } else if (cpid) {
	/* parent process */
	if (type == RT_PROCIN) {
	    xclose(pipefd[PIDX_OUT]);
	    return pipefd[PIDX_IN];
	} else {
	    xclose(pipefd[PIDX_IN]);
	    return pipefd[PIDX_OUT];
	}
    } else {
	/* child process */
	if (type == RT_PROCIN) {
	    xclose(pipefd[PIDX_IN]);
	    if (pipefd[PIDX_OUT] != STDOUT_FILENO) {
		if (xdup2(pipefd[PIDX_OUT], STDOUT_FILENO) < 0)
		    exit(Exit_NOEXEC);
		xclose(pipefd[PIDX_OUT]);
	    }
	} else {
	    xclose(pipefd[PIDX_OUT]);
	    if (pipefd[PIDX_IN] != STDIN_FILENO) {
		if (xdup2(pipefd[PIDX_IN], STDIN_FILENO) < 0)
		    exit(Exit_NOEXEC);
		xclose(pipefd[PIDX_IN]);
	    }
	}
	if (command->is_preparsed)
	    exec_and_or_lists(command->value.preparsed, true);
	else
	    exec_wcs(command->value.unparsed, gt("command redirection"), true);
	assert(false);
    }
}

/* Restores the saved file descriptor and frees `save'. */
void undo_redirections(savefd_T *save)
{
    while (save) {
	if (save->sf_copyfd >= 0) {
	    remove_shellfd(save->sf_copyfd);
	    xdup2(save->sf_copyfd, save->sf_origfd);
	    xclose(save->sf_copyfd);
	} else {
	    xclose(save->sf_origfd);
	}
	is_stdin_redirected = save->sf_stdin_redirected;

	savefd_T *next = save->next;
	free(save);
	save = next;
    }
}

/* Frees the FD-saving info without restoring FD.
 * The copied FDs are closed. */
void clear_savefd(savefd_T *save)
{
    while (save) {
	if (save->sf_copyfd >= 0) {
	    remove_shellfd(save->sf_copyfd);
	    xclose(save->sf_copyfd);
	}

	savefd_T *next = save->next;
	free(save);
	save = next;
    }
}

/* Redirects stdin to "/dev/null" if job control is off and stdin is not yet
 * redirected.
 * If `posixly_correct' is true, the condition is slightly different:
 * "if non-interactive" rather than "if job control is off". */
void maybe_redirect_stdin_to_devnull(void)
{
    int fd;

    if ((posixly_correct ? is_interactive : do_job_control)
	    || is_stdin_redirected)
	return;

    xclose(STDIN_FILENO);
    fd = open("/dev/null", O_RDONLY);
    if (fd > 0) {
	xdup2(fd, STDIN_FILENO);
	xclose(fd);
    }
    is_stdin_redirected = true;
}


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
