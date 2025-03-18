#include <vector>
#include <map>
#include <thread>
#include <mutex>

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN             // 从 Windows 头中排除极少使用的资料
// Windows 头文件: 
#include <windows.h>

#define  _WINSOCK_DEPRECATED_NO_WARNINGS
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <semaphore.h>
#define HANDLE sem_t*
#endif
#include <cstdarg>
#include <cstring>
#include <future>

#include "SocketWrapper.h"

class SocketWrapper::SocketWrapperImpl {

    struct CommandResult {
        CommandResult() {
            this->resultCallback = nullptr;
            this->ctx = nullptr;
            this->signalEvent = nullptr;
        }

        CommandResult(
            OnRecvCmd resultCallback,
            void* ctx,
            HANDLE signalEvent
            ) {
            this->resultCallback = resultCallback;
            this->ctx = ctx;
            this->signalEvent = signalEvent;
        }

        Command::Cmd result;
        OnRecvCmd resultCallback;
        void* ctx;
        HANDLE signalEvent;
    };

public:
    SocketWrapperImpl(
        const std::string& host,
        std::uint16_t port,
        SocketType socketType
        ) : host_(host),
        port_(port),
        isSocketServer_(socketType == SocketType::SocketServer),
        serverSocket_(-1),
        clientSocket_(-1),
        socketReadThd_(nullptr),
        stopFlag_(false),
        ctx_(nullptr),
        onRecvCmd_(nullptr),
        onCheckStop_(nullptr),
        onStop_(nullptr),
        logBufMutex_(nullptr),
        logBuf_(nullptr),
        socketConnectedEvent_(nullptr),
        CmdResultMapMutex_(nullptr),
        pid_(0) {
        logBufMutex_ = std::make_unique<std::mutex>();
        logBuf_ = std::make_unique<char[]>(logBufSize_);
        logPrefixStr_ = (isSocketServer_ ? "[server]" : "[client]");

#ifdef _MSC_VER
        socketConnectedEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
        pid_ = GetCurrentProcessId();
#else
        socketConnectedEvent_ = new sem_t();
        sem_init(socketConnectedEvent_, 0, 1);
        pid_ = syscall(SYS_getpid);
#endif

        CmdResultMapMutex_ = std::make_unique<std::mutex>();
    }

    ~SocketWrapperImpl() {
        disConnectSocket();
    }

