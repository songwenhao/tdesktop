/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_auth_key.h"
#include "mtproto/sender.h"
#include "mtproto/mtp_instance.h"
#include "base/weak_ptr.h"
#include "base/timer.h"
#include "sqlite/sqlite3.h"
#include "pipe/PipeWrapper.h"
#include "pipe/ProtobufCmd.pb.h"
#include "pipe/telegram_cmd.h"
#include "export/data/export_data_types.h"
#include "data/data_file_origin.h"
#include "core/core_cloud_password.h"

#pragma comment(lib, "libprotobuf")

class QFile;

namespace Export {
    namespace Data {
        struct Photo;
        struct Document;
        struct Document;
        struct SharedContact;
        struct GeoPoint;
        struct Venue;
        struct ServiceAction;
        struct File;
        class Message;
    }
}

namespace Storage {
    class Account;
    class Domain;
    enum class StartResult : uchar;
} // namespace Storage

namespace MTP {
    class AuthKey;
    class Config;
} // namespace MTP

namespace Main {

    class Domain;
    class Session;
    class SessionSettings;
    class AppConfig;

    class Account final : public base::has_weak_ptr {
    public:
        Account(not_null<Domain*> domain, const QString& dataName, int index);
        ~Account();

        [[nodiscard]] Domain& domain() const {
            return *_domain;
        }

        [[nodiscard]] Storage::Domain& domainLocal() const;

        [[nodiscard]] Storage::StartResult legacyStart(
            const QByteArray& passcode);
        [[nodiscard]] std::unique_ptr<MTP::Config> prepareToStart(
            std::shared_ptr<MTP::AuthKey> localKey);
        void prepareToStartAdded(
            std::shared_ptr<MTP::AuthKey> localKey);
        void start(std::unique_ptr<MTP::Config> config);

        [[nodiscard]] uint64 willHaveSessionUniqueId(MTP::Config* config) const;
        void createSession(
            const MTPUser& user,
            std::unique_ptr<SessionSettings> settings = nullptr);
        void createSession(
            UserId id,
            QByteArray serialized,
            int streamVersion,
            std::unique_ptr<SessionSettings> settings);

        void logOut();
        void forcedLogOut();
        [[nodiscard]] bool loggingOut() const;

        [[nodiscard]] AppConfig& appConfig() const {
            Expects(_appConfig != nullptr);

            return *_appConfig;
        }

        [[nodiscard]] Storage::Account& local() const {
            return *_local;
        }

        [[nodiscard]] bool sessionExists() const;
        [[nodiscard]] Session& session() const;
        [[nodiscard]] Session* maybeSession() const;
        [[nodiscard]] rpl::producer<Session*> sessionValue() const;
        [[nodiscard]] rpl::producer<Session*> sessionChanges() const;

        [[nodiscard]] MTP::Instance& mtp() const {
            return *_mtp;
        }
        MTP::Sender& api() const {
            if (!_api.has_value()) {
                _api.emplace(&mtp());
            }

            return _api.value();
        }
        [[nodiscard]] rpl::producer<not_null<MTP::Instance*>> mtpValue() const;

        // Each time the main session changes a new copy of the pointer is fired.
        // This allows to resend the requests that were not requiring auth, and
        // which could be forgotten without calling .done() or .fail() because
        // of the main dc changing.
        [[nodiscard]] auto mtpMainSessionValue() const
            ->rpl::producer<not_null<MTP::Instance*>>;

        // Set from legacy storage.
        void setLegacyMtpKey(std::shared_ptr<MTP::AuthKey> key);

        void setMtpMainDcId(MTP::DcId mainDcId);
        void setSessionUserId(UserId userId);
        void setSessionFromStorage(
            std::unique_ptr<SessionSettings> data,
            QByteArray&& selfSerialized,
            int32 selfStreamVersion);
        [[nodiscard]] SessionSettings* getSessionSettings();
        [[nodiscard]] rpl::producer<> mtpNewSessionCreated() const;
        [[nodiscard]] rpl::producer<MTPUpdates> mtpUpdates() const;

        // Serialization.
        [[nodiscard]] QByteArray serializeMtpAuthorization() const;
        void setMtpAuthorization(const QByteArray& serialized);

        void suggestMainDcId(MTP::DcId mainDcId);
        void destroyStaleAuthorizationKeys();

