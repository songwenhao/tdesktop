#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <functional>

#ifndef NO_PROTOBUF
    #include "pipeCmd.pb.h"
    #pragma comment(lib, "libprotobuf")
#endif

enum class PipeType {
    PipeServer = 0,
    PipeClient
};

using OnCheckStop = std::function<bool(void* ctx)>;
using OnStop = std::function<void(void* ctx)>;

#ifndef NO_PROTOBUF
using OnRecvPipeCmd = std::function<void(void* ctx, const PipeCmd::Cmd& cmd)>;
#endif

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

#ifdef NO_PROTOBUF

    std::uint32_t Recv(
        char* data,
        std::uint32_t dataSize
    );

    std::uint32_t Send(
        const char* data,
        std::uint32_t dataSize
    );

    void RegisterCallback(
        void* ctx,
        OnCheckStop onCheckStop = nullptr,
        OnStop onStop = nullptr
    );

#else

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

    static bool ParsePipeCmd(
        PipeCmd::Cmd& cmd,
        const char* data,
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

#endif

    static std::string GenerateUniqueId(bool isPipeServer);

private:
    class PipeWrapperImpl;
    PipeWrapperImpl* pimpl_;
};