    void connectThd(
        const std::function<bool()>& checkStopConnect,
        std::promise<bool> &promiseObj,
        std::uint64_t maxWaitTime
        ) {
        bool connected = false;

        std::string logPrefixStr = std::string("[") + __FUNCTION__ + "] connect socket";

        printLog("%s begin ...", logPrefixStr.c_str());

        do {
            if (isSocketServer_) {
                if (serverSocket_ == -1) {
                    printLog("create socket error: %s(errno: %d)\n", strerror(errno), errno);
                    break;
                }

                if ((clientSocket_ = accept(serverSocket_, (struct sockaddr*)NULL, NULL)) != -1) {
                    connected = true;
                }

                // socket设置为非阻塞
#ifdef _MSC_VER
                unsigned long on = 1;
                if (ioctlsocket(clientSocket_, FIONBIO, &on) < 0) {
                    printLog("set socket no block failed\n");
                    break;
                }
#else
                int oldSocketFlag = fcntl(clientSocket_, F_GETFL, 0);
                int newSocketFlag = oldSocketFlag | O_NONBLOCK;
                if (fcntl(clientSocket_, F_SETFL, newSocketFlag) == -1) {
                    printLog("set socket no block failed\n");
                    break;
                }

                // unsigned long on = 1;
                // if (ioctl(clientSocket_, FIONBIO, &on) < 0) {
                //     printLog("set socket no block failed\n");
                //     break;
                // }
#endif
            } else {
                if (clientSocket_ == -1) {
                    break;
                }

                std::uint64_t waitTime = 0;
                fd_set fds;
                timeval tv;
                int ret;

                while (true) {
                    if (checkStopConnect && checkStopConnect()) {
                        break;
                    }

                    ret = connect(clientSocket_, (struct sockaddr*)&socketAddr_, sizeof(socketAddr_));
                    if (ret == 0) {
                        printLog("connect success\n");
                        connected = true;
                        break;
                    }

                    // 因为是非阻塞的，这个时候错误码应该是WSAEWOULDBLOCK，Linux下是EINPROGRESS
                    if (ret < 0 && errno != EINPROGRESS) {
                        printLog("connect failed error: %s(errno: %d)\n", strerror(errno), errno);
                        break;
                    }

                    FD_ZERO(&fds);
                    FD_SET(clientSocket_, &fds);

                    tv.tv_sec = 1;
                    tv.tv_usec = 0;
                    ret = select(clientSocket_ + 1, NULL, &fds, NULL, &tv);
                    if (ret == 0) {
                        printLog("connect timeout\n");
                        waitTime += 1000;
                        if (waitTime > maxWaitTime) {
                            break;
                        }
                    } else if (ret < 0) {
                        printLog("connect failed error: %s(errno: %d)\n", strerror(errno), errno);
                    } else {
                        printLog("connect success\n");
                        connected = true;
                        break;
                    }
                }
            }

        } while (false);

        promiseObj.set_value(connected);

        printLog("%s end ...", logPrefixStr.c_str());

        if (connected) {
            bool ret = false;

            Command::Cmd cmd;

            while (true) {
                if (checkStop()) {
                    break;
                }

                ret = recvCmd(cmd);
                if (!ret) {
                    continue;
                }

                if (onRecvCmd_) {
                    onRecvCmd_(ctx_, cmd);
                }
            }
        } else {
            if (serverSocket_ != -1) {
#ifdef _MSC_VER
                closesocket(serverSocket_);
#else
                close(serverSocket_);
#endif
                serverSocket_ = -1;
            }

            if (clientSocket_ != -1) {
#ifdef _MSC_VER
                closesocket(clientSocket_);
#else
                close(clientSocket_);
#endif
                clientSocket_ = -1;
            }
        }
    }

    bool init() {
        bool ret = false;
        serverSocket_ = -1;
        clientSocket_ = -1;

        std::string logPrefixStr = std::string("[") + __FUNCTION__ + "] init socket";

        printLog("%s begin ...", logPrefixStr.c_str());

        do {
            if (isSocketServer_) {
                if ((serverSocket_ = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                    printLog("create socket error: %s(errno: %d)\n", strerror(errno), errno);
                    break;
                }

                int reuse = 1;
                if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse , sizeof(int)) == -1) {
                    printLog("setsockopt SO_REUSEADDR error: %s(errno: %d)\n", strerror(errno), errno);
                    break;
                }

                memset(&socketAddr_, 0, sizeof(socketAddr_));
                socketAddr_.sin_family = AF_INET;

                if (port_ == 0 || port_ >= 65535) {
                    socketAddr_.sin_port = htons(INADDR_ANY);
                } else {
                    socketAddr_.sin_port = htons(port_);
                }

                if (inet_pton(AF_INET, host_.c_str(), &socketAddr_.sin_addr.s_addr) <= 0){
                    printLog("inet_pton error for %s\n", host_.c_str());
                    break;
                }

                if (bind(serverSocket_, (struct sockaddr *)&socketAddr_, sizeof(socketAddr_)) == -1) {
                    printLog("bind socket error: %s(errno: %d)\n", strerror(errno), errno);
                    break;
                }

                // 获取真实绑定的端口
                {
                    struct sockaddr_in localaddr;
                    socklen_t len = sizeof(localaddr);
                    int ret = getsockname(serverSocket_, (struct sockaddr*)&localaddr, &len);
                    if (ret == 0) {
                        port_ = ntohs(localaddr.sin_port);
                        printLog("real port: %d\n", port_);
                    }
                }

                if (listen(serverSocket_, 1) == -1) {
                    printLog("listen socket error: %s(errno: %d)\n", strerror(errno), errno);
                    break;
                }

                ret = true;

            } else {
                if (port_ == 0 || port_ >= 65535) {
                    printLog("error port: %d\n", port_);
                    break;
                }

                if ((clientSocket_ = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                    printLog("create socket error: %s(errno: %d)\n", strerror(errno), errno);
                    break;
                }

                memset(&socketAddr_, 0, sizeof(socketAddr_));
                socketAddr_.sin_family = AF_INET;
                socketAddr_.sin_port = htons(port_);

                if (inet_pton(AF_INET, host_.c_str(), &socketAddr_.sin_addr.s_addr) <= 0){
                    printLog("inet_pton error for %s\n", host_.c_str());
                    break;
                }

                // socket设置为非阻塞
#ifdef _MSC_VER
                unsigned long on = 1;
                if (ioctlsocket(clientSocket_, FIONBIO, &on) < 0) {
                    printLog("set socket no block failed\n");
                    break;
                }
#else
                int oldSocketFlag = fcntl(clientSocket_, F_GETFL, 0);
                int newSocketFlag = oldSocketFlag | O_NONBLOCK;
                if (fcntl(clientSocket_, F_SETFL, newSocketFlag) == -1) {
                    printLog("set socket no block failed\n");
                    break;
                }
                // unsigned long on = 1;
                // if (ioctl(clientSocket_, FIONBIO, &on) < 0) {
                //     printLog("set socket no block failed\n");
                //     break;
                // }
#endif
                ret = true;
            }

        } while (false);

        if (!ret) {
            if (serverSocket_ != -1) {
#ifdef _MSC_VER
                closesocket(serverSocket_);
#else
                close(serverSocket_);
#endif
                serverSocket_ = -1;
            }

            if (clientSocket_ != -1) {
#ifdef _MSC_VER
                closesocket(clientSocket_);
#else
                close(clientSocket_);
#endif
                clientSocket_ = -1;
            }
        }

        printLog("%s end ...", logPrefixStr.c_str());

        return ret;
    }