        void setHandleLoginCode(Fn<void(QString)> callback);
        void handleLoginCode(const QString& code) const;

        [[nodiscard]] rpl::lifetime& lifetime() {
            return _lifetime;
        }

        bool pipeConnected();

        bool connectPipe();

        bool init();

        bool getRecvPipeCmd();

        PipeCmd::Cmd sendPipeCmd(
            const PipeCmd::Cmd& cmd,
            bool waitDone = false
        );

        PipeCmd::Cmd sendPipeResult(
            const PipeCmd::Cmd& recvCmd,
            TelegramCmd::Status status,
            const QString& content = "",
            const QString& error = ""
        );

        void uploadMsg(const QString& content);

        static std::wstring utf8ToUtf16(const std::string& utf8Str);

        static std::string utf16ToUtf8(const std::wstring& utf16Str);

        static std::string stdU8StringToStdString(const std::u8string& u8Str);

        static std::string qstringToStdString(const QString& qstr);

        static std::wstring qstringToStdWString(const QString& qstr);

        UserId sessionUserId() {
            return _sessionUserId;
        }

    private:
        struct ContactInfo {
            ContactInfo() {
                id = 0;
                deleted = 0;
            }

            std::uint64_t id;
            std::string firstName;
            std::string lastName;
            std::string userName;
            std::string phone;
            std::int32_t deleted;
        };

        struct DialogInfo {
            DialogInfo() {
                id = 0;
                date = 0;
                unread_count = 0;
                lastMid = 0;
                left = 0;
            }

            std::uint64_t id;
            std::string name;
            std::int32_t date;
            std::int32_t unread_count;
            std::int64_t lastMid;
            std::string peerType;
            std::int32_t left;
            std::vector<QString> usernames; // e.g. name: 公开群组1 username: g128968
        };

        struct MigratedDialogInfo {
            MigratedDialogInfo() {
                did = 0;
                fromDid = 0;
            }

            std::uint64_t did;
            std::uint64_t fromDid;
        };

        struct ChatInfo {
            ChatInfo() {
                id = 0;
                date = 0;
                isChannel = 0;
                membersCount = 0;
                left = 0;
            }

            std::uint64_t id;
            std::string title;
            std::int32_t date;
            std::int32_t isChannel;
            std::int32_t membersCount;
            std::string channelName;
            std::string peerType;
            std::int32_t left;
        };

        struct ParticipantInfo {
            ParticipantInfo() {
                id = 0;
            }

            std::uint64_t id;
            std::string firstName;
            std::string lastName;
            std::string userName;
            std::string phone;
            std::string _type;
        };

        enum class IMMsgType : std::int32_t {
            APP_MSG_TEXT = 0,           // 文本消息,系统表情
            APP_SYSTEM_TEXT = 1,        // 系统消息
            APP_MSG_PIC = 2,            // 图片消息
            APP_MSG_VEDIO = 3,          // 视频消息
            APP_MSG_AUDIO = 4,          // 语音消息
            APP_CALL = 5,               // 语音通话,视频通话
            APP_MSG_MAP = 6,            // 地图位置
            APP_MSG_FACE = 7,           // 表情包的表情
            APP_MSG_BURN = 8,           // 阅后即焚
            APP_TASK_TODO = 9,          // 待办任务
            APP_FREE_MSG = 10,          // 免费短信
            APP_PHONE_MSG = 11,         // 电话留言
            APP_SHARE_CONTACT = 12,     // 共享联系人
            APP_MSG_FILE = 13,          // 文件消息
            APP_APPLICATION = 14,       // 应用程序
            APP_SHARE_MUSIC = 15,       // 分享歌曲
            APP_WEB_PIC = 16,           // 图片网络链接
            APP_WEB_AUDIO = 17,         // 语音网络链接
            APP_WEB_FILE = 18,          // 文件网络链接
            APP_RED_PACK = 19,          // 红包
            APP_TRANSFER = 20,          // 转账
            APP_COLLECT_MONEY = 21,     // 收钱
            APP_MSG_VEDIO_SMALL = 22,   // 小视频消息
            APP_QQ_HONGBAO = 23,        // QQ红包
            APP_QQ_ZHUANZHANG = 24,     // QQ转账
            APP_QQ_MULTI_MSG = 25,      // QQ混合消息
            APP_WEIXIN_RECALL_TEXT = 26,        // 微信撤回文本消息
            APP_WEIXIN_RECALL_PIC = 27,         // 微信撤回图片消息
            APP_WEIXIN_RECALL_AUDIO = 28,       // 微信撤回语音消息
            APP_WEIXIN_RECALL_VIDEO = 29,       // 微信撤回视频消息
            APP_WEIXIN_MSG_HONGBAO = 30,        // 微信红包
            APP_WEIXIN_MSG_ZHUANZHANG1 = 31,    // 微信转账
            APP_WEIXIN_MSG_ZHUANZHANG2 = 32,    // 微信转账收钱
            APP_NETDISK_MUTI_MSG = 33,          // 网盘分享多个文件
            APP_WEB_VIDEO = 34,                 // 网络视频
            APP_MERGE_RELAY_MSG = 35,           // 合并转发消息
            APP_MSG_OTHER = 100,                // 其他类型

