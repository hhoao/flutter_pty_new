#include <stdio.h>
#include <Windows.h>

#include "flutter_pty.h"

#include "include/dart_api.h"
#include "include/dart_api_dl.h"
#include "include/dart_native_api.h"

static BOOL arg_needs_quoting(const char *arg)
{
    if (arg == NULL || arg[0] == '\0')
    {
        return TRUE;
    }

    for (int i = 0; arg[i] != 0; i++)
    {
        if (arg[i] == ' ' || arg[i] == '\t' || arg[i] == '"')
        {
            return TRUE;
        }
    }

    return FALSE;
}

static int quoted_arg_length(const char *arg)
{
    int length = (int)strlen(arg);

    if (!arg_needs_quoting(arg))
    {
        return length;
    }

    length += 2;

    for (int i = 0; arg[i] != 0; i++)
    {
        if (arg[i] == '"')
        {
            length++;
        }
    }

    return length;
}

static void append_command_arg(LPWSTR command, int *pos, const char *arg)
{
    BOOL quote = arg_needs_quoting(arg);

    if (quote)
    {
        command[(*pos)++] = L'"';
    }

    for (int j = 0; arg[j] != 0; j++)
    {
        if (quote && arg[j] == '"')
        {
            command[(*pos)++] = L'\\';
        }

        command[(*pos)++] = (WCHAR)arg[j];
    }

    if (quote)
    {
        command[(*pos)++] = L'"';
    }
}

static LPWSTR build_command(char *executable, char **arguments)
{
    int command_length = 0;

    if (executable != NULL)
    {
        command_length += quoted_arg_length(executable);
    }

    if (arguments != NULL)
    {
        // argv[0] duplicates [executable]; only append argv[1..].
        int i = 1;

        while (arguments[i] != NULL)
        {
            command_length += quoted_arg_length(arguments[i]) + 1;
            i++;
        }
    }

    LPWSTR command = malloc((command_length + 1) * sizeof(WCHAR));

    if (command != NULL)
    {
        int pos = 0;

        append_command_arg(command, &pos, executable);

        if (arguments != NULL)
        {
            int j = 1;

            while (arguments[j] != NULL)
            {
                command[pos++] = L' ';
                append_command_arg(command, &pos, arguments[j]);
                j++;
            }
        }

        command[pos] = 0;
    }

    return command;
}

static LPWSTR build_environment(char **environment)
{
    LPWSTR environment_block = NULL;
    int environment_block_length = 0;

    if (environment != NULL)
    {
        int i = 0;

        while (environment[i] != NULL)
        {
            environment_block_length += (int)strlen(environment[i]) + 1;
            i++;
        }
    }

    environment_block = malloc((environment_block_length + 1) * sizeof(WCHAR));

    if (environment_block != NULL)
    {
        int i = 0;

        if (environment != NULL)
        {
            int j = 0;

            while (environment[j] != NULL)
            {
                int k = 0;

                while (environment[j][k] != 0)
                {
                    environment_block[i] = (WCHAR)environment[j][k];
                    i++;
                    k++;
                }

                environment_block[i++] = 0;

                j++;
            }
        }

        environment_block[i] = 0;
    }

    return environment_block;
}

static LPWSTR build_working_directory(char *working_directory)
{
    if (working_directory == NULL)
    {
        return NULL;
    }

    int working_directory_length = (int)strlen(working_directory);

    LPWSTR working_directory_block = malloc((working_directory_length + 1) * sizeof(WCHAR));

    if (working_directory_block == NULL)
    {
        return NULL;
    }

    int i = 0;

    while (working_directory[i] != 0)
    {
        working_directory_block[i] = (WCHAR)working_directory[i];
        i++;
    }

    working_directory_block[i] = 0;

    return working_directory_block;
}

typedef struct ReadLoopOptions
{
    HANDLE fd;

    Dart_Port port;

    HANDLE hMutex;

    HANDLE stopEvent;

    BOOL ackRead;

} ReadLoopOptions;

