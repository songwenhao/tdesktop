#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <future>

#include "PipeWrapper.h"

#ifdef WINDOWS_PLATFORM
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #include <Windows.h>
#endif

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
        const std::wstring& pipeName,
        PipeType pipeType
    )
        : isPipeServer_(pipeType == PipeType::PipeServer),
        pipeName_(pipeName),
        pipeHandle_(INVALID_HANDLE_VALUE),
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

        pipeCmdResultMapMutex_ = std::make_unique<std::mutex>();

        pid_ = GetCurrentProcessId();

        memset(&pipeReadOverlapped_, 0, sizeof(pipeReadOverlapped_));
        pipeReadOverlapped_.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        memset(&pipeWriteOverlapped_, 0, sizeof(pipeWriteOverlapped_));
        pipeWriteOverlapped_.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    ~PipeWrapperImpl() {
        disConnectPipe();
    }

    void pipeThd(
        const std::function<bool()>& checkStopConnect,
        std::promise<bool> &promiseObj,
        std::uint64_t maxWaitTime
    ) {
        bool connected = false;
        pipeHandle_ = INVALID_HANDLE_VALUE;

        std::wstring logPrevStr = std::wstring(L"[") + __FUNCTIONW__ + L"] connect pipe";

        printLogW(L"%s begin ...", logPrevStr.c_str());

        HANDLE pipeHandle = INVALID_HANDLE_VALUE;

        DWORD errCode = 0;

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
                    pipeName_.c_str(),
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
                    printLogW(L"%s CreateNamedPipe error, error code: %d", logPrevStr.c_str(), errCode);
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
                        if (checkStopConnect && checkStopConnect()) {
                            break;
                        }

                        waitCode = WaitForSingleObject(overlapped.hEvent, 1000);
                        if (waitCode == WAIT_TIMEOUT) {
                            if (checkStop()) {
                                printLogW(L"%s stop!!!", logPrevStr.c_str());
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
                    printLogW(L"%s ConnectNamedPipe error, error code: %d", logPrevStr.c_str(), errCode);
                }
            } else {
                ULONGLONG beginTime = GetTickCount64();

                while (true) {
                    if (checkStopConnect && checkStopConnect()) {
                        break;
                    }

                    // Try to open a named pipe; wait for it, if necessary.
                    pipeHandle = CreateFileW(
                        pipeName_.c_str(),
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
                            if (checkStopConnect && checkStopConnect()) {
                                break;
                            }

                            if (WaitNamedPipeW(pipeName_.c_str(), 1000)) {
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

        if (overlapped.hEvent) {
            CloseHandle(overlapped.hEvent);
        }

        promiseObj.set_value(connected);

        printLogW(L"%s end ...", logPrevStr.c_str());

        if (pipeHandle_ != INVALID_HANDLE_VALUE) {
            bool ret = false;

            PipeCmd::Cmd cmd;

            while (true) {
                if (checkStop()) {
                    break;
                }

                ret = recvCmd(cmd);
                if (!ret) {
                    continue;
                }

                if (onRecvPipeCmd_) {
                    onRecvPipeCmd_(ctx_, cmd);
                }
            }
        }
    }

    bool connectPipe(
        const std::function<bool()>& checkStop,
        std::uint64_t maxWaitTime
    ) {
        bool connected = false;

        do {
            if (!pipeReadOverlapped_.hEvent ||
                !pipeWriteOverlapped_.hEvent) {
                break;
            }

            ResetEvent(pipeReadOverlapped_.hEvent);
            ResetEvent(pipeWriteOverlapped_.hEvent);

            std::promise<bool> promiseObj;

            pipeReadThd_ = new std::thread(
                std::bind(
                    &PipeWrapperImpl::pipeThd, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3
                ), checkStop, std::ref(promiseObj), maxWaitTime
            );

            connected = promiseObj.get_future().get();

        } while (false);

        return connected;
    }

    void disConnectPipe() {
        setStop();

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

        if (pipeReadThd_) {
            if (pipeReadThd_->joinable()) {
                pipeReadThd_->join();
            }
            delete pipeReadThd_;
            pipeReadThd_ = nullptr;
        }
    }

    std::uint32_t pipeRead(
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
                        while (!checkStop()) {
                            ret = WaitForSingleObject(pipeReadOverlapped_.hEvent, 1000);
                            if (ret == WAIT_OBJECT_0) {
                                readSize = 0;
                                ret = GetOverlappedResult(pipeHandle_, &pipeReadOverlapped_, &readSize, FALSE);
                                ok = true;
                                break;
                            }

                            if (checkStop()) {
                                printLogW(L"[%s] stopFlag is true!!!", funcName);
                                break;
                            }
                        }
                    }
                } else {
                    printLogW(L"[%s] pipe read failed, GetLastError=%d", funcName, GetLastError());
                }
            }

        } while (false);

        if (!ok) {
            readSize = 0;
            setStop();
            printLogW(L"[%s] Pipe read failed, stop!", funcName);
        }

        return (std::uint32_t)readSize;
    }

    std::uint32_t pipeWrite(
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

                        if (checkStop()) {
                            printLogW(L"[%s] stopFlag is true!!!", funcName);
                            break;
                        }
                    }
                } else {
                    printLogW(L"[%s] pipe write failed, GetLastError=%d", funcName, GetLastError());
                }
            }

        } while (false);

        if (!ok) {
            writeSize = 0;
            setStop();
            printLogW(L"[%s] Pipe write failed, stop!", funcName);
        }

        return (std::uint32_t)writeSize;
    }

    bool recvCmd(PipeCmd::Cmd& cmd) {
        const char* funcName = __FUNCTION__;

        bool ok = false;

        std::uint32_t dataSize = 0;
        char* data = nullptr;

        do {
            cmd.Clear();

            // 接收命令字节数
            std::uint32_t readSize = pipeRead((char*)&dataSize, sizeof(std::uint32_t));
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

            readSize = pipeRead(data, dataSize);
            if (!readSize) {
                break;
            }

            ok = PipeCmd::BlobDataToCmd(cmd, data, dataSize);
            if (!ok) {
                break;
            }

            printLogA("[%s] <== recv cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(), cmd.action, std::uint32_t(cmd.content.size()));

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

    PipeCmd::Cmd sendCmd(
        const PipeCmd::Cmd& cmd,
        bool waitDone,
        std::uint32_t maxWaitTime,
        const OnRecvPipeCmd& sendCmdCallback,
        void* ctx
    ) {
        const char* funcName = __FUNCTION__;

        PipeCmd::Cmd resultCmd;

        char* buf = nullptr;

        HANDLE signalEvent = nullptr;

        do {
            if (checkStop()) {
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

            std::uint32_t writeSize = pipeWrite((const char*)&dataSize, sizeof(std::uint32_t));
            if (!writeSize) {
                break;
            }

            writeSize = pipeWrite(buf, dataSize);
            if (!writeSize) {
                break;
            }

            printLogA("[%s] ==> send cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(), cmd.action, std::uint32_t(cmd.content.size()));

            // 同步命令等待结果
            if (waitDone && cmd.action > -1) {
                DWORD waitCode = -1;
                DWORD waitTime = 0;

                if (signalEvent) {
                    while (!checkStop() && maxWaitTime > 0) {
                        waitCode = WaitForSingleObject(signalEvent, 1000);
                        if (waitCode != WAIT_TIMEOUT) {
                            break;
                        }

                        waitTime += 1000;
                        if (waitTime >= maxWaitTime) {
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

    void registerCallback(
        void* ctx,
        const OnRecvPipeCmd& onRecvPipeCmd,
        const OnCheckStop& onCheckStop,
        const OnStop& onStop
    ) {
        ctx_ = ctx;
        onRecvPipeCmd_ = onRecvPipeCmd;
        onCheckStop_ = onCheckStop;
        onStop_ = onStop;
    }

    bool checkStop() {
        bool stop = false;

        do {
            if (stopFlag_) {
                stop = true;
                break;
            }

            if (onCheckStop_ && onCheckStop_(ctx_)) {
                stop = true;
                setStop();
                break;
            }

        } while (false);

        return stop;
    }

    void setStop() {
        if (!stopFlag_) {
            stopFlag_ = true;

            if (onStop_) {
                onStop_(ctx_);
            }
        }

    }

    void printLogA(const char* format, ...) {
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

    void printLogW(const wchar_t* format, ...) {
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
    std::wstring pipeName_;
    HANDLE pipeHandle_;
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

    std::uint32_t pid_;
};

PipeWrapper::PipeWrapper(
    const std::wstring& pipeName,
    PipeType pipeType
) : impl_(new PipeWrapperImpl(pipeName, pipeType)) {}

PipeWrapper::~PipeWrapper() {
    if (impl_) {
        delete impl_;
    }
}

bool PipeWrapper::connectPipe(
    const std::function<bool()>& checkStop,
    std::uint64_t maxWaitTime
) {
    return impl_->connectPipe(checkStop, maxWaitTime);
}

void PipeWrapper::disConnectPipe() {
    return impl_->disConnectPipe();
}

void PipeWrapper::registerCallback(
    void* ctx,
    const OnRecvPipeCmd& onRecvPipeCmd,
    const OnCheckStop& onCheckStop,
    const OnStop& onStop
) {
    impl_->registerCallback(ctx, onRecvPipeCmd, onCheckStop, onStop);
}

PipeCmd::Cmd PipeWrapper::sendCmd(
    const PipeCmd::Cmd& cmd,
    bool waitDone,
    std::uint32_t maxWaitTime,
    const OnRecvPipeCmd& sendCmdCallback,
    void* ctx
) {
    return impl_->sendCmd(cmd, waitDone, maxWaitTime, sendCmdCallback, ctx);
}