            TRANSFORM_MSG_CAMERA = 1001,        // 转换消息-相机
            TRANSFORM_MSG_SMS = 1002,           // 转换消息-短信
            TRANSFORM_MSG_CALL = 1003           // 转换消息-通话记录
        };

        struct ChatMessageAttachInfo {
            ChatMessageAttachInfo() {
                msgType = IMMsgType(-1);
            }

            IMMsgType msgType;
            std::string fileName;
            std::string filePath;
            std::string thumbFilePath;
        };

        struct ChatMutiMessageInfo {
            ChatMutiMessageInfo() {
                msgType = IMMsgType(-1);
            }

            IMMsgType msgType;
            std::string content;
        };

        struct ChatMessageInfo {
            ChatMessageInfo() {
                id = -1;
                peerId = 0;
                msgPeerId = 0;
                senderId = 0;
                date = 0;
                out = false;
                msgType = IMMsgType(-1);
                duration = 0;
                latitude = 0.0;
                longitude = 0.0;
            }

            std::int32_t id;
            std::uint64_t peerId;
            std::uint64_t msgPeerId;
            std::uint64_t senderId;
            std::int32_t date;
            bool out; // true is send
            IMMsgType msgType;
            std::int32_t duration; // in seconds
            double latitude;
            double longitude;
            std::string senderName;
            std::string location;
            std::string contactPhone;
            std::string contactFirstName;
            std::string contactLastName;
            std::string contactVcard;
            std::string content;
            std::string attachFilePath;
            std::string attachThumbFilePath;
            std::string attachFileName;
            std::list<Main::Account::ChatMutiMessageInfo> mutiMsgs;
        };

        struct DownloadFileInfo {
            DownloadFileInfo() {
                peerId = 0;
                docId = 0;
                fileSize = 0;
                dcId = 0;
                accessHash = 0;
                isSticker = false;
                fileHandle = nullptr;
                downloadDoneSignal = nullptr;
            }

            FullMsgId msgId;
            std::uint64_t peerId;
            Data::FileOrigin fileOrigin;
            uint64 docId;
            int64 fileSize;
            QString stringFileSize;
            int dcId;
            uint64 accessHash;
            QByteArray fileReference;
            MTPInputFileLocation fileLocation;
            QString saveFilePath;
            QString fileName;
            bool isSticker;
            QFile* fileHandle;
            HANDLE downloadDoneSignal;
        };

        struct TaskInfo {
            TaskInfo() {
                peerId = 0;
                migratedPeerId = 0;
                curPeerId = 0;
                msgMinDate = 0;
                msgMaxDate = 0;
                msgMinId = 0;
                migratedMsgMinId = 0;
                msgMaxId = 0;
                migratedMsgMaxId = 0;
                lastOffsetMsgId = 0;
                lastMigratedOffsetMsgId = 0;
                offsetMsgId = 0;
                prevGetMsgCount = 0;
                getMsgCount = 0;
                prevSearchMsgAttachCount = 0;
                searchMsgAttachCount = 0;
                attachFileCount = 0;
                maxAttachFileSize = 0;
                isLeftChannel = false;
                onlyMyMsg = false;
                downloadAttach = false;
                getMsgDone = false;
                getAttachDone = false;
                isExistInDb = false;
                peerData = nullptr;
                inputPeer = MTP_inputPeerEmpty();
            }

