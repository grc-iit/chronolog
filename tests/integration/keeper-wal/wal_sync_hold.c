/* Test preload for keeper_wal_shutdown_test.sh.
 *
 * While CHRONOLOG_TEST_HOLD_WAL_SYNC names a file that exists, fdatasync on a
 * WAL segment waits until that file is removed. The first such wait creates
 * CHRONOLOG_TEST_WAL_SYNC_HELD, so the script can see that a durable group is
 * inside its sync. Other files, including the exit summary, are not held.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int (*real_fdatasync)(int) = NULL;

static int call_fdatasync(int fd)
{
    if(real_fdatasync == NULL)
    {
        real_fdatasync = (int (*)(int))dlsym(RTLD_NEXT, "fdatasync");
        if(real_fdatasync == NULL)
        {
            errno = ENOSYS;
            return -1;
        }
    }
    return real_fdatasync(fd);
}

/* A segment is "<20 digits>.wal". Copies and partials are not. */
static int is_wal_segment(int fd)
{
    char proc_path[64];
    char file_path[4096];
    ssize_t length;
    char const* slash;

    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    length = readlink(proc_path, file_path, sizeof(file_path) - 1);
    if(length < 0)
    {
        return 0;
    }
    file_path[length] = '\0';
    slash = strrchr(file_path, '/');
    slash = (slash == NULL) ? file_path : slash + 1;
    if(strlen(slash) != 24 || strcmp(slash + 20, ".wal") != 0)
    {
        return 0;
    }
    for(int i = 0; i < 20; ++i)
    {
        if(slash[i] < '0' || slash[i] > '9')
        {
            return 0;
        }
    }
    return 1;
}

int fdatasync(int fd)
{
    char const* hold = getenv("CHRONOLOG_TEST_HOLD_WAL_SYNC");
    if(hold != NULL && hold[0] != '\0' && access(hold, F_OK) == 0 && is_wal_segment(fd))
    {
        char const* flag = getenv("CHRONOLOG_TEST_WAL_SYNC_HELD");
        if(flag != NULL && flag[0] != '\0')
        {
            int flag_fd = open(flag, O_CREAT | O_WRONLY, 0644);
            if(flag_fd >= 0)
            {
                close(flag_fd);
            }
        }
        while(access(hold, F_OK) == 0)
        {
            usleep(20000);
        }
    }
    return call_fdatasync(fd);
}
