#include "LogTrace.h"

#include "PipeWrapper.h"

class PipeWrapper::PipeWrapperImpl {
public:
PipeWrapperImpl(
    const std::wstring& wstrPipeName,
    const std::wstring& wstrHeartbeatEventName,
    PipeType pipeType
)
    : isPipeServer_(pipeType == PipeType::PipeServer),
    readPipeHandle_(INVALID_HANDLE_VALUE),
    writePipeHandle_(INVALID_HANDLE_VALUE),
    pipeName_(wstrPipeName),
    heartbeatEventName_(wstrHeartbeatEventName),
    heartbeatThd_(nullptr),
    pipeReadThd_(nullptr),
    pipeWriteThd_(nullptr),
    stopFlag_(false),
    ctx_(nullptr),
    handleRecvCmdFunc_(nullptr),
    checkStop_(nullptr)
{
    logBufA_ = std::make_unique<char[]>(logBufSize_);
    logBufW_ = std::make_unique<wchar_t[]>(logBufSize_);

    pid_ = GetCurrentProcessId();

    logPrevStrA_ = (isPipeServer_ ? "[pipe server]" : "[pipe client]");
    logPrevStrW_ = (isPipeServer_ ? L"[pipe server]" : L"[pipe client]");

    s2c_heartbeatEventHandle_ = CreateEventW(NULL, FALSE, FALSE, (heartbeatEventName_ + L"-s2c").c_str());
    c2s_heartbeatEventHandle_ = CreateEventW(NULL, FALSE, FALSE, (heartbeatEventName_ + L"-c2s").c_str());

    memset(&pipeReadOverlapped_, 0, sizeof(pipeReadOverlapped_));
    pipeReadOverlapped_.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    memset(&pipeWriteOverlapped_, 0, sizeof(pipeWriteOverlapped_));
    pipeWriteOverlapped_.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    readPipeConnectedEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    writePipeConnectedEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
}

~PipeWrapperImpl() {
    DisConnectPipe();
}

static void PipeThd(
    PipeWrapperImpl* obj,
    bool isReadPipe,
    ULONGLONG maxWaitTime
) {
    bool isSuccess = false;
    bool isReadPipeHandle = false;
    std::wstring logPrevStr;

    if (isReadPipe) {
        logPrevStr = std::wstring(L"[") + __FUNCTIONW__ + L"] Connect read pipe";
    } else {
        logPrevStr = std::wstring(L"[") + __FUNCTIONW__ + L"] Connect write pipe";
    }

    obj->LogW(L"%s begin ...", logPrevStr.c_str());

    HANDLE pipeHandle = INVALID_HANDLE_VALUE;

    /*
    * | server |           | client |
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
        if (obj->isPipeServer_) {
            obj->readPipeHandle_ = INVALID_HANDLE_VALUE;
        } else {
            obj->writePipeHandle_ = INVALID_HANDLE_VALUE;
        }
    } else {
        if (obj->isPipeServer_) {
            obj->writePipeHandle_ = INVALID_HANDLE_VALUE;
        } else {
            obj->readPipeHandle_ = INVALID_HANDLE_VALUE;
        }
    }

    DWORD errCode = 0;

    std::wstring pipeName = obj->pipeName_;
    if (isReadPipe) {
        pipeName += L"-read";
    } else {
        pipeName += L"-write";
    }

    OVERLAPPED overlapped;
    DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

    do {
        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!overlapped.hEvent) {
            break;
        }

        if (obj->isPipeServer_) {
            pipeHandle = CreateNamedPipeW(
                pipeName.c_str(),
                PIPE_ACCESS_DUPLEX |
                FILE_FLAG_OVERLAPPED,
                pipeMode,
                1,
                obj->pipeBufSize_,
                obj->pipeBufSize_,
                0,
                NULL);

            if (pipeHandle == INVALID_HANDLE_VALUE) {
                errCode = GetLastError();
                obj->LogW(L"%s CreateNamedPipe error, error code: %d", logPrevStr.c_str(), errCode);
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
                        if (obj->stopFlag_ || (obj->checkStop_ && obj->checkStop_(obj->ctx_))) {
                            obj->LogW(L"%s stop connect pipe!!!", logPrevStr.c_str());
                            break;
                        }

                        if (GetTickCount64() - beginTime >= maxWaitTime) {
                            break;
                        }

                        continue;
                    } else if (waitCode == WAIT_OBJECT_0) {
                        isSuccess = true;
                        break;
                    } else {
                        break;
                    }
                }
            } else if (errCode == ERROR_PIPE_CONNECTED) {
                isSuccess = true;
            } else {
                obj->LogW(L"%s ConnectNamedPipe error, error code: %d", logPrevStr.c_str(), errCode);
            }
        } else {
            ULONGLONG beginTime = GetTickCount64();

            while (true) {
                // Try to open a named pipe; wait for it, if necessary.
                pipeHandle = CreateFileW(
                    pipeName.c_str(),
                    GENERIC_READ |
                    GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    NULL);

                errCode = GetLastError();

                if (pipeHandle != INVALID_HANDLE_VALUE && errCode == ERROR_SUCCESS) {
                    isSuccess = true;
                } else if (pipeHandle != INVALID_HANDLE_VALUE && errCode == ERROR_PIPE_BUSY) {
                    // All pipe instances are busy, so wait. 
                    while (true) {
                        if (WaitNamedPipeW(pipeName.c_str(), 1000)) {
                            isSuccess = true;
                            break;
                        } else {
                            if (GetTickCount64() - beginTime >= maxWaitTime) {
                                break;
                            }
                        }
                    }
                }

                if (isSuccess) {
                    isSuccess = false;

                    if (SetNamedPipeHandleState(pipeHandle, &pipeMode, NULL, NULL)) {
                        isSuccess = true;
                    }
                }

                if (isSuccess) {
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

    if (!isSuccess) {
        if (pipeHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(pipeHandle);
            pipeHandle = INVALID_HANDLE_VALUE;
        }
    }

    if (isReadPipe) {
        if (obj->isPipeServer_) {
            obj->readPipeHandle_ = pipeHandle;
            isReadPipeHandle = true;
        } else {
            obj->writePipeHandle_ = pipeHandle;
        }

        if (obj->readPipeConnectedEvent_) {
            SetEvent(obj->readPipeConnectedEvent_);
        }
    } else {
        if (obj->isPipeServer_) {
            obj->writePipeHandle_ = pipeHandle;
        } else {
            obj->readPipeHandle_ = pipeHandle;
            isReadPipeHandle = true;
        }

        if (obj->writePipeConnectedEvent_) {
            SetEvent(obj->writePipeConnectedEvent_);
        }
    }

    if (overlapped.hEvent) {
        CloseHandle(overlapped.hEvent);
    }

    obj->LogW(L"%s end ...", logPrevStr.c_str());

    if (isReadPipeHandle && obj->readPipeConnectedEvent_ != INVALID_HANDLE_VALUE) {
        bool ret = false;
        PipeCmd::Cmd cmd;

        while (true) {
            if (obj->stopFlag_ || (obj->checkStop_ && obj->checkStop_(obj->ctx_))) {
                break;
            }

            ret = obj->RecvCmd(cmd);
            if (!ret) {
                continue;
            }

            if (obj->handleRecvCmdFunc_) {
                obj->handleRecvCmdFunc_(obj->ctx_, cmd);
            }
        }
    }
}

bool ConnectPipe(ULONGLONG maxWaitTime) {
    bool connected = false;

    do {
        if (!s2c_heartbeatEventHandle_ ||
            !c2s_heartbeatEventHandle_ ||
            !pipeReadOverlapped_.hEvent ||
            !pipeWriteOverlapped_.hEvent ||
            !readPipeConnectedEvent_ ||
            !writePipeConnectedEvent_) {
            break;
        }

        ResetEvent(s2c_heartbeatEventHandle_);
        ResetEvent(c2s_heartbeatEventHandle_);
        ResetEvent(pipeReadOverlapped_.hEvent);
        ResetEvent(pipeWriteOverlapped_.hEvent);
        ResetEvent(readPipeConnectedEvent_);
        ResetEvent(writePipeConnectedEvent_);

        pipeReadThd_ = new std::thread(PipeThd, this, true, maxWaitTime);

        pipeWriteThd_ = new std::thread(PipeThd, this, false, maxWaitTime);

        DWORD waitCode = -1;
        HANDLE eventHandles[2] = { readPipeConnectedEvent_, writePipeConnectedEvent_ };

        while (!stopFlag_) {
            if (checkStop_ && checkStop_(ctx_)) {
                stopFlag_ = true;
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

    if (connected) {
        StartHeartbeatThd();
    }

    return connected;
}

void DisConnectPipe() {
    stopFlag_ = true;

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
        s2c_heartbeatEventHandle_ = NULL;
    }

    if (c2s_heartbeatEventHandle_) {
        CloseHandle(c2s_heartbeatEventHandle_);
        c2s_heartbeatEventHandle_ = NULL;
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
        pipeReadOverlapped_.hEvent = NULL;
    }

    if (pipeWriteOverlapped_.hEvent) {
        CloseHandle(pipeWriteOverlapped_.hEvent);
        pipeWriteOverlapped_.hEvent = NULL;
    }

    if (readPipeConnectedEvent_) {
        CloseHandle(readPipeConnectedEvent_);
        readPipeConnectedEvent_ = NULL;
    }

    if (writePipeConnectedEvent_) {
        CloseHandle(writePipeConnectedEvent_);
        writePipeConnectedEvent_ = NULL;
    }
}

bool StartHeartbeatThd() {
    bool ret = false;

    heartbeatThd_ = new std::thread([this]() {
        int waitTime = 0;
        const int maxWaitTime = 5;
        DWORD waitCode = -1;
        while (true) {
            if (stopFlag_) {
                Quit();
                break;
            }

            if (checkStop_ && checkStop_(ctx_)) {
                Quit();
                break;
            }

            if (isPipeServer_) {
                //LogW(L"[s to c] set heartbeat event ...");

                SetEvent(s2c_heartbeatEventHandle_);

                waitCode = WaitForSingleObject(c2s_heartbeatEventHandle_, 1000);
                if (waitCode != WAIT_OBJECT_0) {
#ifndef _DEBUG
                    ++waitTime;
                    if (waitTime >= maxWaitTime) {
                        LogW(L" wait for heartbeat event timeout %d seconds, exit ...", waitTime);
                        stopFlag_ = true;
                        break;
                    }
#endif
            } else {
                    waitTime = 0;
                }
            } else {
                //LogW(L"[c to s] set heartbeat event ...");

                SetEvent(c2s_heartbeatEventHandle_);

                waitCode = WaitForSingleObject(s2c_heartbeatEventHandle_, 1000);
                if (waitCode != WAIT_OBJECT_0) {
#ifndef _DEBUG
                    ++waitTime;
                    if (waitTime >= maxWaitTime) {
                        LogW(L" wait for heartbeat event timeout %d seconds, exit ...", waitTime);
                        stopFlag_ = true;
                        break;
                    }
#endif
                } else {
                    waitTime = 0;
                }
            }
        }
    });

    if (heartbeatThd_ && heartbeatThd_->native_handle()) {
        ret = true;
    }

    return ret;
}

bool PipeRead(
    unsigned char*& data,
    std::uint32_t& dataSize
) {
    const wchar_t* funcName = __FUNCTIONW__;

    bool isSuccess = false;

    DWORD readSize = 0;

    do {
        data = new unsigned char[dataSize + 2];
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
            isSuccess = true;
        } else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_IO_PENDING) {
                if (pipeReadOverlapped_.hEvent) {
                    while (true) {
                        ret = WaitForSingleObject(pipeReadOverlapped_.hEvent, 1000);
                        if (ret == WAIT_OBJECT_0) {
                            readSize = 0;
                            ret = GetOverlappedResult(readPipeHandle_, &pipeReadOverlapped_, &readSize, FALSE);
                            isSuccess = true;
                            break;
                        }

                        if (stopFlag_ || (checkStop_ && checkStop_(ctx_))) {
                            LogW(L"[%s] stopFlag is true!!!", funcName);
                            break;
                        }
                    }
                }
            } else {
                LogW(L"[%s] pipe read failed, GetLastError=%d", funcName, GetLastError());
                break;
            }
        }

        if (readSize < dataSize) {
            dataSize = readSize;
        }

        data[dataSize] = '\0';
        data[dataSize + 1] = '\0';

    } while (false);

    if (!isSuccess) {
        if (data) {
            delete[] data;
            data = nullptr;
        }

        dataSize = 0;
        stopFlag_ = true;
        LogW(L"[%s] Pipe read failed, stop!", funcName);
    }

    return isSuccess;
}

bool PipeWrite(
    const unsigned char* data,
    std::uint32_t dataSize
) {
    const wchar_t* funcName = __FUNCTIONW__;

    bool isSuccess = false;
    DWORD writeSize = 0;

    do {
        pipeWriteOverlapped_.Offset = 0;
        if (pipeWriteOverlapped_.hEvent) {
            ResetEvent(pipeWriteOverlapped_.hEvent);
        }

        DWORD writeBytes = 0;

        BOOL ret = WriteFile(
            writePipeHandle_,
            data,
            dataSize,
            &writeSize,
            &pipeWriteOverlapped_);

        if (ret) {
            isSuccess = true;
        } else {
            DWORD errorCode = GetLastError();
            if (errorCode == ERROR_IO_PENDING) {
                while (true) {
                    if (pipeWriteOverlapped_.hEvent) {
                        ret = WaitForSingleObject(pipeWriteOverlapped_.hEvent, 1000);
                        if (ret == WAIT_OBJECT_0) {
                            writeSize = 0;
                            ret = GetOverlappedResult(writePipeHandle_, &pipeWriteOverlapped_, &writeSize, FALSE);
                            isSuccess = true;
                            break;
                        }
                    }

                    if (stopFlag_ || (checkStop_ && checkStop_(ctx_))) {
                        LogW(L"[%s] stopFlag is true!!!", funcName);
                        break;
                    }
                }
            } else {
                LogW(L"[%s] pipe write failed, GetLastError=%d", funcName, GetLastError());
                break;
            }
        }

    } while (false);

    if (!isSuccess) {
        stopFlag_ = true;
        LogW(L"[%s] Pipe write failed, stop!", funcName);
    }

    return isSuccess;
}

bool RecvCmd(PipeCmd::Cmd& cmd) {
    const char* funcName = __FUNCTION__;

    bool isSuccess = false;

    std::uint32_t dataSize = 0;
    unsigned char* data = nullptr;

    do {
        cmd.Clear();

        // 接收命令字节数
        dataSize = sizeof(std::uint32_t);
        isSuccess = PipeRead(data, dataSize);
        if (!isSuccess || !data || dataSize == 0) {
            isSuccess = false;
            break;
        }

        // 接收命令
        dataSize = *(std::uint32_t*)data;
        delete[] data;
        data = nullptr;
        isSuccess = PipeRead(data, dataSize);
        if (!isSuccess || !data || dataSize == 0) {
            isSuccess = false;
            break;
        }

        isSuccess = ParsePipeCmd(cmd, data, dataSize);
        if (!isSuccess) {
            break;
        }

        LogA("[%s] <== recv cmd, seq: %lld action: %d content: %s", funcName, cmd.seq_number(), cmd.action(), cmd.content().c_str());

        std::string key = (isPipeServer_ ? ("[pipe client]-" + std::to_string(cmd.seq_number())) : ("[pipe server]-" + std::to_string(cmd.seq_number())));

        OnRecvPipeCmd resultCallback = nullptr;
        void* ctx = nullptr;
        HANDLE signalEvent = nullptr;

        {
            // 设置命令执行结果，取出命令注册的信息
            std::lock_guard<std::mutex> locker(pipeCmdResultMapMutex_);
            auto iter = pipeCmdResultMap_.find(key);
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
            std::lock_guard<std::mutex> locker(pipeCmdResultMapMutex_);
            auto iter = pipeCmdResultMap_.find(key);
            if (iter != pipeCmdResultMap_.end()) {
                pipeCmdResultMap_.erase(iter);
            }
        }

    } while (false);

    if (data) {
        delete[] data;
    }

    return isSuccess;
}

void Quit() {
    if (stopCallback_) {
        stopCallback_(ctx_);
    }

    PipeCmd::Cmd cmd;
    cmd.set_action(-1);

    if (handleRecvCmdFunc_) {
        handleRecvCmdFunc_(ctx_, cmd);
    }
}

PipeCmd::Cmd SendCmd(
    const PipeCmd::Cmd& cmd,
    bool waitDone,
    DWORD waitTime,
    const OnRecvPipeCmd& sendCmdCallback,
    void* ctx
) {
    const char* funcName = __FUNCTION__;

    PipeCmd::Cmd resultCmd;

    HANDLE signalEvent = NULL;

    do {
        if (stopFlag_) {
            break;
        }

        PipeCmd::Cmd copyCmd = cmd;
        if (copyCmd.seq_number() == 0) {
            copyCmd.set_seq_number(cmdSeq_);
            ++cmdSeq_;
        }

        std::string key = (isPipeServer_ ? ("[pipe client]-" + std::to_string(copyCmd.seq_number())) : ("[pipe server]-" + std::to_string(copyCmd.seq_number())));

        // 同步命令设置事件
        if (waitDone && copyCmd.action() > -1) {
            signalEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        }

        {
            // 注册命令
            std::lock_guard<std::mutex> locker(pipeCmdResultMapMutex_);
            pipeCmdResultMap_.insert({ key, PipeCmdResult(sendCmdCallback, ctx, signalEvent) });
        }

        std::vector<unsigned char> buf;
        buf.resize(copyCmd.ByteSizeLong());
        if (!copyCmd.SerializeToArray(buf.data(), (int)buf.size())) {
            break;
        }

        std::uint32_t dataSize = (std::uint32_t)buf.size();
        bool isSuccess = PipeWrite((const unsigned char*)&dataSize, sizeof(std::uint32_t));
        if (!isSuccess) {
            break;
        }

        isSuccess = PipeWrite(buf.data(), dataSize);
        if (!isSuccess) {
            break;
        }

        LogA("[%s] ==> send cmd, seq: %lld action: %d content: %s", funcName, copyCmd.seq_number(), copyCmd.action(), copyCmd.content().c_str());

        // 同步命令等待结果
        if (waitDone && copyCmd.action() > -1) {
            DWORD waitCode = -1;

            if (signalEvent) {
                while (!stopFlag_ && waitTime > 0) {
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
                std::lock_guard<std::mutex> locker(pipeCmdResultMapMutex_);
                auto iter = pipeCmdResultMap_.find(key);
                if (iter != pipeCmdResultMap_.end()) {
                    resultCmd = iter->second.result;
                    pipeCmdResultMap_.erase(iter);
                }
            }
        }
    } while (false);

    if (signalEvent) {
        CloseHandle(signalEvent);
        signalEvent = NULL;
    }

    return resultCmd;
}

void RegisterCallback(
    void* ctx,
    const OnRecvPipeCmd& recvCmdCallback,
    const OnCheckStop& checkStopCallback,
    const OnStop& stopCallback
) {
    ctx_ = ctx;
    handleRecvCmdFunc_ = recvCmdCallback;
    checkStop_ = checkStopCallback;
    stopCallback_ = stopCallback;
}

void LogA(const char* format, ...) {
#ifndef _DEBUG
    return;
#endif

    std::lock_guard<std::mutex> locker(logBufMutex_);

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
    
    std::lock_guard<std::mutex> locker(logBufMutex_);

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
    OnRecvPipeCmd handleRecvCmdFunc_;
    OnCheckStop checkStop_;
    OnStop stopCallback_;

    OVERLAPPED pipeReadOverlapped_;
    OVERLAPPED pipeWriteOverlapped_;
    const std::uint32_t pipeBufSize_ = 4096;

    std::mutex logBufMutex_;
    std::string logPrevStrA_;
    std::wstring logPrevStrW_;
    std::unique_ptr<char[]> logBufA_;
    std::unique_ptr<wchar_t[]> logBufW_;
    const std::uint32_t logBufSize_ = 1024 * 1024;

    HANDLE readPipeConnectedEvent_;
    HANDLE writePipeConnectedEvent_;

    std::int64_t cmdSeq_ = 1;
    std::mutex pipeCmdResultMapMutex_;
    std::map<std::string, PipeCmdResult> pipeCmdResultMap_;

    DWORD pid_ = 0;
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

bool PipeWrapper::ConnectPipe(ULONGLONG maxWaitTime) {
    return pimpl_->ConnectPipe(maxWaitTime);
}

void PipeWrapper::DisConnectPipe() {
    return pimpl_->DisConnectPipe();
}

PipeCmd::Cmd PipeWrapper::SendCmd(
    const PipeCmd::Cmd& cmd,
    bool waitDone,
    DWORD waitTime,
    const OnRecvPipeCmd& sendCmdCallback,
    void* ctx
) {
    return pimpl_->SendCmd(cmd, waitDone, waitTime, sendCmdCallback, ctx);
}

void PipeWrapper::RegisterCallback(
    void* ctx,
    const OnRecvPipeCmd& recvCmdCallback,
    const OnCheckStop& checkStopCallback,
    const OnStop& stopCallback
) {
    pimpl_->RegisterCallback(ctx, recvCmdCallback, checkStopCallback, stopCallback);
}

bool PipeWrapper::ParsePipeCmd(
    PipeCmd::Cmd& cmd,
    const unsigned char* data,
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

std::string PipeWrapper::Utf16ToUtf8(const std::wstring& str) {
    std::string dst;

    do {
        int utf8Size = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, NULL, 0, NULL, NULL);
        if (utf8Size == 0) {
            break;
        }

        std::vector<char> resultVec(utf8Size);
        int retSize = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, &resultVec[0], utf8Size, NULL, NULL);
        if (retSize != utf8Size) {
            break;
        }

        dst = std::string(&resultVec[0]);

    } while (false);

    return dst;
}

std::wstring PipeWrapper::Utf8ToUtf16(const std::string& str) {
    std::wstring dst;

    do {
        int wideSize = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
        if (ERROR_NO_UNICODE_TRANSLATION == wideSize || wideSize == 0) {
            break;
        }

        std::vector<wchar_t> resultVec(wideSize);
        int retSize = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &resultVec[0], wideSize);
        if (retSize != wideSize) {
            break;
        }

        dst = std::wstring(&resultVec[0]);
    } while (false);

    return dst;
}