static DWORD WINAPI read_loop(LPVOID arg)
{
    ReadLoopOptions *options = (ReadLoopOptions *)arg;

    char buffer[1024];

    while (1)
    {
        DWORD readlen = 0;

        if (options->ackRead)
        {
            HANDLE wait_handles[] = {options->stopEvent, options->hMutex};
            DWORD wait_result = WaitForMultipleObjects(
                2, wait_handles, FALSE, INFINITE);
            if (wait_result != WAIT_OBJECT_0 + 1)
            {
                break;
            }
        }
        else if (WaitForSingleObject(options->stopEvent, 0) == WAIT_OBJECT_0)
        {
            break;
        }

        /* Anonymous pipe reads can block indefinitely. Poll for available
         * data instead, so the stop event always wins during teardown and
         * there is no close-vs-ReadFile race with the owner thread. */
        DWORD available = 0;
        while (available == 0)
        {
            if (WaitForSingleObject(options->stopEvent, 20) == WAIT_OBJECT_0)
            {
                goto done;
            }
            if (!PeekNamedPipe(options->fd, NULL, 0, NULL, &available, NULL))
            {
                goto done;
            }
        }

        if (WaitForSingleObject(options->stopEvent, 0) == WAIT_OBJECT_0)
        {
            break;
        }

        BOOL ok = ReadFile(options->fd, buffer, sizeof(buffer), &readlen, NULL);

        if (!ok)
        {
            break;
        }

        if (readlen <= 0)
        {
            break;
        }

        Dart_CObject result;
        result.type = Dart_CObject_kTypedData;
        result.value.as_typed_data.type = Dart_TypedData_kUint8;
        result.value.as_typed_data.length = readlen;
        result.value.as_typed_data.values = (uint8_t *)buffer;

        Dart_PostCObject_DL(options->port, &result);
    }

done:
    free(options);
    return 0;
}

static HANDLE start_read_thread(HANDLE fd, Dart_Port port, HANDLE mutex,
                                HANDLE stopEvent, BOOL ackRead)
{
    ReadLoopOptions *options = malloc(sizeof(ReadLoopOptions));

    if (options == NULL)
    {
        return NULL;
    }

    options->fd = fd;
    options->port = port;
    options->hMutex = mutex;
    options->stopEvent = stopEvent;
    options->ackRead = ackRead;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, read_loop, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
        return NULL;
    }

    return thread;
}

typedef struct WaitExitOptions
{
    HANDLE pid;

    Dart_Port port;
} WaitExitOptions;

static DWORD WINAPI wait_exit_thread(LPVOID arg)
{
    WaitExitOptions *options = (WaitExitOptions *)arg;

    DWORD exit_code = 0;

    WaitForSingleObject(options->pid, INFINITE);

    GetExitCodeProcess(options->pid, &exit_code);

    CloseHandle(options->pid);

    Dart_PostInteger_DL(options->port, exit_code);

    free(options);

    return 0;
}

static BOOL start_wait_exit_thread(HANDLE pid, Dart_Port port)
{
    WaitExitOptions *options = malloc(sizeof(WaitExitOptions));

    if (options == NULL)
    {
        return FALSE;
    }

    options->pid = pid;
    options->port = port;

    DWORD thread_id;

    HANDLE thread = CreateThread(NULL, 0, wait_exit_thread, options, 0, &thread_id);

    if (thread == NULL)
    {
        free(options);
        return FALSE;
    }

    CloseHandle(thread);
    return TRUE;
}

typedef struct PtyHandle
{
    HANDLE inputWriteSide;

    HANDLE outputReadSide;

    HPCON hPty;

    DWORD dwProcessId;

    BOOL ackRead;

    HANDLE hMutex;

    HANDLE stopEvent;

    HANDLE readThread;

} PtyHandle;

char *error_message = NULL;

