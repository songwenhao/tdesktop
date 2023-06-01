#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <map>
#include <thread>
#include <functional>
#include <mutex>

#include "pipeCmd.pb.h"
#pragma comment(lib, "libprotobuf")

enum class PipeType {
    PipeServer = 0,
    PipeClient
};

using OnRecvPipeCmd = std::function<void(void* ctx, const PipeCmd::Cmd& cmd)>;
using OnCheckStop = std::function<bool(void* ctx)>;
using OnStop = std::function<void(void* ctx)>;

struct PipeCmdResult {
    PipeCmdResult() {
        this->resultCallback = nullptr;
        this->ctx = nullptr;
        this->signalEvent = nullptr;
    }

    PipeCmdResult(
        const OnRecvPipeCmd& resultCallback,
        void* ctx,
        HANDLE signalEvent
    ) {
        this->resultCallback = resultCallback;
        this->ctx = ctx;
        this->signalEvent = signalEvent;
    }

    PipeCmd::Cmd result;
    OnRecvPipeCmd resultCallback;
    void* ctx;
    HANDLE signalEvent;
};

class PipeWrapper {
public:
    PipeWrapper(
        const std::wstring& pipeName,
        const std::wstring& heartbeatEventName,
        PipeType pipeType
    );

    ~PipeWrapper();

    bool ConnectPipe(ULONGLONG maxWaitTime = -1);

    void DisConnectPipe();

    PipeCmd::Cmd SendCmd(
        const PipeCmd::Cmd& cmd,
        bool waitDone = true,
        DWORD waitTime = -1,
        const OnRecvPipeCmd& sendCmdCallback = nullptr,
        void* ctx = nullptr
    );

    void RegisterCallback(
        void* ctx,
        const OnRecvPipeCmd& onRecvPipeCmd,
        const OnCheckStop& onCheckStop,
        const OnStop& onStop
    );

    static bool ParsePipeCmd(
        PipeCmd::Cmd& cmd,
        const unsigned char* data,
        std::uint32_t dataSize
    );

    static void AddExtraData(
        PipeCmd::Cmd& cmd,
        const std::string& key,
        const std::string& value
    );

    static void AddExtraData(
        PipeCmd::Cmd& cmd,
        const std::string& key,
        int value
    );

    static void AddExtraData(
        PipeCmd::Cmd& cmd,
        const std::string& key,
        unsigned int value
    );

    static void AddExtraData(
        PipeCmd::Cmd& cmd,
        const std::string& key,
        long long value
    );

    static void AddExtraData(
        PipeCmd::Cmd& cmd,
        const std::string& key,
        unsigned long long value
    );

    static void AddExtraData(
        PipeCmd::Cmd& cmd,
        const std::string& key,
        double value
    );

    static std::string GetStringExtraData(
        const PipeCmd::Cmd& cmd,
        const std::string& key
    );

    static long long GetNumExtraData(
        const PipeCmd::Cmd& cmd,
        const std::string& key
    );

    static double GetRealExtraData(
        const PipeCmd::Cmd& cmd,
        const std::string& key
    );

    static bool GetBooleanExtraData(
        const PipeCmd::Cmd& cmd,
        const std::string& key
    );

    static std::string Utf16ToUtf8(const std::wstring& str);

    static std::wstring Utf8ToUtf16(const std::string& str);
private:
    class PipeWrapperImpl;
    PipeWrapperImpl* pimpl_;
};