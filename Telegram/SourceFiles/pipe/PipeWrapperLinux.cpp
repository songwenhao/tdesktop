#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <semaphore.h>
#include <cstdarg>
#include <cstring>
#include <future>

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
            sem_t* signalEvent
        ) {
            this->resultCallback = resultCallback;
            this->ctx = ctx;
            this->signalEvent = signalEvent;
        }

        PipeCmd::Cmd result;
        OnRecvPipeCmd resultCallback;
        void* ctx;
        sem_t* signalEvent;
    };

public:
    PipeWrapperImpl(
        const std::string& pipeName,
        PipeType pipeType
    ) : isPipeServer_(pipeType == PipeType::PipeServer),
        readPipeHandle_(-1),
        writePipeHandle_(-1),
        pipeName_(pipeName),
        pipeReadThd_(nullptr),
        stopFlag_(false),
        ctx_(nullptr),
        onRecvPipeCmd_(nullptr),
        onCheckStop_(nullptr),
        onStop_(nullptr),
        logBufMutex_(nullptr),
        logBuf_(nullptr),
        pipeConnectedEvent_(nullptr),
        pipeCmdResultMapMutex_(nullptr),
        pid_(0) {
        readPipeName_ = pipeName_ + "-read";
        writePipeName_ = pipeName_ + "-write";

        logBufMutex_ = std::make_unique<std::mutex>();
        logBuf_ = std::make_unique<char[]>(logBufSize_);
        logPrefixStr_ = (isPipeServer_ ? "[pipe server]" : "[pipe client]");

        pipeConnectedEvent_ = new sem_t();
        sem_init(pipeConnectedEvent_, 0, 1);

        pipeCmdResultMapMutex_ = std::make_unique<std::mutex>();

        pid_ = syscall(SYS_getpid);
    }

    ~PipeWrapperImpl() {
        disConnectPipe();
    }

    void pipeThd(
        const std::function<bool()>& checkStopConnect,
        std::promise<bool>& promiseObj,
        std::uint64_t maxWaitTime
    ) {
        bool connected = false;
        readPipeHandle_ = -1;
        writePipeHandle_ = -1;

        std::string logPrefixStr = std::string("[") + __FUNCTION__ + "] connect pipe";

        printLog("%s begin ...", logPrefixStr.c_str());

        do {
            if (isPipeServer_) {
                unlink(readPipeName_.c_str());
                unlink(writePipeName_.c_str());

                int res = mkfifo(
                    readPipeName_.c_str(),
                    0666
                );
                if (res == -1) {
                    printLog(
                        "%s mkfifo pipe: %s error, error: %s(errno: %d)",
                        logPrefixStr.c_str(),
                        readPipeName_.c_str(),
                        strerror(errno),
                        errno
                    );
                    break;
                }

                readPipeHandle_ = open(
                    readPipeName_.c_str(),
                    O_RDONLY | O_NONBLOCK,
                    0666
                );

                if (readPipeHandle_ == -1) {
                    printLog(
                        "%s open read name pipe: %s error, error: %s(errno: %d)",
                        logPrefixStr.c_str(),
                        readPipeName_.c_str(),
                        strerror(errno),
                        errno
                    );
                    break;
                }

                res = mkfifo(
                    writePipeName_.c_str(),
                    0666
                );
                if (res == -1) {
                    printLog(
                        "%s mkfifo pipe: %s error, error: %s(errno: %d)",
                        logPrefixStr.c_str(),
                        writePipeName_.c_str(),
                        strerror(errno),
                        errno
                    );
                    break;
                }

                connected = true;

            } else {
                std::uint64_t waitTime = 0;
                while (true) {
                    if (checkStopConnect && checkStopConnect()) {
                        break;
                    }

                    if (readPipeHandle_ == -1) {
                        readPipeHandle_ = open(
                            writePipeName_.c_str(),
                            O_RDONLY | O_NONBLOCK,
                            0666
                        );
                        if (readPipeHandle_ == -1) {
                            printLog("%s open read name pipe: %s error, error: "
                                     "%s(errno: %d)",
                                     logPrefixStr.c_str(), writePipeName_.c_str(),
                                     strerror(errno), errno);
                        }
                    }

                    if (writePipeHandle_ == -1) {
                        writePipeHandle_ = open(
                            readPipeName_.c_str(),
                            O_WRONLY | O_NONBLOCK,
                            0666
                        );

                        if (writePipeHandle_ == -1) {
                            printLog("%s open write name pipe: %s error, error: "
                                     "%s(errno: %d)",
                                     logPrefixStr.c_str(), readPipeName_.c_str(),
                                     strerror(errno), errno);
                        }
                    }

                    if (readPipeHandle_ != -1 && writePipeHandle_ != -1) {
                        connected = true;
                        break;
                    }

                    waitTime += 1000;
                    if (waitTime > maxWaitTime) {
                        break;
                    }

                    sleep(1);
                }
            }

        } while (false);

        promiseObj.set_value(connected);

        printLog("%s end ...", logPrefixStr.c_str());

        if (connected) {
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
        std::promise<bool> promiseObj;

        pipeReadThd_ = new std::thread(
            std::bind(
                &PipeWrapperImpl::pipeThd, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3
            ), checkStop, std::ref(promiseObj), maxWaitTime
        );

        connected = promiseObj.get_future().get();

        return connected;
    }

    void disConnectPipe() {
        setStop();

        // first close write pipe, notify the other end read thread exit
        if (writePipeHandle_ != -1) {
            close(writePipeHandle_);
            writePipeHandle_ = -1;
            unlink(writePipeName_.c_str());
        }

        if (readPipeHandle_ != -1) {
            close(readPipeHandle_);
            readPipeHandle_ = -1;
            unlink(readPipeName_.c_str());
        }

        if (pipeConnectedEvent_) {
            sem_close(pipeConnectedEvent_);
            delete pipeConnectedEvent_;
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
        const char* funcName = __FUNCTION__;

        bool ok = false;

        std::uint32_t totalReadSize = 0;
        ssize_t readSize = 0;

        do {
            if (!data) {
                break;
            }

            fd_set fds;
            timeval timeout = {0, 0};
            int ret;

            while (!checkStop()) {
                timeout.tv_sec = 3;
                timeout.tv_usec = 0;
                FD_ZERO(&fds);
                FD_SET(readPipeHandle_, &fds);
                ret = select(readPipeHandle_ + 1, &fds, nullptr, nullptr, &timeout);
                if (ret == -1) {
                    printLog("[%s] select read pipe failed, error: %s(errno: %d)", funcName, strerror(errno), errno);
                    break;
                } else if (ret) {
                    if (FD_ISSET(readPipeHandle_, &fds)) {
                        readSize = read(readPipeHandle_, data + totalReadSize, dataSize - totalReadSize);
                        if (readSize <= 0 && errno != EINTR) {
                            break;
                        }

                        totalReadSize += readSize;
                        if (totalReadSize >= dataSize) {
                            break;
                        }
                    }
                }
            }

            if (totalReadSize >= dataSize) {
                ok = true;
            } else {
                printLog("[%s] pipe read failed, error: %s(errno: %d)", funcName, strerror(errno), errno);
            }

        } while (false);

        if (!ok) {
            totalReadSize = 0;
            setStop();
            printLog("[%s] Pipe read failed, stop!", funcName);
        }

        return totalReadSize;
    }

    std::uint32_t pipeWrite(
        const char* data,
        std::uint32_t dataSize
    ) {
        const char* funcName = __FUNCTION__;

        bool ok = false;
        std::uint32_t totalWriteSize = 0;
        ssize_t writeSize = 0;

        do {
            if (writePipeHandle_ == -1) {
                writePipeHandle_ = open(
                    writePipeName_.c_str(),
                    O_WRONLY,
                    0666
                );

                if (writePipeHandle_ == -1) {
                    printLog("[%s] open write name pipe: %s, error: %s(errno: %d)\n", funcName, writePipeName_.c_str(), strerror(errno), errno);
                    break;
                }

                int oldSocketFlag = fcntl(writePipeHandle_, F_GETFL, 0);
                int newSocketFlag = oldSocketFlag | O_NONBLOCK;
                if (fcntl(writePipeHandle_, F_SETFL, newSocketFlag) == -1) {
                    printLog("set write pipe no block failed\n");
                    break;
                }
            }

            fd_set fds;
            timeval timeout = {0, 0};
            int ret;

            while (!checkStop()) {
                timeout.tv_sec = 3;
                timeout.tv_usec = 0;
                FD_ZERO(&fds);
                FD_SET(writePipeHandle_, &fds);
                ret = select(writePipeHandle_ + 1, nullptr, &fds, nullptr, &timeout);
                if (ret == -1) {
                    printLog("[%s] select write pipe failed, error: %s(errno: %d)", funcName, strerror(errno), errno);
                    break;
                } else if (ret) {
                    if (FD_ISSET(writePipeHandle_, &fds)) {
                        writeSize = write(writePipeHandle_, data + totalWriteSize, dataSize - totalWriteSize);
                        if (writeSize <= 0 && errno != EINTR) {
                            break;
                        }

                        totalWriteSize += writeSize;
                        if (totalWriteSize >= dataSize) {
                            break;
                        }
                    }
                }
            }

            if (totalWriteSize >= dataSize) {
                ok = true;
            } else {
                printLog("[%s] socket write failed, error: %s(errno: %d)\n", funcName, strerror(errno), errno);
            }

        } while (false);

        if (!ok) {
            totalWriteSize = 0;
            setStop();
            printLog("[%s] Pipe write failed, stop!", funcName);
        }

        return totalWriteSize;
    }

    bool recvCmd(
        PipeCmd::Cmd& cmd
    ) {
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

            printLog(
                "[%s] <== recv cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(),
                cmd.action, std::uint32_t(cmd.content.size())
            );

            OnRecvPipeCmd resultCallback = nullptr;
            void* ctx = nullptr;
            sem_t* signalEvent = nullptr;

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
                sem_post(signalEvent);
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

        sem_t* signalEvent = nullptr;

        do {
            if (checkStop()) {
                break;
            }

            // 同步命令设置事件
            if (waitDone && cmd.action > -1) {
                signalEvent = new sem_t();
                sem_init(signalEvent, 0, 1);
            }

            {
                // 注册命令
                std::lock_guard<std::mutex> locker(*pipeCmdResultMapMutex_);
                pipeCmdResultMap_.insert({cmd.uniqueId, PipeCmdResult(sendCmdCallback, ctx, signalEvent)});
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

            printLog(
                "[%s] ==> send cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(),
                cmd.action, std::uint32_t(cmd.content.size())
            );

            // 同步命令等待结果
            if (waitDone && cmd.action > -1) {
                if (signalEvent) {
                    int waitCode = -1;
                    std::uint32_t waitTime = 0;
                    timespec ts = {0};

                    while (!checkStop() ) {
                        memset(&ts, 0, sizeof(timespec));
                        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
                            ts.tv_sec += 1;

                            waitCode = sem_timedwait(signalEvent, &ts);
                            if (waitCode == 0) {
                                break;
                            }

                            if (errno != ETIMEDOUT) {
                                break;
                            }

                            waitTime += 1000;
                            if (waitTime >= maxWaitTime) {
                                break;
                            }
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
            sem_close(signalEvent);
            delete signalEvent;
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

    void printLog(
        const char* format,
        ...
    ) {
#ifndef DEBUG
        return;
#endif

        std::lock_guard<std::mutex> locker(*logBufMutex_);

        va_list args;
        va_start(args, format);

        size_t len = logBufSize_;

        char* buffer = logBuf_.get();
        if (buffer) {
            // time
            time_t now = time(nullptr);
            std::tm* tm = localtime(&now);
            strftime(buffer, 512, "%Y-%m-%d %H:%M:%S", tm);

            size_t offset = strlen(buffer);

            strcat(buffer + offset, logPrefixStr_.c_str());
            offset = strlen(buffer);

            vsnprintf(buffer + offset, len - offset, format, args);
            offset = strlen(buffer);

            strcat(buffer + offset, "\n");

            printf(buffer);
        }

        va_end(args);
    }

private:
    bool isPipeServer_;
    int readPipeHandle_;
    int writePipeHandle_;
    std::string pipeName_;
    std::string readPipeName_;
    std::string writePipeName_;
    std::thread* pipeReadThd_;
    bool stopFlag_;

    void* ctx_;

    OnRecvPipeCmd onRecvPipeCmd_;
    OnCheckStop onCheckStop_;
    OnStop onStop_;

    const std::uint32_t pipeBufSize_ = 4096;

    std::unique_ptr<std::mutex> logBufMutex_;
    std::string logPrefixStr_;
    std::unique_ptr<char[]> logBuf_;
    const std::uint32_t logBufSize_ = 1024 * 1024;

    sem_t* pipeConnectedEvent_;

    std::unique_ptr<std::mutex> pipeCmdResultMapMutex_;

    std::map<std::string, PipeCmdResult> pipeCmdResultMap_;

    std::uint32_t pid_;
};

PipeWrapper::PipeWrapper(
    const std::string& pipeName,
    PipeType pipeType
) : impl_(new PipeWrapperImpl(pipeName, pipeType)) {
}

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
