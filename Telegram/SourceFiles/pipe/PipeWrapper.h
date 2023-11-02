#pragma once


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <functional>
#include "PipeCmd.h"

enum class PipeType {
    PipeServer = 0,
    PipeClient
};

using OnCheckStop = std::function<bool(void* ctx)>;
using OnStop = std::function<void(void* ctx)>;
using OnRecvPipeCmd = std::function<void(void* ctx, const PipeCmd::Cmd& cmd)>;

class PipeWrapper {
public:
    PipeWrapper(
        const std::wstring& pipeName,
        const std::wstring& heartbeatEventName,
        PipeType pipeType
    );

    ~PipeWrapper();

    PipeWrapper(const PipeWrapper& rhs) = delete;
    PipeWrapper(PipeWrapper&& rhs) = delete;
    PipeWrapper& operator=(const PipeWrapper& rhs) = delete;
    PipeWrapper& operator=(PipeWrapper&& rhs) = delete;

    bool ConnectPipe(
        std::function<bool()> checkStop = nullptr,
        ULONGLONG maxWaitTime = 30000
    );

    void DisConnectPipe();

    void RegisterCallback(
        void* ctx,
        OnRecvPipeCmd onRecvPipeCmd = nullptr,
        OnCheckStop onCheckStop = nullptr,
        OnStop onStop = nullptr
    );

    PipeCmd::Cmd SendCmd(
        const PipeCmd::Cmd& cmd,
        bool waitDone = true,
        DWORD waitTime = -1,
        OnRecvPipeCmd sendCmdCallback = nullptr,
        void* ctx = nullptr
    );

private:
    class PipeWrapperImpl;
    PipeWrapperImpl* pimpl_;
};