#ifndef ls_io_utils_h_
#define ls_io_utils_h_

#include <stddef.h>
#include <sys/types.h>
#include <fcntl.h>
#include "compdep.h"

// Reads data from a file descriptor and appends it to *pbuf (and reallocates it as needed, as if
// realloc() were used) until either end of file is reached or a read error occurs.
//
// *pbuf must point to an object of size at least *palloc bytes.
//
// *psize is the previous "size" of the buffer, that is, position to append after. Must not be
// greater than *palloc.
//
// If the end of file is reached, returns the total number of bytes appended.
// If a read error occurs, returns -1 and errno is set.
// *pbuf, *psize and *palloc are always updated.
ssize_t
ls_full_read_append(int fd, char **pbuf, size_t *psize, size_t *palloc);

// Spawns a process and returns its PID.
//
// If pipe_fd is NULL, the behaviour is same as:
//     1. forking, and;
//     2. in child process, calling execvp(file, argv), and exiting with code 127 if it fails, and;
//     3. returning PID of spawned process, or (pid_t) -1 if fork() fails.
//
// If pipe_fd is not NULL, the behaviour differs in that:
//     1. a pipe is created, and;
//     2. child process' stdout is redirected to its write end, and;
//     3. its read end is returned as *pipe_fd, and;
//     4. (pid_t) -1 is also returned if some of the actions described above fails.
//
// If (pid_t) -1 is returned, errno is set by a failed function.
//
// This function is thread-safe.
pid_t
ls_spawnp_pipe(const char *file, int *pipe_fd, char *const *argv);

// Makes a file descriptor CLOEXEC.
//
// On success, fd is returned.
//
// On failure, -1 is returned and errno is set.
LS_INHEADER
int
ls_make_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return -1;
    }
    return fd;
}

#endif
