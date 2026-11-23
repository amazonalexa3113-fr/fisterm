#pragma once

#include <sys/types.h>
#include <string>
#include <cstddef>

class PTY
{
public:
    PTY() = default;
    ~PTY();

    PTY(const PTY&) = delete;
    PTY& operator=(const PTY&) = delete;

    bool Start(
        const std::string& shell,
        int columns,
        int rows
    );

    ssize_t Read(char* buffer, size_t size);
    bool Write(const char* data, size_t size);

    void Resize(int columns, int rows);

    bool IsRunning();
    void Stop();

    int GetMasterFD() const;
    pid_t GetPID() const;

private:
    int masterFD = -1;
    pid_t childPID = -1;
};
