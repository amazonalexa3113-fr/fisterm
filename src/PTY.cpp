#include "PTY.hpp"

#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

PTY::~PTY()
{
    Stop();
}

bool PTY::Start(
    const std::string& shell,
    int columns,
    int rows
)
{
    if (masterFD >= 0 || childPID > 0)
        return false;

    struct winsize ws{};
    ws.ws_col = columns;
    ws.ws_row = rows;

    pid_t pid = forkpty(&masterFD, nullptr, nullptr, &ws);

    if (pid < 0)
    {
        masterFD = -1;
        return false;
    }

    if (pid == 0)
    {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        execl(shell.c_str(), shell.c_str(), "-i", nullptr);
        _exit(127);
    }

    childPID = pid;

    int flags = fcntl(masterFD, F_GETFL, 0);

    if (flags >= 0)
        fcntl(masterFD, F_SETFL, flags | O_NONBLOCK);

    return true;
}

ssize_t PTY::Read(char* buffer, size_t size)
{
    if (masterFD < 0)
        return -1;

    ssize_t result = read(masterFD, buffer, size);

    if (result < 0 && errno == EIO)
        return 0;

    return result;
}

bool PTY::Write(const char* data, size_t size)
{
    if (masterFD < 0)
        return false;

    size_t offset = 0;

    while (offset < size)
    {
        ssize_t result = write(
            masterFD,
            data + offset,
            size - offset
        );

        if (result < 0)
        {
            if (errno == EINTR)
                continue;

            return false;
        }

        if (result == 0)
            return false;

        offset += static_cast<size_t>(result);
    }

    return true;
}

void PTY::Resize(int columns, int rows)
{
    if (masterFD < 0)
        return;

    struct winsize ws{};
    ws.ws_col = columns;
    ws.ws_row = rows;

    ioctl(masterFD, TIOCSWINSZ, &ws);
}

bool PTY::IsRunning()
{
    if (childPID <= 0)
        return false;

    int status = 0;
    pid_t result = waitpid(childPID, &status, WNOHANG);

    if (result == childPID)
    {
        childPID = -1;
        return false;
    }

    if (result < 0 && errno == ECHILD)
    {
        childPID = -1;
        return false;
    }

    return result == 0 || (result < 0 && errno == EINTR);
}

void PTY::Stop()
{
    if (childPID > 0)
    {
        kill(-childPID, SIGHUP);
        kill(childPID, SIGTERM);

        int status = 0;
        bool exited = false;

        for (int i = 0; i < 50; ++i)
        {
            pid_t result = waitpid(
                childPID,
                &status,
                WNOHANG
            );

            if (result == childPID ||
                (result < 0 && errno == ECHILD))
            {
                exited = true;
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(10)
            );
        }

        if (!exited)
        {
            kill(-childPID, SIGKILL);
            kill(childPID, SIGKILL);

            while (waitpid(childPID, &status, 0) < 0 &&
                   errno == EINTR)
            {
            }
        }

        childPID = -1;
    }

    if (masterFD >= 0)
    {
        close(masterFD);
        masterFD = -1;
    }
}

int PTY::GetMasterFD() const
{
    return masterFD;
}

pid_t PTY::GetPID() const
{
    return childPID;
}
