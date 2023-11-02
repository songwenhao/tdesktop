#include <vector>
#include <map>
#include <thread>
#include <mutex>

#include <combaseapi.h>
#pragma comment(lib, "Ole32")

#include "LogTrace.h"

#include "PipeWrapper.h"

class PipeWrapper::PipeWrapperImpl {

    struct PipeCmdResult {
        PipeCmdResult() {
            this->resultCallback = nullptr;
            this->ctx = nullptr;
            this->signalEvent = nullptr;
        }

        PipeCmdResult(
            OnRecvPipeCmd resultCallback,
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

public:
    PipeWrapperImpl(
        const std::wstring& wstrPipeName,
        const std::wstring& wstrHeartbeatEventName,
        PipeType pipeType
    )
        : isPipeServer_(pipeType == PipeType::PipeServer),
        pipeHandle_(INVALID_HANDLE_VALUE),
        s2c_heartbeatEventHandle_(nullptr),
        c2s_heartbeatEventHandle_(nullptr),
        pipeName_(wstrPipeName),
        heartbeatEventName_(wstrHeartbeatEventName),
        heartbeatThd_(nullptr),
        pipeReadThd_(nullptr),
        stopFlag_(false),
        ctx_(nullptr),
        onRecvPipeCmd_(nullptr),
        onCheckStop_(nullptr),
        onStop_(nullptr),
        logBufMutex_(nullptr),
        logBufA_(nullptr),
        logBufW_(nullptr),
        pipeConnectedEvent_(nullptr),
        pipeCmdResultMapMutex_(nullptr),
        pid_(0) {
        logBufMutex_ = std::make_unique<std::mutex>();
        logBufA_ = std::make_unique<char[]>(logBufSize_);
        logBufW_ = std::make_unique<wchar_t[]>(logBufSize_);
        logPrevStrA_ = (isPipeServer_ ? "[pipe server]" : "[pipe client]");
        logPrevStrW_ = (isPipeServer_ ? L"[pipe server]" : L"[pipe client]");

        pipeConnectedEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        pipeCmdResultMapMutex_ = std::make_unique<std::mutex>();

        pid_ = GetCurrentProcessId();

        if (!heartbeatEventName_.empty()) {
            s2c_heartbeatEventHandle_ = CreateEventW(nullptr, FALSE, FALSE, (heartbeatEventName_ + L"-s2c").c_str());
            c2s_heartbeatEventHandle_ = CreateEventW(nullptr, FALSE, FALSE, (heartbeatEventName_ + L"-c2s").c_str());
        }

        memset(&pipeReadOverlapped_, 0, sizeof(pipeReadOverlapped_));
        pipeReadOverlapped_.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        memset(&pipeWriteOverlapped_, 0, sizeof(pipeWriteOverlapped_));
        pipeWriteOverlapped_.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~PipeWrapperImpl() {
        DisConnectPipe();
    }

    void PipeThd(
        ULONGLONG maxWaitTime
    ) {
        bool connected = false;
        pipeHandle_ = INVALID_HANDLE_VALUE;

        std::wstring logPrevStr = std::wstring(L"[") + __FUNCTIONW__ + L"] connect pipe";

        LogW(L"%s begin ...", logPrevStr.c_str());

        HANDLE pipeHandle = INVALID_HANDLE_VALUE;

        DWORD errCode = 0;

        std::wstring pipeName = pipeName_;

        OVERLAPPED overlapped;
        DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

        do {
            memset(&overlapped, 0, sizeof(overlapped));
            overlapped.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!overlapped.hEvent) {
                break;
            }

            if (isPipeServer_) {
                pipeHandle = CreateNamedPipeW(
                    pipeName.c_str(),
                    PIPE_ACCESS_DUPLEX |
                    FILE_FLAG_OVERLAPPED,
                    pipeMode,
                    1,
                    pipeBufSize_,
                    pipeBufSize_,
                    0,
                    nullptr);

                if (pipeHandle == INVALID_HANDLE_VALUE) {
                    errCode = GetLastError();
                    LogW(L"%s CreateNamedPipe error, error code: %d", logPrevStr.c_str(), errCode);
                    break;
                }

                // Wait for the client to connect; if it succeeds, 
                // the function returns a nonzero value. If the function
                // returns zero, GetLastError returns ERROR_PIPE_CONNECTED. 
                BOOL ret = ConnectNamedPipe(pipeHandle, &overlapped);
                errCode = GetLastError();
                if (errCode == ERROR_IO_PENDING) {
                    DWORD waitCode = 0;
                    ULONGLONG beginTime = GetTickCount64();
                    while (true) {
                        waitCode = WaitForSingleObject(overlapped.hEvent, 1000);
                        if (waitCode == WAIT_TIMEOUT) {
                            if (CheckStop()) {
                                LogW(L"%s stop!!!", logPrevStr.c_str());
                                break;
                            }

                            if (GetTickCount64() - beginTime >= maxWaitTime) {
                                break;
                            }

                            continue;
                        } else if (waitCode == WAIT_OBJECT_0) {
                            connected = true;
                            break;
                        } else {
                            break;
                        }
                    }
                } else if (errCode == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else {
                    LogW(L"%s ConnectNamedPipe error, error code: %d", logPrevStr.c_str(), errCode);
                }
            } else {
                ULONGLONG beginTime = GetTickCount64();

                while (true) {
                    if (CheckStop()) {
                        break;
                    }

                    // Try to open a named pipe; wait for it, if necessary.
                    pipeHandle = CreateFileW(
                        pipeName.c_str(),
                        GENERIC_READ |
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        nullptr);

                    errCode = GetLastError();

                    if (pipeHandle != INVALID_HANDLE_VALUE && errCode == ERROR_SUCCESS) {
                        connected = true;
                    } else if (pipeHandle != INVALID_HANDLE_VALUE && errCode == ERROR_PIPE_BUSY) {
                        // All pipe instances are busy, so wait. 
                        while (true) {
                            if (CheckStop()) {
                                break;
                            }

                            if (WaitNamedPipeW(pipeName.c_str(), 1000)) {
                                connected = true;
                                break;
                            } else {
                                if (GetTickCount64() - beginTime >= maxWaitTime) {
                                    break;
                                }
                            }
                        }
                    }

                    if (connected) {
                        connected = false;

                        if (SetNamedPipeHandleState(pipeHandle, &pipeMode, nullptr, nullptr)) {
                            connected = true;
                        }
                    }

                    if (connected) {
                        break;
                    } else {
                        if (pipeHandle != INVALID_HANDLE_VALUE) {
                            CloseHandle(pipeHandle);
                            pipeHandle = INVALID_HANDLE_VALUE;
                        }

                        if (GetTickCount64() - beginTime >= maxWaitTime) {
                            break;
                        }

                        Sleep(1000);
                    }
                }
            }

        } while (false);

        if (!connected) {
            if (pipeHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(pipeHandle);
                pipeHandle = INVALID_HANDLE_VALUE;
            }
        }

        pipeHandle_ = pipeHandle;
        if (pipeConnectedEvent_) {
            SetEvent(pipeConnectedEvent_);
        }

        if (overlapped.hEvent) {
            CloseHandle(overlapped.hEvent);
        }

        LogW(L"%s end ...", logPrevStr.c_str());

        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            bool ret = false;

            PipeCmd::Cmd cmd;

            while (true) {
                if (CheckStop()) {
                    break;
                }

                ret = RecvCmd(cmd);
                if (!ret) {
                    continue;
                }

                if (onRecvPipeCmd_) {
                    onRecvPipeCmd_(ctx_, cmd);
                }
            }
        }
    }

    bool ConnectPipe(
        std::function<bool()> checkStop,
        ULONGLONG maxWaitTime
    ) {
        bool connected = false;

        do {
            if (!pipeReadOverlapped_.hEvent ||
                !pipeWriteOverlapped_.hEvent ||
                !pipeConnectedEvent_) {
                break;
            }

            ResetEvent(pipeReadOverlapped_.hEvent);
            ResetEvent(pipeWriteOverlapped_.hEvent);
            ResetEvent(pipeConnectedEvent_);

            pipeReadThd_ = new std::thread(std::bind(&PipeWrapperImpl::PipeThd, this, std::placeholders::_1), maxWaitTime);

            DWORD waitCode = -1;

            while (true) {
                if (checkStop && checkStop()) {
                    break;
                }

                waitCode = WaitForSingleObject(pipeConnectedEvent_, 1000);
                if (waitCode == WAIT_OBJECT_0) {
                    connected = pipeHandle_ != INVALID_HANDLE_VALUE;
                    break;
                } else if (waitCode == WAIT_TIMEOUT) {
                    if (maxWaitTime > 1000) {
                        maxWaitTime -= 1000;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
        } while (false);

#ifndef _DEBUG
        if (connected && !heartbeatEventName_.empty()) {
            StartHeartbeatThd();
        }
#endif

        return connected;
    }

    void DisConnectPipe() {
        SetStop();

        if (heartbeatThd_) {
            if (heartbeatThd_->joinable()) {
                heartbeatThd_->join();
            }
            delete heartbeatThd_;
            heartbeatThd_ = nullptr;
        }

        if (pipeReadThd_) {
            if (pipeReadThd_->joinable()) {
                pipeReadThd_->join();
            }
            delete pipeReadThd_;
            pipeReadThd_ = nullptr;
        }

        if (s2c_heartbeatEventHandle_) {
            CloseHandle(s2c_heartbeatEventHandle_);
            s2c_heartbeatEventHandle_ = nullptr;
        }

        if (c2s_heartbeatEventHandle_) {
            CloseHandle(c2s_heartbeatEventHandle_);
            c2s_heartbeatEventHandle_ = nullptr;
        }

        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            if (isPipeServer_) {
                DisconnectNamedPipe(pipeHandle_);
            }

            CloseHandle(pipeHandle_);

            pipeHandle_ = INVALID_HANDLE_VALUE;
        }

        if (pipeReadOverlapped_.hEvent) {
            CloseHandle(pipeReadOverlapped_.hEvent);
            pipeReadOverlapped_.hEvent = nullptr;
        }

        if (pipeWriteOverlapped_.hEvent) {
            CloseHandle(pipeWriteOverlapped_.hEvent);
            pipeWriteOverlapped_.hEvent = nullptr;
        }

        if (pipeConnectedEvent_) {
            CloseHandle(pipeConnectedEvent_);
            pipeConnectedEvent_ = nullptr;
        }
    }

    bool StartHeartbeatThd() {
        bool ret = false;

        do {
            if (!s2c_heartbeatEventHandle_ || !c2s_heartbeatEventHandle_) {
                break;
            }

            ResetEvent(s2c_heartbeatEventHandle_);
            ResetEvent(c2s_heartbeatEventHandle_);

            heartbeatThd_ = new std::thread([this]() {
                int waitTime = 0;
                const int maxWaitTime = 5;
                DWORD waitCode = -1;
                while (true) {
                    if (CheckStop()) {
                        break;
                    }

                    if (isPipeServer_) {
                        //LogW(L"[s to c] set heartbeat event ...");

                        SetEvent(s2c_heartbeatEventHandle_);

                        waitCode = WaitForSingleObject(c2s_heartbeatEventHandle_, 1000);
                        if (waitCode != WAIT_OBJECT_0) {
                            ++waitTime;
                            if (waitTime >= maxWaitTime) {
                                LogW(L" wait for heartbeat event timeout %d seconds, exit ...", waitTime);
                                SetStop();
                                break;
                            }
                        } else {
                            waitTime = 0;
                        }
                    } else {
                        //LogW(L"[c to s] set heartbeat event ...");

                        SetEvent(c2s_heartbeatEventHandle_);

                        waitCode = WaitForSingleObject(s2c_heartbeatEventHandle_, 1000);
                        if (waitCode != WAIT_OBJECT_0) {
                            ++waitTime;
                            if (waitTime >= maxWaitTime) {
                                LogW(L" wait for heartbeat event timeout %d seconds, exit ...", waitTime);
                                SetStop();
                                break;
                            }
                        } else {
                            waitTime = 0;
                        }
                    }
                }
                });

            if (heartbeatThd_ && heartbeatThd_->native_handle()) {
                ret = true;
            }

        } while (false);

        return ret;
    }

    std::uint32_t PipeRead(
        char* data,
        std::uint32_t dataSize
    ) {
        const wchar_t* funcName = __FUNCTIONW__;

        bool ok = false;

        DWORD readSize = 0;

        do {
            if (!data) {
                break;
            }

            pipeReadOverlapped_.Offset = 0;
            if (pipeReadOverlapped_.hEvent) {
                ResetEvent(pipeReadOverlapped_.hEvent);
            }

            BOOL ret = ReadFile(
                pipeHandle_,
                data,
                dataSize,
                &readSize,
                &pipeReadOverlapped_
            );

            if (ret) {
                ok = true;
            } else {
                DWORD errorCode = GetLastError();
                if (errorCode == ERROR_IO_PENDING) {
                    if (pipeReadOverlapped_.hEvent) {
                        while (true) {
                            ret = WaitForSingleObject(pipeReadOverlapped_.hEvent, 1000);
                            if (ret == WAIT_OBJECT_0) {
                                readSize = 0;
                                ret = GetOverlappedResult(pipeHandle_, &pipeReadOverlapped_, &readSize, FALSE);
                                ok = true;
                                break;
                            }

                            if (CheckStop()) {
                                LogW(L"[%s] stopFlag is true!!!", funcName);
                                break;
                            }
                        }
                    }
                } else {
                    LogW(L"[%s] pipe read failed, GetLastError=%d", funcName, GetLastError());
                }
            }

        } while (false);

        if (!ok) {
            readSize = 0;
            SetStop();
            LogW(L"[%s] Pipe read failed, stop!", funcName);
        }

        return (std::uint32_t)readSize;
    }

    std::uint32_t PipeWrite(
        const char* data,
        std::uint32_t dataSize
    ) {
        const wchar_t* funcName = __FUNCTIONW__;

        bool ok = false;
        DWORD writeSize = 0;

        do {
            pipeWriteOverlapped_.Offset = 0;
            if (pipeWriteOverlapped_.hEvent) {
                ResetEvent(pipeWriteOverlapped_.hEvent);
            }

            BOOL ret = WriteFile(
                pipeHandle_,
                data,
                dataSize,
                &writeSize,
                &pipeWriteOverlapped_);

            if (ret) {
                ok = true;
            } else {
                DWORD errorCode = GetLastError();
                if (errorCode == ERROR_IO_PENDING) {
                    while (true) {
                        if (pipeWriteOverlapped_.hEvent) {
                            ret = WaitForSingleObject(pipeWriteOverlapped_.hEvent, 1000);
                            if (ret == WAIT_OBJECT_0) {
                                writeSize = 0;
                                ret = GetOverlappedResult(pipeHandle_, &pipeWriteOverlapped_, &writeSize, FALSE);
                                ok = true;
                                break;
                            }
                        }

                        if (CheckStop()) {
                            LogW(L"[%s] stopFlag is true!!!", funcName);
                            break;
                        }
                    }
                } else {
                    LogW(L"[%s] pipe write failed, GetLastError=%d", funcName, GetLastError());
                }
            }

        } while (false);

        if (!ok) {
            writeSize = 0;
            SetStop();
            LogW(L"[%s] Pipe write failed, stop!", funcName);
        }

        return (std::uint32_t)writeSize;
    }

    bool RecvCmd(PipeCmd::Cmd& cmd) {
        const char* funcName = __FUNCTION__;

        bool ok = false;

        std::uint32_t dataSize = 0;
        char* data = nullptr;

        do {
            cmd.Clear();

            // 接收命令字节数
            std::uint32_t readSize = PipeRead((char*)&dataSize, sizeof(std::uint32_t));
            if (!readSize) {
                break;
            }

            // 接收命令
            try {
                data = new char[dataSize + 2];
            } catch (std::exception& e) {
                (e);
            }

            if (!data) {
                break;
            }

            memset(data, 0, dataSize + 2);

            readSize = PipeRead(data, dataSize);
            if (!readSize) {
                break;
            }

            ok = PipeCmd::BlobDataToCmd(cmd, data, dataSize);
            if (!ok) {
                break;
            }

            LogA("[%s] <== recv cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(), cmd.action, std::uint32_t(cmd.content.size()));

            OnRecvPipeCmd resultCallback = nullptr;
            void* ctx = nullptr;
            HANDLE signalEvent = nullptr;

            {
                // 设置命令执行结果，取出命令注册的信息
                std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                auto iter = pipeCmdResultMap_.find(cmd.uniqueId);
                if (iter != pipeCmdResultMap_.end()) {
                    iter->second.result = cmd;
                    resultCallback = iter->second.resultCallback;
                    ctx = iter->second.ctx;
                    signalEvent = iter->second.signalEvent;
                }
            }

            // 通过判断事件是否存在，决定命令是同步的还是异步的
            // 同步的命令，触发事件
            // 异步的命令，调用命令注册的回调
            if (signalEvent) {
                SetEvent(signalEvent);
            } else {
                if (resultCallback) {
                    resultCallback(ctx, cmd);
                }

                // 异步命令无需保留执行结果
                std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                auto iter = pipeCmdResultMap_.find(cmd.uniqueId);
                if (iter != pipeCmdResultMap_.end()) {
                    pipeCmdResultMap_.erase(iter);
                }
            }

        } while (false);

        if (data) {
            delete[] data;
        }

        return ok;
    }

    PipeCmd::Cmd SendCmd(
        const PipeCmd::Cmd& cmd,
        bool waitDone,
        DWORD waitTime,
        OnRecvPipeCmd sendCmdCallback,
        void* ctx
    ) {
        const char* funcName = __FUNCTION__;

        PipeCmd::Cmd resultCmd;

        char* buf = nullptr;

        HANDLE signalEvent = nullptr;

        do {
            if (CheckStop()) {
                break;
            }

            // 同步命令设置事件
            if (waitDone && cmd.action > -1) {
                signalEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }

            {
                // 注册命令
                std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                pipeCmdResultMap_.insert({ cmd.uniqueId, PipeCmdResult(sendCmdCallback, ctx, signalEvent) });
            }

            std::uint32_t dataSize = 0;
            buf = PipeCmd::CmdToBlobData(cmd, dataSize);
            if (!buf || !dataSize) {
                break;
            }

            std::uint32_t writeSize = PipeWrite((const char*)&dataSize, sizeof(std::uint32_t));
            if (!writeSize) {
                break;
            }

            writeSize = PipeWrite(buf, dataSize);
            if (!writeSize) {
                break;
            }

            LogA("[%s] ==> send cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(), cmd.action, std::uint32_t(cmd.content.size()));

            // 同步命令等待结果
            if (waitDone && cmd.action > -1) {
                DWORD waitCode = -1;

                if (signalEvent) {
                    while (!CheckStop() && waitTime > 0) {
                        waitCode = WaitForSingleObject(signalEvent, 1000);
                        if (waitCode != WAIT_TIMEOUT) {
                            break;
                        }

                        if (waitTime > 1000) {
                            waitTime -= 1000;
                        } else {
                            break;
                        }
                    }
                }

                {
                    std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                    auto iter = pipeCmdResultMap_.find(cmd.uniqueId);
                    if (iter != pipeCmdResultMap_.end()) {
                        resultCmd = iter->second.result;
                        pipeCmdResultMap_.erase(iter);
                    }
                }
            }
        } while (false);

        if (buf) {
            delete[] buf;
            buf = nullptr;
        }

        if (signalEvent) {
            CloseHandle(signalEvent);
            signalEvent = nullptr;
        }

        return resultCmd;
    }

    void RegisterCallback(
        void* ctx,
        OnRecvPipeCmd onRecvPipeCmd,
        OnCheckStop onCheckStop,
        OnStop onStop
    ) {
        ctx_ = ctx;
        onRecvPipeCmd_ = onRecvPipeCmd;
        onCheckStop_ = onCheckStop;
        onStop_ = onStop;
    }

    bool CheckStop() {
        bool stop = false;

        do {
            if (stopFlag_) {
                stop = true;
                break;
            }

            if (onCheckStop_ && onCheckStop_(ctx_)) {
                stop = true;
                SetStop();
                break;
            }

        } while (false);

        return stop;
    }

    void SetStop() {
        if (!stopFlag_) {
            stopFlag_ = true;

            if (onStop_) {
                onStop_(ctx_);
            }
        }

    }

    void LogA(const char* format, ...) {
#ifndef _DEBUG
        return;
#endif

        std::lock_guard<std::mutex> locker(*logBufMutex_);

        va_list args;
        va_start(args, format);

        size_t len = logBufSize_;

        char* buffer = logBufA_.get();
        if (buffer) {
            // time
            SYSTEMTIME sysTime;
            memset(&sysTime, 0, sizeof(SYSTEMTIME));
            GetSystemTime(&sysTime);

            size_t offset = 0;

            _snprintf_s(buffer + offset, len - offset, _TRUNCATE, "[%04u/%02u/%02u %02u:%02u:%02u] ", sysTime.wYear,
                sysTime.wMonth, sysTime.wDay, sysTime.wHour + 8, sysTime.wMinute, sysTime.wSecond);
            offset = strlen(buffer);

            strncat_s(buffer + offset, len - offset, logPrevStrA_.c_str(), _TRUNCATE);
            offset = strlen(buffer);

            _vsnprintf_s(buffer + offset, len - offset, _TRUNCATE, format, args);
            offset = strlen(buffer);

            strncat_s(buffer + offset, len - offset, "\r\n", _TRUNCATE);

            OutputDebugStringA(buffer);
        }

        va_end(args);
    }

    void LogW(const wchar_t* format, ...) {
#ifndef _DEBUG
        return;
#endif

        std::lock_guard<std::mutex> locker(*logBufMutex_);

        va_list args;
        va_start(args, format);

        size_t len = logBufSize_;

        wchar_t* buffer = logBufW_.get();
        if (buffer) {
            // time
            SYSTEMTIME sysTime;
            memset(&sysTime, 0, sizeof(SYSTEMTIME));
            GetSystemTime(&sysTime);

            size_t offset = 0;

            _snwprintf_s(buffer + offset, len - offset, _TRUNCATE, L"[%04u/%02u/%02u %02u:%02u:%02u] ", sysTime.wYear,
                sysTime.wMonth, sysTime.wDay, sysTime.wHour + 8, sysTime.wMinute, sysTime.wSecond);
            offset = wcslen(buffer);

            wcsncat_s(buffer + offset, len - offset, logPrevStrW_.c_str(), _TRUNCATE);
            offset = wcslen(buffer);

            _vsnwprintf_s(buffer + offset, len - offset, _TRUNCATE, format, args);
            offset = wcslen(buffer);

            wcsncat_s(buffer + offset, len - offset, L"\r\n", _TRUNCATE);

            OutputDebugStringW(buffer);
        }

        va_end(args);
    }

private:
    bool isPipeServer_;
    HANDLE pipeHandle_;
    HANDLE s2c_heartbeatEventHandle_;
    HANDLE c2s_heartbeatEventHandle_;
    std::wstring pipeName_;
    std::wstring heartbeatEventName_;
    std::thread* heartbeatThd_;
    std::thread* pipeReadThd_;
    bool stopFlag_;

    void* ctx_;

    OnRecvPipeCmd onRecvPipeCmd_;
    OnCheckStop onCheckStop_;
    OnStop onStop_;

    OVERLAPPED pipeReadOverlapped_;
    OVERLAPPED pipeWriteOverlapped_;
    const std::uint32_t pipeBufSize_ = 4096;

    std::unique_ptr<std::mutex> logBufMutex_;
    std::string logPrevStrA_;
    std::wstring logPrevStrW_;
    std::unique_ptr<char[]> logBufA_;
    std::unique_ptr<wchar_t[]> logBufW_;
    const std::uint32_t logBufSize_ = 1024 * 1024;

    HANDLE pipeConnectedEvent_;

    std::unique_ptr<std::mutex> pipeCmdResultMapMutex_;

    std::map<std::string, PipeCmdResult> pipeCmdResultMap_;

    DWORD pid_;
};

PipeWrapper::PipeWrapper(
    const std::wstring& pipeName,
    const std::wstring& heartbeatEventName,
    PipeType pipeType
) : pimpl_(new PipeWrapperImpl(pipeName, heartbeatEventName, pipeType)) {}

PipeWrapper::~PipeWrapper() {
    if (pimpl_) {
        delete pimpl_;
    }
}

bool PipeWrapper::ConnectPipe(
    std::function<bool()> checkStop,
    ULONGLONG maxWaitTime
) {
    return pimpl_->ConnectPipe(checkStop, maxWaitTime);
}

void PipeWrapper::DisConnectPipe() {
    return pimpl_->DisConnectPipe();
}

void PipeWrapper::RegisterCallback(
    void* ctx,
    OnRecvPipeCmd onRecvPipeCmd,
    OnCheckStop onCheckStop,
    OnStop onStop
) {
    pimpl_->RegisterCallback(ctx, onRecvPipeCmd, onCheckStop, onStop);
}

PipeCmd::Cmd PipeWrapper::SendCmd(
    const PipeCmd::Cmd& cmd,
    bool waitDone,
    DWORD waitTime,
    OnRecvPipeCmd sendCmdCallback,
    void* ctx
) {
    return pimpl_->SendCmd(cmd, waitDone, waitTime, sendCmdCallback, ctx);
}