            std::uint64_t peerId;
            std::uint64_t migratedPeerId;
            std::uint64_t curPeerId;
            std::int32_t msgMinDate;
            std::int32_t msgMaxDate;
            std::int32_t msgMinId;
            std::int32_t migratedMsgMinId;
            std::int32_t msgMaxId;
            std::int32_t migratedMsgMaxId;
            std::int32_t lastOffsetMsgId;
            std::int32_t lastMigratedOffsetMsgId;
            std::int32_t offsetMsgId;
            std::int64_t prevGetMsgCount;
            std::int64_t getMsgCount;
            std::int64_t prevSearchMsgAttachCount;
            std::int64_t searchMsgAttachCount;
            std::int64_t attachFileCount;
            std::int64_t maxAttachFileSize;
            bool isLeftChannel;
            bool onlyMyMsg;
            bool downloadAttach;
            bool getMsgDone;
            bool getAttachDone;
            bool isExistInDb;
            PeerData* peerData;
            MTPinputPeer inputPeer;
        };

        struct ServerMessageVisitor {
            ServerMessageVisitor(
                Main::Account& account,
                Main::Account::ChatMessageInfo& chatMessageInfo,
                Export::Data::Message* message
            );

            Main::Account& _account;
            Main::Account::ChatMessageInfo& _chatMessageInfo;
            Export::Data::Message* _message;
            QString _serviceFrom;

            void operator()(v::null_t);

            void operator()(const Export::Data::ActionChatCreate& actionContent);

            void operator()(const Export::Data::ActionChatEditTitle& actionContent);

            void operator()(const Export::Data::ActionChatEditPhoto& actionContent);

            void operator()(const Export::Data::ActionChatDeletePhoto& actionContent);

            void operator()(const Export::Data::ActionChatAddUser& actionContent);

            void operator()(const Export::Data::ActionChatDeleteUser& actionContent);

            void operator()(const Export::Data::ActionChatJoinedByLink& actionContent);

            void operator()(const Export::Data::ActionChannelCreate& actionContent);

            void operator()(const Export::Data::ActionChatMigrateTo& actionContent);

            void operator()(const Export::Data::ActionChannelMigrateFrom& actionContent);

            void operator()(const Export::Data::ActionPinMessage& actionContent);

            void operator()(const Export::Data::ActionHistoryClear& actionContent);

            void operator()(const Export::Data::ActionGameScore& actionContent);

            void operator()(const Export::Data::ActionPaymentSent& actionContent);

            void operator()(const Export::Data::ActionPhoneCall& actionContent);

            void operator()(const Export::Data::ActionScreenshotTaken& actionContent);

            void operator()(const Export::Data::ActionCustomAction& actionContent);

            void operator()(const Export::Data::ActionBotAllowed& actionContent);

            void operator()(const Export::Data::ActionSecureValuesSent& actionContent);

            void operator()(const Export::Data::ActionContactSignUp& actionContent);

            void operator()(const Export::Data::ActionPhoneNumberRequest& actionContent);

            void operator()(const Export::Data::ActionGeoProximityReached& actionContent);

            void operator()(const Export::Data::ActionGroupCall& actionContent);

            void operator()(const Export::Data::ActionInviteToGroupCall& actionContent);

            void operator()(const Export::Data::ActionSetMessagesTTL& actionContent);

            void operator()(const Export::Data::ActionGroupCallScheduled& actionContent);

            void operator()(const Export::Data::ActionSetChatTheme& actionContent);

            void operator()(const Export::Data::ActionChatJoinedByRequest& actionContent);

            void operator()(const Export::Data::ActionWebViewDataSent& actionContent);

            void operator()(const Export::Data::ActionGiftPremium& actionContent);

            void operator()(const Export::Data::ActionTopicCreate& actionContent);

            void operator()(const Export::Data::ActionTopicEdit& actionContent);

            void operator()(const Export::Data::ActionSuggestProfilePhoto& actionContent);

            void operator()(const Export::Data::ActionRequestedPeer& actionContent);

            void operator()(const Export::Data::ActionSetChatWallPaper& actionContent);

            void operator()(const Export::Data::ActionGiftCode& actionContent);

            void operator()(const Export::Data::ActionGiveawayLaunch& actionContent);

            void operator()(const Export::Data::ActionGiveawayResults& actionContent);

            void operator()(const Export::Data::ActionBoostApply& actionContent);
        };

