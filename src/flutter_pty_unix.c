
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "forkpty.h"
#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

typedef struct PtyHandle
{
    int ptm;

    int pid;

    int shell_pgid;

    pthread_mutex_t mutex;

    pthread_cond_t ack_cond;

    bool ackRead;

    /* Protected by mutex. */
    bool closed;

    /* Protected by mutex; true after a chunk is posted until ackRead. */
    bool awaiting_ack;

    /* Joinable so pty_close can wait for the reader before freeing. */
    pthread_t read_thread;

    bool read_thread_started;

    int wake_read_fd;

    int wake_write_fd;

} PtyHandle;

typedef struct ReadLoopOptions
{
    PtyHandle *handle;

    int fd;

    int wake_fd;

    Dart_Port port;

    bool waitForReadAck;

} ReadLoopOptions;

char *error_message = NULL;

static void *read_loop(void *arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[1024];

    while (1)
    {
        pthread_mutex_lock(&options->handle->mutex);
        while (options->waitForReadAck && options->handle->awaiting_ack &&
               !options->handle->closed)
        {
            pthread_cond_wait(&options->handle->ack_cond,
                              &options->handle->mutex);
        }
        bool closed = options->handle->closed;
        pthread_mutex_unlock(&options->handle->mutex);

        if (closed)
        {
            break;
        }

        /* Poll instead of blocking in read(): close() from another thread
         * does not wake a blocked reader, so without the poll tick a
         * disposed PTY would strand this thread forever. */
        struct pollfd poll_fds[2];
        poll_fds[0].fd = options->fd;
        poll_fds[0].events = POLLIN;
        poll_fds[1].fd = options->wake_fd;
        poll_fds[1].events = POLLIN;
        int ready = poll(poll_fds, 2, -1);

        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if (poll_fds[1].revents & POLLIN)
        {
            char signal_buffer[32];
            while (read(options->wake_fd, signal_buffer,
                        sizeof(signal_buffer)) > 0)
            {
            }
            break;
        }

        if (!(poll_fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
        {
            continue;
        }

        ssize_t n = read(options->fd, buffer, sizeof(buffer));

        if (n <= 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = n;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        if (options->waitForReadAck)
        {
            pthread_mutex_lock(&options->handle->mutex);
            if (options->handle->closed)
            {
                pthread_mutex_unlock(&options->handle->mutex);
                break;
            }
            /* Set this before posting so a fast Dart ack cannot be lost. */
            options->handle->awaiting_ack = true;
            pthread_mutex_unlock(&options->handle->mutex);
        }

        if (!Dart_PostCObject_DL(options->port, &result))
        {
            if (options->waitForReadAck)
            {
                pthread_mutex_lock(&options->handle->mutex);
                options->handle->awaiting_ack = false;
                pthread_cond_broadcast(&options->handle->ack_cond);
                pthread_mutex_unlock(&options->handle->mutex);
            }
            break;
        }
    }

    free(options);

    return NULL;
}

static bool start_read_thread(PtyHandle *handle, int fd, int wake_fd,
                              Dart_Port port, bool waitForReadAck)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    if (options == NULL)
    {
        return false;
    }

    options->handle = handle;

    options->fd = fd;

    options->wake_fd = wake_fd;

    options->port = port;

    options->waitForReadAck = waitForReadAck;

    if (pthread_create(&handle->read_thread, NULL, &read_loop, options) != 0)
    {
        free(options);
        return false;
    }
    handle->read_thread_started = true;
    return true;
}

typedef struct WaitExitOptions
{
    int pid;

    Dart_Port port;

} WaitExitOptions;

static void *wait_exit_thread(void *arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    int status;

    waitpid(options->pid, &status, 0);

    if (WIFEXITED(status))
    {
        Dart_PostInteger_DL(options->port, WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        Dart_PostInteger_DL(options->port, -WTERMSIG(status));
    }

    free(options);

    return NULL;
}

static bool start_wait_exit_thread(int pid, Dart_Port port)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));

    if (options == NULL)
    {
        return false;
    }

    options->pid = pid;

    options->port = port;

    pthread_t thread;
    if (pthread_create(&thread, NULL, &wait_exit_thread, options) != 0)
    {
        free(options);
        return false;
    }
    /* No caller needs to join the exit watcher; detach it so its pthread
     * bookkeeping is reclaimed as soon as waitpid/PostInteger completes. */
    pthread_detach(thread);
    return true;
}