FFI_PLUGIN_EXPORT PtyHandle *pty_create(PtyOptions *options)
{
    HANDLE inputReadSide = NULL;
    HANDLE inputWriteSide = NULL;

    HANDLE outputReadSide = NULL;
    HANDLE outputWriteSide = NULL;

    if (!CreatePipe(&inputReadSide, &inputWriteSide, NULL, 0))
    {
        error_message = "Failed to create input pipe";
        return NULL;
    }

    if (!CreatePipe(&outputReadSide, &outputWriteSide, NULL, 0))
    {
        CloseHandle(inputReadSide);
        CloseHandle(inputWriteSide);
        error_message = "Failed to create output pipe";
        return NULL;
    }

    COORD size;

    size.X = options->cols;
    size.Y = options->rows;

    HPCON hPty;

    HRESULT result = CreatePseudoConsole(size, inputReadSide, outputWriteSide, 0, &hPty);

    if (FAILED(result))
    {
        CloseHandle(inputReadSide);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        CloseHandle(outputWriteSide);
        error_message = "Failed to create pseudo console";
        return NULL;
    }

    /* ConPTY duplicates these ends internally; the parent only retains the
     * write end for input and read end for output. */
    CloseHandle(inputReadSide);
    inputReadSide = NULL;
    CloseHandle(outputWriteSide);
    outputWriteSide = NULL;

    STARTUPINFOEXW startupInfo;

    ZeroMemory(&startupInfo, sizeof(startupInfo));
    startupInfo.StartupInfo.cb = sizeof(startupInfo);

    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = NULL;
    startupInfo.StartupInfo.hStdOutput = NULL;
    startupInfo.StartupInfo.hStdError = NULL;

    SIZE_T bytesRequired;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);
    startupInfo.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(bytesRequired);

    if (startupInfo.lpAttributeList == NULL)
    {
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to allocate proc thread attribute list";
        return NULL;
    }

    BOOL ok = InitializeProcThreadAttributeList(startupInfo.lpAttributeList, 1, 0, &bytesRequired);

    if (!ok)
    {
        free(startupInfo.lpAttributeList);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to initialize proc thread attribute list";
        return NULL;
    }

    ok = UpdateProcThreadAttribute(startupInfo.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hPty,
                                   sizeof(hPty),
                                   NULL,
                                   NULL);

    if (!ok)
    {
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        free(startupInfo.lpAttributeList);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to update proc thread attribute list";
        return NULL;
    }

    LPWSTR command = build_command(options->executable, options->arguments);

    LPWSTR environment_block = build_environment(options->environment);

    LPWSTR working_directory = build_working_directory(options->working_directory);

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));

    ok = CreateProcessW(NULL,
                        command,
                        NULL,
                        NULL,
                        FALSE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment_block,
                        working_directory,
                        &startupInfo.StartupInfo,
                        &processInfo);

    if (command != NULL)
    {
        free(command);
    }

    if (environment_block != NULL)
    {
        free(environment_block);
    }

    if (working_directory != NULL)
    {
        free(working_directory);
    }

    DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
    free(startupInfo.lpAttributeList);

    if (!ok)
    {
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to create process";
        DWORD error = GetLastError();
        printf("error no: %lu\n", (unsigned long)error);
        return NULL;
    }

    CloseHandle(processInfo.hThread);

    HANDLE mutex = CreateSemaphore(
        NULL, // default security attributes
        1,    // initial count
        1,    // maximum count
        NULL);

    if (mutex == NULL)
    {
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        CloseHandle(processInfo.hProcess);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to create PTY mutex";
        return NULL;
    }

    HANDLE stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (stopEvent == NULL)
    {
        CloseHandle(mutex);
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        CloseHandle(processInfo.hProcess);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to create PTY stop event";
        return NULL;
    }

    PtyHandle *pty = malloc(sizeof(PtyHandle));

    if (pty == NULL)
    {
        CloseHandle(stopEvent);
        CloseHandle(mutex);
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        CloseHandle(processInfo.hProcess);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        error_message = "Failed to allocate pty handle";
        return NULL;
    }

    pty->inputWriteSide = inputWriteSide;
    pty->outputReadSide = outputReadSide;
    pty->hPty = hPty;
    pty->dwProcessId = processInfo.dwProcessId;
    pty->ackRead = options->ackRead;
    pty->hMutex = mutex;
    pty->stopEvent = stopEvent;
    pty->readThread = NULL;

    pty->readThread = start_read_thread(outputReadSide, options->stdout_port,
                                        mutex, stopEvent, options->ackRead);
    if (pty->readThread == NULL)
    {
        CloseHandle(stopEvent);
        CloseHandle(mutex);
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        CloseHandle(processInfo.hProcess);
        CloseHandle(inputWriteSide);
        CloseHandle(outputReadSide);
        ClosePseudoConsole(hPty);
        free(pty);
        error_message = "Failed to start PTY reader";
        return NULL;
    }

    if (!start_wait_exit_thread(processInfo.hProcess, options->exit_port))
    {
        /* The reader owns no process handle, so terminate and reap it here;
         * pty_close will then stop the reader and release its PTY resources. */
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        pty_close(pty);
        CloseHandle(processInfo.hProcess);
        error_message = "Failed to start PTY exit watcher";
        return NULL;
    }

    return pty;
}