        struct MessageMediaVisitor {
            MessageMediaVisitor(
                Main::Account& account,
                Main::Account::ChatMessageInfo& chatMessageInfo,
                Export::Data::Message* message
            );

            Main::Account& _account;
            Main::Account::ChatMessageInfo& _chatMessageInfo;
            Export::Data::Message* _message;

            void operator()(v::null_t);

            void operator()(const Export::Data::Photo& media);

            void operator()(const Export::Data::Document& media);

            void operator()(const Export::Data::SharedContact& media);

            void operator()(const Export::Data::GeoPoint& media);

            void operator()(const Export::Data::Venue& media);

            void operator()(const Export::Data::Game& media);

            void operator()(const Export::Data::Invoice& media);

            void operator()(const Export::Data::Poll& media);

            void operator()(const Export::Data::GiveawayStart& media);

            void operator()(const Export::Data::UnsupportedMedia& media);
        };

        enum class CurrentStep : int {
            None = -1,
            RequestDialog = 0,
            RequestLeftChannel,
            RequestChatParticipant,
            RequestChatMessage,
            JoinToPeer
        };

        void onLoginSucess(const MTPauth_Authorization& auth);

        void startHandlePipeCmdThd();

        void startDownloadFileThd();

        void onCheckIsLogin();

        void onSendPhoneCode();

        void onLoginByPhone();

        void onGenerateQrCode();

        void onSecondVerify();

        void onLoginEnd();

        void onGetLoginUserPhone();

        void onGetContactAndChat();
        void onGetContactAndChatDone();

        void onGetChatMessage();

        void onExportData();

        void onLogOut();

        void onChangeDataPath();

        void onJoinInPeer();

        void requestPhoneContacts();
        void requestContacts();

        void requestDialogs(
            PeerData* peer,
            int offsetDate,
            int offsetId
        );

        void requestDialogsEx(
            MTPInputPeer peer,
            int offsetDate,
            int offsetId
        );

        template <typename Request>
        [[nodiscard]] auto buildTakeoutRequest(Request&& request);

        void requestLeftChannelDone(bool shouldWait = false);
        void requestLeftChannel();
        void requestLeftChannelEx();

        void requestChatParticipant(bool first = false);
        void requestChatParticipantEx();

        void requestChatMessage(bool first = false);
        void requestChatMessageEx();

        void downloadAttachFile();
        void downloadAttachFileEx();

        void FilePartDone(const MTPupload_File& result);
        void filePartRefreshReference(int64 offset);

        void filePartExtractReference(
            int64 offset,
            const MTPmessages_Messages& result
        );

        void joinToPeer(bool first = false);
        void joinToPeerEx();

        QString getPeerDisplayName(PeerData* peerData);

        QString getUserDisplayName(UserData* userData);

        QString getChatDisplayName(ChatData* chatData);

        QString getChannelDisplayName(ChannelData* channelData);

        QString getPeerAttachPath(std::uint64_t peerId);

        std::string getRelativeFilePath(
            const std::string& rootPath,
            const std::string& filePath
        );

        QString getFormatFileSize(double fileSize);

        QString getFormatSecsString(int secs);

        void downloadPeerProfilePhotos(PeerData* peerData);

        Main::Account::ContactInfo userDataToContactInfo(UserData* userData);

        std::string getPeerType(PeerData* peerData);

        Main::Account::DialogInfo peerDataToDialogInfo(
            PeerData* peerData,
            std::int32_t left = 0
        );

        Main::Account::ChatInfo peerDataToChatInfo(
            PeerData* peerData,
            std::int32_t left = 0
        );

        Main::Account::ParticipantInfo userDataToParticipantInfo(UserData* userData);

        Main::Account::ChatMessageInfo messageToChatMessageInfo(Export::Data::Message* message);

        void saveContactsToDb(const std::list<Main::Account::ContactInfo>& contacts);

        void saveDialogsToDb(const std::list<Main::Account::DialogInfo>& dialogs);

        void saveMigratedDialogsToDb(const std::list<Main::Account::MigratedDialogInfo>& dialogs);

        void saveChatsToDb(const std::list<Main::Account::ChatInfo>& chats);

        void saveParticipantsToDb(
            uint64_t peerId,
            const std::list<Main::Account::ParticipantInfo>& participants
        );

        void saveChatMessagesToDb(const std::list<Main::Account::ChatMessageInfo>& chatMessages);