static void set_environment(char **environment)
{
    if (environment == NULL)
    {
        return;
    }

    while (*environment != NULL)
    {
        putenv(*environment);
        environment++;
    }
}

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    struct winsize ws;

    ws.ws_row = options->rows;
    ws.ws_col = options->cols;

    int ptm;

    int wake_pipe[2];
    if (pipe(wake_pipe) != 0)
    {
        error_message = "pipe failed";
        return NULL;
    }
    fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

    int pid = pty_forkpty(&ptm, NULL, NULL, &ws);

    if (pid < 0)
    {
        error_message = "pty_forkpty failed";
        perror("pty_forkpty");
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        return NULL;
    }

    if (pid == 0)
    {
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        set_environment(options->environment);

        if (options->working_directory != NULL && strlen(options->working_directory) > 0)
        {
            chdir(options->working_directory);
        }

        int ok = execvp(options->executable, options->arguments);

        if (ok < 0)
        {
            perror("execvp");
            _exit(127);
        }
    }

    PtyHandle *handle = (PtyHandle *)malloc(sizeof(PtyHandle));

    if (handle == NULL)
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(ptm);
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        error_message = "Failed to allocate pty handle";
        return NULL;
    }

    handle->ptm = ptm;
    handle->pid = pid;
    /* After forkpty/setsid the child is its own session/pgroup leader, so
     * shell pgid equals pid. Do not call getpgid here: it can still observe
     * the parent's group before the child finishes setsid. */
    handle->shell_pgid = pid;
    pthread_mutex_init(&handle->mutex, NULL);
    pthread_cond_init(&handle->ack_cond, NULL);
    handle->ackRead = options->ackRead;
    handle->closed = false;
    handle->awaiting_ack = false;
    handle->read_thread_started = false;
    handle->wake_read_fd = wake_pipe[0];
    handle->wake_write_fd = wake_pipe[1];

    if (!start_read_thread(handle, ptm, handle->wake_read_fd,
                           options->stdout_port, options->ackRead))
    {
        pthread_cond_destroy(&handle->ack_cond);
        pthread_mutex_destroy(&handle->mutex);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(ptm);
        close(handle->wake_read_fd);
        close(handle->wake_write_fd);
        free(handle);
        return NULL;
    }

    if (!start_wait_exit_thread(pid, options->exit_port))
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        pty_close(handle);
        error_message = "Failed to start PTY exit watcher";
        return NULL;
    }

    return handle;
}

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return;
    }

    pthread_mutex_lock(&handle->mutex);
    handle->closed = true;
    handle->awaiting_ack = false;
    int fd = handle->ptm;
    handle->ptm = -1;
    int wake_fd = handle->wake_write_fd;
    handle->wake_write_fd = -1;
    pthread_cond_broadcast(&handle->ack_cond);
    pthread_mutex_unlock(&handle->mutex);

    /* Wake poll without closing the master fd from another thread. */
    if (wake_fd >= 0)
    {
        const char signal = 1;
        write(wake_fd, &signal, 1);
    }

    /* The reader dereferences the handle, so it must be gone before free. */
    if (handle->read_thread_started)
    {
        pthread_join(handle->read_thread, NULL);
    }

    if (fd >= 0)
    {
        close(fd);
    }
    if (wake_fd >= 0)
    {
        close(wake_fd);
    }
    if (handle->wake_read_fd >= 0)
    {
        close(handle->wake_read_fd);
    }

    pthread_cond_destroy(&handle->ack_cond);
    pthread_mutex_destroy(&handle->mutex);
    free(handle);
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length)
{
    // PTY masters behave like pipes: large writes (e.g. 20k+ char bracketed
    // pastes) may return short. Dropping the remainder leaves an unclosed
    // \x1b[200~...\x1b[201~ sequence and fullscreen paste ACK fails.
    int written = 0;
    while (written < length)
    {
        ssize_t n = write(handle->ptm, buffer + written, (size_t)(length - written));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (n == 0)
        {
            break;
        }
        written += (int)n;
    }
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return;
    }

    pthread_mutex_lock(&handle->mutex);
    if (handle->ackRead && handle->awaiting_ack)
    {
        handle->awaiting_ack = false;
        pthread_cond_signal(&handle->ack_cond);
    }
    pthread_mutex_unlock(&handle->mutex);
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    struct winsize ws;

    ws.ws_row = rows;
    ws.ws_col = cols;

    return ioctl(handle->ptm, TIOCSWINSZ, &ws);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    return handle->pid;
}

FFI_PLUGIN_EXPORT char *pty_error(void)
{
    return NULL;
}

FFI_PLUGIN_EXPORT int pty_get_master_fd(PtyHandle *handle)
{
    if (handle == NULL) {
        return -1;
    }
    return handle->ptm;
}

FFI_PLUGIN_EXPORT int pty_get_foreground_pgid(PtyHandle *handle)
{
    if (handle == NULL) {
        return -1;
    }
    pid_t pgid = tcgetpgrp(handle->ptm);
    if (pgid < 0) {
        return -1;
    }
    return (int)pgid;
}

FFI_PLUGIN_EXPORT int pty_get_shell_pgid(PtyHandle *handle)
{
    if (handle == NULL || handle->shell_pgid <= 0) {
        return -1;
    }
    return handle->shell_pgid;
}
