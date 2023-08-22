#include <vector>
#include <map>
#include <thread>
#include <mutex>

#include <combaseapi.h>
#pragma comment(lib, "Ole32")

#include "LogTrace.h"

#include "PipeWrapper.h"

class PipeWrapper::PipeWrapperImpl {

#ifndef NO_PROTOBUF
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
#endif

public:
    PipeWrapperImpl(
        const std::wstring& wstrPipeName,
        const std::wstring& wstrHeartbeatEventName,
        PipeType pipeType
    )
        : isPipeServer_(pipeType == PipeType::PipeServer),
        readPipeHandle_(INVALID_HANDLE_VALUE),
        writePipeHandle_(INVALID_HANDLE_VALUE),
        s2c_heartbeatEventHandle_(nullptr),
        c2s_heartbeatEventHandle_(nullptr),
        pipeName_(wstrPipeName),
        heartbeatEventName_(wstrHeartbeatEventName),
        heartbeatThd_(nullptr),
        pipeReadThd_(nullptr),
        pipeWriteThd_(nullptr),
        stopFlag_(false),
        ctx_(nullptr),
#ifndef NO_PROTOBUF
        onRecvPipeCmd_(nullptr),
#endif
        onCheckStop_(nullptr),
        onStop_(nullptr),
        logBufMutex_(nullptr),
        logBufA_(nullptr),
        logBufW_(nullptr),
        readPipeConnectedEvent_(nullptr),
        writePipeConnectedEvent_(nullptr),
        pipeCmdResultMapMutex_(nullptr),
        pid_(0) {
        logBufMutex_ = std::make_unique<std::mutex>();
        logBufA_ = std::make_unique<char[]>(logBufSize_);
        logBufW_ = std::make_unique<wchar_t[]>(logBufSize_);
        logPrevStrA_ = (isPipeServer_ ? "[pipe server]" : "[pipe client]");
        logPrevStrW_ = (isPipeServer_ ? L"[pipe server]" : L"[pipe client]");

        readPipeConnectedEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        writePipeConnectedEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

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
        bool isReadPipe,
        ULONGLONG maxWaitTime
    ) {
        bool connected = false;
        bool isReadPipeHandle = false;
        std::wstring logPrevStr;

        if (isReadPipe) {
            logPrevStr = std::wstring(L"[") + __FUNCTIONW__ + L"] Connect read pipe";
        } else {
            logPrevStr = std::wstring(L"[") + __FUNCTIONW__ + L"] Connect write pipe";
        }

        LogW(L"%s begin ...", logPrevStr.c_str());

        HANDLE pipeHandle = INVALID_HANDLE_VALUE;

        /*
        * | server |         | client |
        *
        * |write pipe|      |read pipe|
        *             \    /
        *              \  /
        *               \/
        *               /\
        *              /  \
        *             /    \
        *            /      \
        * |read pipe|        |write pipe|
        *
        */
        if (isReadPipe) {
            if (isPipeServer_) {
                readPipeHandle_ = INVALID_HANDLE_VALUE;
            } else {
                writePipeHandle_ = INVALID_HANDLE_VALUE;
            }
        } else {
            if (isPipeServer_) {
                writePipeHandle_ = INVALID_HANDLE_VALUE;
            } else {
                readPipeHandle_ = INVALID_HANDLE_VALUE;
            }
        }

        DWORD errCode = 0;

        std::wstring pipeName = pipeName_;
        if (isReadPipe) {
            pipeName += L"-read";
        } else {
            pipeName += L"-write";
        }

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

        if (isReadPipe) {
            if (isPipeServer_) {
                readPipeHandle_ = pipeHandle;
                isReadPipeHandle = true;
            } else {
                writePipeHandle_ = pipeHandle;
            }

            if (readPipeConnectedEvent_) {
                SetEvent(readPipeConnectedEvent_);
            }
        } else {
            if (isPipeServer_) {
                writePipeHandle_ = pipeHandle;
            } else {
                readPipeHandle_ = pipeHandle;
                isReadPipeHandle = true;
            }

            if (writePipeConnectedEvent_) {
                SetEvent(writePipeConnectedEvent_);
            }
        }

        if (overlapped.hEvent) {
            CloseHandle(overlapped.hEvent);
        }

        LogW(L"%s end ...", logPrevStr.c_str());

#ifndef NO_PROTOBUF
        if (isReadPipeHandle && readPipeConnectedEvent_ != INVALID_HANDLE_VALUE) {
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
#endif
    }

    bool ConnectPipe(
        std::function<bool()> checkStop,
        ULONGLONG maxWaitTime
    ) {
        bool connected = false;

        do {
            if (!pipeReadOverlapped_.hEvent ||
                !pipeWriteOverlapped_.hEvent ||
                !readPipeConnectedEvent_ ||
                !writePipeConnectedEvent_) {
                break;
            }

            ResetEvent(pipeReadOverlapped_.hEvent);
            ResetEvent(pipeWriteOverlapped_.hEvent);
            ResetEvent(readPipeConnectedEvent_);
            ResetEvent(writePipeConnectedEvent_);

            pipeReadThd_ = new std::thread(std::bind(&PipeWrapperImpl::PipeThd, this, std::placeholders::_1, std::placeholders::_2), true, maxWaitTime);

            pipeWriteThd_ = new std::thread(std::bind(&PipeWrapperImpl::PipeThd, this, std::placeholders::_1, std::placeholders::_2), false, maxWaitTime);

            DWORD waitCode = -1;
            HANDLE eventHandles[2] = { readPipeConnectedEvent_, writePipeConnectedEvent_ };

            while (true) {
                if (checkStop && checkStop()) {
                    break;
                }

                waitCode = WaitForMultipleObjects(_countof(eventHandles), eventHandles, TRUE, 1000);
                if (waitCode == WAIT_OBJECT_0) {
                    connected = readPipeHandle_ != INVALID_HANDLE_VALUE && writePipeHandle_ != INVALID_HANDLE_VALUE;
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

        if (pipeWriteThd_) {
            if (pipeWriteThd_->joinable()) {
                pipeWriteThd_->join();
            }
            delete pipeWriteThd_;
            pipeWriteThd_ = nullptr;
        }

        if (s2c_heartbeatEventHandle_) {
            CloseHandle(s2c_heartbeatEventHandle_);
            s2c_heartbeatEventHandle_ = nullptr;
        }

        if (c2s_heartbeatEventHandle_) {
            CloseHandle(c2s_heartbeatEventHandle_);
            c2s_heartbeatEventHandle_ = nullptr;
        }

        if (readPipeHandle_ != INVALID_HANDLE_VALUE) {
            if (isPipeServer_) {
                DisconnectNamedPipe(readPipeHandle_);
            }

            CloseHandle(readPipeHandle_);

            readPipeHandle_ = INVALID_HANDLE_VALUE;
        }

        if (writePipeHandle_ != INVALID_HANDLE_VALUE) {
            if (isPipeServer_) {
                DisconnectNamedPipe(writePipeHandle_);
            }

            CloseHandle(writePipeHandle_);

            writePipeHandle_ = INVALID_HANDLE_VALUE;
        }

        if (pipeReadOverlapped_.hEvent) {
            CloseHandle(pipeReadOverlapped_.hEvent);
            pipeReadOverlapped_.hEvent = nullptr;
        }

        if (pipeWriteOverlapped_.hEvent) {
            CloseHandle(pipeWriteOverlapped_.hEvent);
            pipeWriteOverlapped_.hEvent = nullptr;
        }

        if (readPipeConnectedEvent_) {
            CloseHandle(readPipeConnectedEvent_);
            readPipeConnectedEvent_ = nullptr;
        }

        if (writePipeConnectedEvent_) {
            CloseHandle(writePipeConnectedEvent_);
            writePipeConnectedEvent_ = nullptr;
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
                readPipeHandle_,
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
                                ret = GetOverlappedResult(readPipeHandle_, &pipeReadOverlapped_, &readSize, FALSE);
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

            readSize = readSize;

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
                writePipeHandle_,
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
                                ret = GetOverlappedResult(writePipeHandle_, &pipeWriteOverlapped_, &writeSize, FALSE);
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

#ifdef NO_PROTOBUF
    
    void RegisterCallback(
        void* ctx,
        OnCheckStop onCheckStop,
        OnStop onStop
    ) {
        ctx_ = ctx;
        onCheckStop_ = onCheckStop;
        onStop_ = onStop;
    }

#else

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

            ok = ParsePipeCmd(cmd, data, dataSize);
            if (!ok) {
                break;
            }

            LogA("[%s] <== recv cmd, unique ID: %s action: %d content: %s", funcName, cmd.unique_id().c_str(), cmd.action(), cmd.content().c_str());

            OnRecvPipeCmd resultCallback = nullptr;
            void* ctx = nullptr;
            HANDLE signalEvent = nullptr;

            {
                // 设置命令执行结果，取出命令注册的信息
                std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                auto iter = pipeCmdResultMap_.find(cmd.unique_id());
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
                auto iter = pipeCmdResultMap_.find(cmd.unique_id());
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

        HANDLE signalEvent = nullptr;

        do {
            if (CheckStop()) {
                break;
            }

            PipeCmd::Cmd copyCmd = cmd;
            if (copyCmd.unique_id().empty()) {
                copyCmd.set_unique_id(GenerateUniqueId(isPipeServer_));
            }

            // 同步命令设置事件
            if (waitDone && copyCmd.action() > -1) {
                signalEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }

            {
                // 注册命令
                std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                pipeCmdResultMap_.insert({ copyCmd.unique_id(), PipeCmdResult(sendCmdCallback, ctx, signalEvent) });
            }

            std::vector<char> buf;
            buf.resize(copyCmd.ByteSizeLong());
            if (!copyCmd.SerializeToArray(buf.data(), (int)buf.size())) {
                break;
            }

            std::uint32_t dataSize = (std::uint32_t)buf.size();
            std::uint32_t writeSize = PipeWrite((const char*)&dataSize, sizeof(std::uint32_t));
            if (!writeSize) {
                break;
            }

            writeSize = PipeWrite(buf.data(), dataSize);
            if (!writeSize) {
                break;
            }

            LogA("[%s] ==> send cmd, unique ID: %s action: %d content: %s", funcName, copyCmd.unique_id().c_str(), copyCmd.action(), copyCmd.content().c_str());

            // 同步命令等待结果
            if (waitDone && copyCmd.action() > -1) {
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
                    auto iter = pipeCmdResultMap_.find(copyCmd.unique_id());
                    if (iter != pipeCmdResultMap_.end()) {
                        resultCmd = iter->second.result;
                        pipeCmdResultMap_.erase(iter);
                    }
                }
            }
        } while (false);

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

#endif

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
    HANDLE readPipeHandle_;
    HANDLE writePipeHandle_;
    HANDLE s2c_heartbeatEventHandle_;
    HANDLE c2s_heartbeatEventHandle_;
    std::wstring pipeName_;
    std::wstring heartbeatEventName_;
    std::thread* heartbeatThd_;
    std::thread* pipeReadThd_;
    std::thread* pipeWriteThd_;
    bool stopFlag_;

    void* ctx_;

#ifndef NO_PROTOBUF
    OnRecvPipeCmd onRecvPipeCmd_;
#endif

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

    HANDLE readPipeConnectedEvent_;
    HANDLE writePipeConnectedEvent_;

    std::unique_ptr<std::mutex> pipeCmdResultMapMutex_;

#ifndef NO_PROTOBUF
    std::map<std::string, PipeCmdResult> pipeCmdResultMap_;
#endif

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

#ifdef NO_PROTOBUF

std::uint32_t PipeWrapper::Recv(
    char* data,
    std::uint32_t dataSize
) {
    return pimpl_->PipeRead(data, dataSize);
}

std::uint32_t PipeWrapper::Send(
    const char* data,
    std::uint32_t dataSize
) {
    return pimpl_->PipeWrite(data, dataSize);
}

void PipeWrapper::RegisterCallback(
    void* ctx,
    OnCheckStop onCheckStop,
    OnStop onStop
) {
    pimpl_->RegisterCallback(ctx, onCheckStop, onStop);
}

#else

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

bool PipeWrapper::ParsePipeCmd(
    PipeCmd::Cmd& cmd,
    const char* data,
    std::uint32_t dataSize
) {
    bool ok = false;

    if (data && dataSize > 0 && cmd.ParseFromArray(data, dataSize)) {
        ok = true;
    }

    return ok;
}

void PipeWrapper::AddExtraData(
    PipeCmd::Cmd& cmd,
    const std::string& key,
    const std::string& value
) {
    PipeCmd::Extra* extra = cmd.add_extra();
    if (extra) {
        extra->set_type(PipeCmd::ExtraType::String);
        extra->set_key(key);
        extra->set_string_value(value);
    }
}

void PipeWrapper::AddExtraData(
    PipeCmd::Cmd& cmd,
    const std::string& key,
    int value
) {
    return AddExtraData(cmd, key, (long long)value);
}

void PipeWrapper::AddExtraData(
    PipeCmd::Cmd& cmd,
    const std::string& key,
    unsigned int value
) {
    return AddExtraData(cmd, key, (unsigned long long)value);
}

void PipeWrapper::AddExtraData(
    PipeCmd::Cmd& cmd,
    const std::string& key,
    long long value
) {
    PipeCmd::Extra* extra = cmd.add_extra();
    if (extra) {
        extra->set_type(PipeCmd::ExtraType::Num);
        extra->set_key(key);
        extra->set_num_value(value);
    }
}

void PipeWrapper::AddExtraData(
    PipeCmd::Cmd& cmd,
    const std::string& key,
    unsigned long long value
) {
    PipeCmd::Extra* extra = cmd.add_extra();
    if (extra) {
        extra->set_type(PipeCmd::ExtraType::Num);
        extra->set_key(key);
        extra->set_num_value(value);
    }
}

void PipeWrapper::AddExtraData(
    PipeCmd::Cmd& cmd,
    const std::string& key,
    double value
) {
    PipeCmd::Extra* extra = cmd.add_extra();
    if (extra) {
        extra->set_type(PipeCmd::ExtraType::Real);
        extra->set_key(key);
        extra->set_real_value(value);
    }
}

std::string PipeWrapper::GetStringExtraData(
    const PipeCmd::Cmd& cmd,
    const std::string& key
) {
    std::string data;

    for (const auto& extra : cmd.extra()) {
        if (extra.key() == key && extra.type() == PipeCmd::ExtraType::String) {
            data = extra.string_value();
            break;
        }
    }

    return data;
}

long long PipeWrapper::GetNumExtraData(
    const PipeCmd::Cmd& cmd,
    const std::string& key
) {
    long long data = -1LL;

    for (const auto& extra : cmd.extra()) {
        if (extra.key() == key && extra.type() == PipeCmd::ExtraType::Num) {
            data = extra.num_value();
            break;
        }
    }

    return data;
}

double PipeWrapper::GetRealExtraData(
    const PipeCmd::Cmd& cmd,
    const std::string& key
) {
    double data = 0.0;

    for (const auto& extra : cmd.extra()) {
        if (extra.key() == key && extra.type() == PipeCmd::ExtraType::Real) {
            data = extra.real_value();
            break;
        }
    }

    return data;
}

bool PipeWrapper::GetBooleanExtraData(
    const PipeCmd::Cmd& cmd,
    const std::string& key
) {
    bool data = false;

    for (const auto& extra : cmd.extra()) {
        if (extra.key() == key && extra.type() == PipeCmd::ExtraType::Num) {
            data = extra.num_value() != 0;
            break;
        }
    }

    return data;
}
#endif

std::string PipeWrapper::GenerateUniqueId(bool isPipeServer) {
    std::string uniqueId;
    char buf[256] = { 0 };
    GUID guid = { 0 };

    HRESULT ret = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(::CoCreateGuid(&guid))) {
        _snprintf_s(buf, _countof(buf), _TRUNCATE,
            "%s-{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            (isPipeServer ? "[pipe server]" : "[pipe client]"),
            guid.Data1,
            guid.Data2,
            guid.Data3,
            guid.Data4[0], guid.Data4[1],
            guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5],
            guid.Data4[6], guid.Data4[7]);
        uniqueId = buf;
    }

    if (uniqueId.empty()) {
        auto duration_since_epoch = std::chrono::system_clock::now().time_since_epoch();
        auto microseconds_since_epoch = std::chrono::duration_cast<std::chrono::microseconds>(duration_since_epoch).count();
        uniqueId = std::string((isPipeServer ? "[pipe server]-" : "[pipe client]-")) + std::to_string(microseconds_since_epoch);
    }

    if (SUCCEEDED(ret)) {
        ::CoUninitialize();
    }

    return uniqueId;
}