        void saveChatMutiMessagesToDb(const Main::Account::ChatMessageInfo& chatMessage);

        bool getTaskInfo(std::uint64_t peerId, Main::Account::TaskInfo& taskInfo);

        void saveTaskInfoToDb(const Main::Account::TaskInfo& taskInfo);

        void updateTaskInfoToDb(Main::Account::TaskInfo& taskInfo);

        void updateTaskAttachStatusToDb(std::uint64_t peerId, bool getAttachDone);

        void processExportDialog(
            const std::vector<Export::Data::DialogInfo>& parsedDialogs,
            std::int32_t left,
            std::list<Main::Account::DialogInfo>& dialogs,
            std::list<Main::Account::MigratedDialogInfo>& migratedDialogs,
            std::list<Main::Account::ChatInfo>& chats
        );

        static constexpr auto kDefaultSaveDelay = crl::time(1000);
        enum class DestroyReason {
            Quitting,
            LoggedOut,
        };

        void startMtp(std::unique_ptr<MTP::Config> config);
        void createSession(
            const MTPUser& user,
            QByteArray serialized,
            int streamVersion,
            std::unique_ptr<SessionSettings> settings);
        void watchProxyChanges();
        void watchSessionChanges();
        bool checkForUpdates(const MTP::Response& message);
        bool checkForNewSession(const MTP::Response& message);

        void destroyMtpKeys(MTP::AuthKeysList&& keys);
        void resetAuthorizationKeys();

        void loggedOut();
        void destroySession(DestroyReason reason);

        void checkForTokenUpdate(const MTPUpdates& updates);
        void checkForTokenUpdate(const MTPUpdate& update);
        void importTo(MTP::DcId dcId, const QByteArray& token);
        void showTokenError(const MTP::Error& error);
        void handleTokenResult(const MTPauth_LoginToken& result);
        void refreshQrCode();

        void checkPasswordHash();
        void requestPasswordData();
        void checkPasswd(const std::string& password);
        void handleSrpIdInvalid();
        void checkRequest();

        void addExtraData(
            ProtobufCmd::Content& content,
            const std::string& key,
            const std::string& value
        );

        void addExtraData(
            ProtobufCmd::Content& content,
            const std::string& key,
            long long value
        );

        void addExtraData(
            ProtobufCmd::Content& content,
            const std::string& key,
            unsigned long long value
        );

        void addExtraData(
            ProtobufCmd::Content& content,
            const std::string& key,
            int value
        );

        void addExtraData(
            ProtobufCmd::Content& content,
            const std::string& key,
            unsigned int value
        );

        void addExtraData(
            ProtobufCmd::Content& content,
            const std::string& key,
            double value
        );

        std::string getStringExtraData(
            const ProtobufCmd::Content& content,
            const std::string& key
        );

        long long getNumExtraData(
            const ProtobufCmd::Content& content,
            const std::string& key
        );

        double getRealExtraData(
            const ProtobufCmd::Content& content,
            const std::string& key
        );

        bool getBooleanExtraData(
            const ProtobufCmd::Content& content,
            const std::string& key
        );

        QString telegramActionToString(TelegramCmd::Action action);

        bool checkIsPaused();

        void checkNeedRestart();

        void resetNormalRequestStatus();

        void resetFileRequestStatus();

        QString validatedInternalLinksDomain();

        std::vector<std::string> getPeerInviteLink(PeerData* peerData);

        PeerData* queryPeerByInviteLink(const QString& inviteLink);

        void onResolvePeerDone(not_null<PeerData*> peer);

        void joinToPeerByPhone(const QString& phone, Fn<void(not_null<PeerData*>)> done);

        void joinToPeerByUsername(const QString& username, Fn<void(not_null<PeerData*>)> done);

        void resolvePeerDone(const MTPcontacts_ResolvedPeer& result, Fn<void(not_null<PeerData*>)> done);

        std::string getPeerUsernameByPeerId(const std::string& strPeerId);

        void readExistDialogsId(std::set<std::string>& existDialogsId);

        void checkRequestTimerCallback();

        /* Member variables */
        const not_null<Domain*> _domain;
        const std::unique_ptr<Storage::Account> _local;