    std::uint16_t getServerSocketPort() const {
        return port_;
    }

    bool connectSocket(
        const std::function<bool()>& checkStop,
        std::uint64_t maxWaitTime
        ) {
        bool connected = false;
        std::promise<bool> promiseObj;

        socketReadThd_ = new std::thread(
            std::bind(
                &SocketWrapperImpl::connectThd, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3
                ), checkStop, std::ref(promiseObj), maxWaitTime
            );

        connected = promiseObj.get_future().get();

        return connected;
    }

    void disConnectSocket() {
        setStop();

        if (serverSocket_ != -1) {
#ifdef _MSC_VER
            closesocket(serverSocket_);
#else
            close(serverSocket_);
#endif
            serverSocket_ = -1;
        }

        if (clientSocket_ != -1) {
#ifdef _MSC_VER
            closesocket(clientSocket_);
#else
            close(clientSocket_);
#endif
            clientSocket_ = -1;
        }

        if (socketConnectedEvent_) {
#ifdef _MSC_VER
            CloseHandle(socketConnectedEvent_);
#else
            sem_close(socketConnectedEvent_);
            delete socketConnectedEvent_;
#endif
            socketConnectedEvent_ = nullptr;
        }

        if (socketReadThd_) {
            if (socketReadThd_->joinable()) {
                socketReadThd_->join();
            }
            delete socketReadThd_;
            socketReadThd_ = nullptr;
        }
    }

    std::uint32_t socketRead(
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
                FD_SET(clientSocket_, &fds);
                ret = select(clientSocket_ + 1, &fds, nullptr, nullptr, &timeout);
                if (ret == -1) {
                    printLog("[%s] select read socket failed, error: %s(errno: %d)", funcName, strerror(errno), errno);
                    break;
                } else if (ret) {
                    /*if (FD_ISSET(clientSocket_, &fds)) {
                    }*/
                    readSize = read(clientSocket_, data + totalReadSize, dataSize - totalReadSize);
                    if (readSize <= 0 && errno != EINTR) {
                        break;
                    }

                    totalReadSize += readSize;
                    if (totalReadSize >= dataSize) {
                        break;
                    }
                } else if (ret == 0) {
                    //time out when ret = 0
                    continue;
                }
            }

