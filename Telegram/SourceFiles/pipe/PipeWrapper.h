#pragma once

#if defined(_MSC_VER) || defined(WIN64) || defined(_WIN64) || defined(__WIN64__) || defined(WIN32) \
|| defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    #define WINDOWS_PLATFORM
#endif

#include <string>
#include <functional>
#include "PipeCmd.h"

using OnCheckStop = std::function<bool(
    void* ctx
)>;
using OnStop = std::function<void(
    void* ctx
)>;
using OnRecvPipeCmd = std::function<void(
    void* ctx,
    const PipeCmd::Cmd& cmd
)>;

class PipeWrapper {
public:
    enum class PipeType {
        PipeServer = 0,
        PipeClient
    };

#ifdef WINDOWS_PLATFORM
    PipeWrapper(
        const std::wstring& pipeName,
        PipeType pipeType
    );
#else
    PipeWrapper(
        const std::string& pipeName,
        PipeType pipeType
    );
#endif

    ~PipeWrapper();

    PipeWrapper(
        const PipeWrapper& rhs
    ) = delete;

    PipeWrapper(
        PipeWrapper&& rhs
    ) = delete;

    PipeWrapper& operator=(
        const PipeWrapper& rhs
    ) = delete;

    PipeWrapper& operator=(
        PipeWrapper&& rhs
    ) = delete;

    bool connectPipe(
        const std::function<bool()>& checkStop = nullptr,
        std::uint64_t maxWaitTime = 30000
    );

    void disConnectPipe();

    void registerCallback(
        void* ctx,
        const OnRecvPipeCmd& onRecvPipeCmd = nullptr,
        const OnCheckStop& onCheckStop = nullptr,
        const OnStop& onStop = nullptr
    );

    PipeCmd::Cmd sendCmd(
        const PipeCmd::Cmd& cmd,
        bool waitDone = true,
        std::uint32_t maxWaitTime = -1,
        const OnRecvPipeCmd& sendCmdCallback = nullptr,
        void* ctx = nullptr
    );

private:
    class PipeWrapperImpl;
    PipeWrapperImpl* impl_;
};