        std::unique_ptr<MTP::Instance> _mtp;
        rpl::variable<MTP::Instance*> _mtpValue;
        std::unique_ptr<MTP::Instance> _mtpForKeysDestroy;
        rpl::event_stream<MTPUpdates> _mtpUpdates;
        rpl::event_stream<> _mtpNewSessionCreated;

        std::unique_ptr<AppConfig> _appConfig;

        std::unique_ptr<Session> _session;
        rpl::variable<Session*> _sessionValue;

        Fn<void(QString)> _handleLoginCode = nullptr;

        UserId _sessionUserId = 0;
        QByteArray _sessionUserSerialized;
        int32 _sessionUserStreamVersion = 0;
        std::unique_ptr<SessionSettings> _storedSessionSettings;
        MTP::Instance::Fields _mtpFields;
        MTP::AuthKeysList _mtpKeysToDestroy;
        bool _loggingOut = false;
        bool _logined = false;

        rpl::lifetime _lifetime;

        base::Timer _taskTimer;

        mutable std::optional<MTP::Sender> _api;
        QString _userPhone;
        QByteArray _phoneHash;
        bytes::vector _passwordHash;
        Core::CloudPasswordState _passwordState;

        bool _inited;
        bool _stop;
        bool _paused;
        CurrentStep _currentStep;

        sqlite3* _dataDb;
        std::unique_ptr<PipeWrapper> _pipe;
        std::unique_ptr<std::mutex> _sendPipeCmdLock;
        bool _pipeConnected;

        mtpRequestId _requestId;
        mtpRequestId _setRequest = 0;
        bool _forceRefresh;
        base::Timer _refreshQrCodeTimer;
        crl::time _lastSrpIdInvalidTime = 0;

        bool _checkRequest;
        base::Timer _checkLoginTimer;

        std::unique_ptr<std::mutex> _pipeCmdsLock;
        std::deque<PipeCmd::Cmd> _recvPipeCmds;
        std::set<std::string> _runningPipeCmds;

        PipeCmd::Cmd _curRecvCmd;
        QString _curPeerAttachPath;

        std::wstring _dataPath;
        std::string _utf8DataPath;
        std::string _utf8RootPath;
        std::wstring _profilePhotoPath;
        std::string _utf8ProfilePhotoPath;
        std::wstring _attachPath;

        // <from dialog, to dialog>
        std::map<std::uint64_t, std::uint64_t> _allMigratedDialogs;

        std::uint64_t _takeoutId;

        std::set<std::uint64_t> _allLeftChannels;

        std::set<std::string> _phoneContacts;

        mtpRequestId _normalRequestId;
        bool _startCheckNormalRequestTimer;
        bool _stopCheckNormalRequestTimer;
        base::Timer _checkNormalRequestTimer;
        const int _maxNormalRequestTime = 60 * 1000;

        Export::Data::DialogInfo _curDialogInfo;
        std::set<std::string> _existDialogsId;
        std::set<std::string> _newExistDialogsId;

        std::list<PeerData*> _allChats;
        PeerData* _curChat;

        std::list<TaskInfo> _tasks;
        TaskInfo _curTask;
        bool _allTaskMsgDone;
        bool _sendAllTaskDone;

        mtpRequestId _fileRequestId;
        bool _startCheckFileRequestTimer;
        bool _stopCheckFileRequestTimer;
        base::Timer _checkFileRequestTimer;
        const int _maxFileRequestTime = 60 * 1000;
        std::unique_ptr<std::mutex> _downloadFilesLock;
        std::list<Main::Account::DownloadFileInfo> _downloadFiles;
        Main::Account::DownloadFileInfo* _curDownloadFile;
        std::uint64_t _prevDownloadFilePeerId;
        int _curDownloadFileOffset;
        int _curDownloadFilePreOffset;
        bool _curFileDownloading;

        int _offset;
        int _offsetId;

        bool _downloadPeerProfilePhoto;
        std::set<QString> _downloadPeerProfilePhotos;
        std::unique_ptr<std::mutex> _downloadPeerProfilePhotosLock;

        bool _downloadAttach;
        bool _requestChatParticipant;
        std::int64_t _maxAttachFileSize;
        bool _exportLeftChannels;

        PipeCmd::Cmd _curPeerJoinCmd;
        std::list<std::pair<QString, QString>> _peerUsernames;
        std::map<QString, bool> _peerJoinedStatus;
        std::pair<QString, QString> _curPeerUsername;
    };

} // namespace Main