            if (totalReadSize >= dataSize) {
                ok = true;
            } else {
                printLog("[%s] socket read failed, error: %s(errno: %d)\n", funcName, strerror(errno), errno);
            }

        } while (false);

        if (!ok) {
            totalReadSize = 0;
            setStop();
            printLog("[%s] socket read failed, stop!", funcName);
        }

        return totalReadSize;
    }

    std::uint32_t socketWrite(
        const char* data,
        std::uint32_t dataSize
        ) {
        const char* funcName = __FUNCTION__;

        bool ok = false;
        std::uint32_t totalWriteSize = 0;
        ssize_t writeSize = 0;

        do {
            fd_set fds;
            timeval timeout = {0, 0};
            int ret;

            while (!checkStop()) {
                timeout.tv_sec = 3;
                timeout.tv_usec = 0;
                FD_ZERO(&fds);
                FD_SET(clientSocket_, &fds);
                ret = select(clientSocket_ + 1, nullptr, &fds, nullptr, &timeout);
                if (ret == -1) {
                    printLog("[%s] select write socket failed, error: %s(errno: %d)", funcName, strerror(errno), errno);
                    break;
                } else if (ret) {
                    if (FD_ISSET(clientSocket_, &fds)) {
                        writeSize = write(clientSocket_, data + totalWriteSize, dataSize - totalWriteSize);
                        if (writeSize <= 0 && errno != EINTR) {
                            break;
                        }

                        totalWriteSize += writeSize;
                        if (totalWriteSize >= dataSize) {
                            break;
                        }
                    }
                } else if (ret == 0) {
                    //time out when ret = 0
                    continue;
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
            printLog("[%s] socket write failed, stop!", funcName);
        }

        return totalWriteSize;
    }

    bool recvCmd(
        Command::Cmd& cmd
        ) {
        const char* funcName = __FUNCTION__;

        bool ok = false;

        std::uint32_t dataSize = 0;
        char* data = nullptr;

        do {
            cmd.Clear();

            // 接收命令字节数
            std::uint32_t readSize = socketRead((char*)&dataSize, sizeof(std::uint32_t));
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

            readSize = socketRead(data, dataSize);
            if (!readSize) {
                break;
            }

            ok = Command::BlobDataToCmd(cmd, data, dataSize);
            if (!ok) {
                break;
            }

            printLog(
                "[%s] <== recv cmd, unique ID: %s action: %d content size: %d", funcName, cmd.uniqueId.c_str(),
                cmd.action, std::uint32_t(cmd.content.size())
                );

            OnRecvCmd resultCallback = nullptr;
            void* ctx = nullptr;
            HANDLE signalEvent = nullptr;

            {
                // 设置命令执行结果，取出命令注册的信息
                std::lock_guard<std::mutex> locker(*CmdResultMapMutex_);
                auto iter = cmdResultMap_.find(cmd.uniqueId);
                if (iter != cmdResultMap_.end()) {
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
#ifdef _MSC_VER
                SetEvent(signalEvent);
#else
                sem_post(signalEvent);
#endif
            } else {
                if (resultCallback) {
                    resultCallback(ctx, cmd);
                }

                // 异步命令无需保留执行结果
                std::lock_guard<std::mutex> locker(*CmdResultMapMutex_);
                auto iter = cmdResultMap_.find(cmd.uniqueId);
                if (iter != cmdResultMap_.end()) {
                    cmdResultMap_.erase(iter);
                }
            }

        } while (false);

        if (data) {
            delete[] data;
        }

        return ok;
    }

    Command::Cmd sendCmd(
        const Command::Cmd& cmd,
        bool waitDone,
        std::uint32_t maxWaitTime,
        OnRecvCmd sendCmdCallback,
        void* ctx
        ) {
        const char* funcName = __FUNCTION__;

        Command::Cmd resultCmd;

        char* buf = nullptr;

        HANDLE signalEvent = nullptr;

        do {
            if (checkStop()) {
                break;
            }

            // 同步命令设置事件
            if (waitDone && cmd.action > -1) {
#ifdef _MSC_VER
                signalEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
#else
                signalEvent = new sem_t();
                sem_init(signalEvent, 0, 0);
#endif
            }

            {
                // 注册命令
                std::lock_guard<std::mutex> locker(*CmdResultMapMutex_);
                cmdResultMap_.insert({cmd.uniqueId, CommandResult(sendCmdCallback, ctx, signalEvent)});
            }

            std::uint32_t dataSize = 0;
            buf = Command::CmdToBlobData(cmd, dataSize);
            if (!buf || !dataSize) {
                break;
            }

            std::uint32_t writeSize = socketWrite((const char*)&dataSize, sizeof(std::uint32_t));
            if (!writeSize) {
                break;
            }

            writeSize = socketWrite(buf, dataSize);
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

#ifdef _MSC_VER
                    DWORD waitCode = -1;
                    DWORD waitTime = 0;

                    if (signalEvent) {
                        while (!CheckStop()) {
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
#else
                    int waitCode = -1;
                    std::int32_t waitTime = 0;
                    timespec ts;

                    while (!checkStop()) {
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
#endif
                }

                {
                    std::lock_guard<std::mutex> locker(*CmdResultMapMutex_);
                    auto iter = cmdResultMap_.find(cmd.uniqueId);
                    if (iter != cmdResultMap_.end()) {
                        resultCmd = iter->second.result;
                        cmdResultMap_.erase(iter);
                    }
                }
            }
        } while (false);

        if (buf) {
            delete[] buf;
            buf = nullptr;
        }

        if (signalEvent) {
#ifdef _MSC_VER
            CloseHandle(signalEvent);
#else
            sem_close(signalEvent);
            delete signalEvent;
#endif
            signalEvent = nullptr;
        }

        return resultCmd;
    }

    void registerCallback(
        void* ctx,
        const OnRecvCmd& onRecvPipeCmd,
        const OnCheckStop& onCheckStop,
        const OnStop& onStop
        ) {
        ctx_ = ctx;
        onRecvCmd_ = onRecvPipeCmd;
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
#ifdef _MSC_VER
    #ifndef _DEBUG
        return;
    #endif
#else
    #ifndef DEBUG
        return;
    #endif
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

#ifdef _MSC_VER
            OutputDebugStringA(buffer);
#endif
        }

        va_end(args);
    }

private:
    std::string host_;
    std::uint16_t port_;
    struct sockaddr_in socketAddr_;
    bool isSocketServer_;
    int serverSocket_;
    int clientSocket_;

    std::thread* socketReadThd_;
    bool stopFlag_;

    void* ctx_;

    OnRecvCmd onRecvCmd_;
    OnCheckStop onCheckStop_;
    OnStop onStop_;

    std::unique_ptr<std::mutex> logBufMutex_;
    std::string logPrefixStr_;
    std::unique_ptr<char[]> logBuf_;
    const std::uint32_t logBufSize_ = 1024 * 1024;

    HANDLE socketConnectedEvent_;

    std::unique_ptr<std::mutex> CmdResultMapMutex_;

    std::map<std::string, CommandResult> cmdResultMap_;

    std::uint32_t pid_;
};

SocketWrapper::SocketWrapper(
    const std::string& host,
    std::uint16_t port,
    SocketType socketType
    ) : impl_(new SocketWrapperImpl(host, port, socketType)) {
}

SocketWrapper::~SocketWrapper() {
    if (impl_) {
        delete impl_;
    }
}

bool SocketWrapper::init() {
    return impl_->init();
}

std::uint16_t SocketWrapper::getServerSocketPort() const {
    return impl_->getServerSocketPort();
}

bool SocketWrapper::connectSocket(
    const std::function<bool()>& checkStop,
    std::uint64_t maxWaitTime
    ) {
    return impl_->connectSocket(checkStop, maxWaitTime);
}

void SocketWrapper::disConnectSocket() {
    return impl_->disConnectSocket();
}

void SocketWrapper::registerCallback(
    void* ctx,
    const OnRecvCmd& onRecvPipeCmd,
    const OnCheckStop& onCheckStop,
    const OnStop& onStop
    ) {
    impl_->registerCallback(ctx, onRecvPipeCmd, onCheckStop, onStop);
}

Command::Cmd SocketWrapper::sendCmd(
    const Command::Cmd& cmd,
    bool waitDone,
    std::uint32_t maxWaitTime,
    const OnRecvCmd& sendCmdCallback,
    void* ctx
    ) {
    return impl_->sendCmd(cmd, waitDone, maxWaitTime, sendCmdCallback, ctx);
}