FFI_PLUGIN_EXPORT void pty_write(PtyHandle *handle, char *buffer, int length)
{
    // ConPTY pipes can short-write large bracketed pastes the same way Unix
    // PTYs do; loop until the full buffer (including paste terminators) is out.
    int written = 0;
    while (written < length)
    {
        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(
            handle->inputWriteSide,
            buffer + written,
            (DWORD)(length - written),
            &bytesWritten,
            NULL);
        if (!ok || bytesWritten == 0)
        {
            break;
        }
        written += (int)bytesWritten;
    }

    FlushFileBuffers(handle->inputWriteSide);
}

FFI_PLUGIN_EXPORT void pty_close(PtyHandle *handle)
{
    if (handle == NULL)
    {
        return;
    }

    /* Wake both the ACK wait and the output polling loop before releasing the
     * handles they use. The reader is joined before any of those handles are
     * closed. */
    if (handle->stopEvent != NULL)
    {
        SetEvent(handle->stopEvent);
    }
    if (handle->readThread != NULL)
    {
        WaitForSingleObject(handle->readThread, INFINITE);
        CloseHandle(handle->readThread);
        handle->readThread = NULL;
    }
    if (handle->inputWriteSide != NULL)
    {
        CloseHandle(handle->inputWriteSide);
    }
    if (handle->outputReadSide != NULL)
    {
        CloseHandle(handle->outputReadSide);
    }
    if (handle->hPty != NULL)
    {
        ClosePseudoConsole(handle->hPty);
    }
    if (handle->hMutex != NULL)
    {
        CloseHandle(handle->hMutex);
    }
    if (handle->stopEvent != NULL)
    {
        CloseHandle(handle->stopEvent);
    }
    free(handle);
}

FFI_PLUGIN_EXPORT void pty_ack_read(PtyHandle *handle)
{
    if (handle != NULL && handle->ackRead && handle->hMutex != NULL)
    {
        ReleaseSemaphore(handle->hMutex, 1, NULL);
    }
}

FFI_PLUGIN_EXPORT int pty_resize(PtyHandle *handle, int rows, int cols)
{
    COORD size;

    size.X = cols;
    size.Y = rows;

    return ResizePseudoConsole(handle->hPty, size);
}

FFI_PLUGIN_EXPORT int pty_getpid(PtyHandle *handle)
{
    return (int)handle->dwProcessId;
}

FFI_PLUGIN_EXPORT char *pty_error()
{
    return error_message;
}

FFI_PLUGIN_EXPORT int pty_get_master_fd(PtyHandle *handle)
{
    (void)handle;
    return -1; /* no POSIX fd */
}

FFI_PLUGIN_EXPORT int pty_get_foreground_pgid(PtyHandle *handle)
{
    (void)handle;
    return -1; /* ConPTY has no tcgetpgrp; Phase A documents unsupported */
}

FFI_PLUGIN_EXPORT int pty_get_shell_pgid(PtyHandle *handle)
{
    (void)handle;
    return -1; /* no POSIX process groups on Windows */
}
