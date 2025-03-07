#ifndef SOCKETWRAPPER_H
#define SOCKETWRAPPER_H

#include <string>
#include <functional>
#include "Command.h"

enum class SocketType {
    SocketServer = 0,
    SocketClient
};

using OnCheckStop = std::function<bool(
    void* ctx
    )>;
using OnStop = std::function<void(
    void* ctx
    )>;
using OnRecvCmd = std::function<void(
    void* ctx,
    const Command::Cmd& cmd
    )>;

class SocketWrapper {
public:
    SocketWrapper(
        const std::string& host,
        std::uint16_t port,
        SocketType socketType
        );

    ~SocketWrapper();

    SocketWrapper(
        const SocketWrapper& rhs
        ) = delete;

    SocketWrapper(
        SocketWrapper&& rhs
        ) = delete;

    SocketWrapper& operator=(
        const SocketWrapper& rhs
        ) = delete;

    SocketWrapper& operator=(
        SocketWrapper&& rhs
        ) = delete;

    bool init();

    std::uint16_t getServerSocketPort() const;

    bool connectSocket(
        const std::function<bool()>& checkStop = nullptr,
        std::uint64_t maxWaitTime = 30000
        );

    void disConnectSocket();

    void registerCallback(
        void* ctx,
        const OnRecvCmd& onRecvPipeCmd = nullptr,
        const OnCheckStop& onCheckStop = nullptr,
        const OnStop& onStop = nullptr
        );

    Command::Cmd sendCmd(
        const Command::Cmd& cmd,
        bool waitDone = true,
        std::uint32_t maxWaitTime = -1,
        const OnRecvCmd& sendCmdCallback = nullptr,
        void* ctx = nullptr
        );

private:
    class SocketWrapperImpl;
    SocketWrapperImpl* impl_;
};

#endif // SOCKETWRAPPER_H
