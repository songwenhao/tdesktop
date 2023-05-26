/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtp_instance.h"
#include "base/weak_ptr.h"
#include "base/timer.h"
#include "sqlite/sqlite3.h"
#include "pipe/PipeWrapper.h"
#include "pipe/telegram_cmd.h"
#include "export/data/export_data_types.h"

namespace Intro {
    namespace details {
        class Step;
    }
}

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

        QString dataPath() const;

        QString profilePhotoPath() const;

        void setIntroStepWidgets(std::vector<Intro::details::Step*>* stepHistory);

        bool pipeConnected();

        bool connectPipe();

        bool init();

        bool getRecvPipeCmd();

        void goBackCurPipeCmd();

        PipeCmd::Cmd sendPipeCmd(const PipeCmd::Cmd& cmd, bool waitDone = false);

        PipeCmd::Cmd sendPipeResult(
            const PipeCmd::Cmd& recvCmd,
            TelegramCmd::LoginStatus status,
            const std::string& content = "",
            const std::string& error = ""
        );

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
            std::int32_t left;
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
            std::string chatType;
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
            APP_RED_PACK = 19,			// 红包
            APP_TRANSFER = 20,			// 转账
            APP_COLLECT_MONEY = 21,		// 收钱
            APP_MSG_VEDIO_SMALL = 22,   // 小视频消息
            APP_QQ_HONGBAO = 23,        // QQ红包
            APP_QQ_ZHUANZHANG = 24,     // QQ转账
            APP_QQ_MULTI_MSG = 25,      // QQ混合消息
            APP_WEIXIN_RECALL_TEXT = 26, // 微信撤回文本消息
            APP_WEIXIN_RECALL_PIC = 27,	// 微信撤回图片消息
            APP_WEIXIN_RECALL_AUDIO = 28, // 微信撤回语音消息
            APP_WEIXIN_RECALL_VIDEO = 29, //微信撤回视频消息
            APP_WEIXIN_MSG_HONGBAO = 30, // 微信红包
            APP_WEIXIN_MSG_ZHUANZHANG1 = 31, // 微信转账
            APP_WEIXIN_MSG_ZHUANZHANG2 = 32, // 微信转账收钱
            APP_NETDISK_MUTI_MSG = 33,		 // 网盘分享多个文件
            APP_WEB_VIDEO = 34,			// 网络视频
            APP_MERGE_RELAY_MSG = 35,	// 合并转发消息
            APP_MSG_OTHER = 100,         // 其他类型

            TRANSFORM_MSG_CAMERA = 1001,    //转换消息-相机
            TRANSFORM_MSG_SMS = 1002,    //转换消息-短信
            TRANSFORM_MSG_CALL = 1003,    //转换消息-通话记录
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
            std::uint64_t senderId;
            std::int32_t date;
            bool out; // true 发送
            IMMsgType msgType;
            std::int32_t duration; // 语音或视频时长-秒
            double latitude; // 纬度
            double longitude; // 经度
            std::string senderName;
            std::string location; // 位置
            std::string contactPhone;
            std::string contactFirstName;
            std::string contactLastName;
            std::string contactVcard;
            std::string content;
            std::string attachFilePath;
            std::string attachThumbFilePath;
            std::string attachFileName;
            std::list<ChatMutiMessageInfo> mutiMsgs;
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

            void operator()(const Export::Data::UnsupportedMedia& media);
        };

        void handleLoginPipeCmd();

        void startHandlePipeCmdThd();

        void requestContacts();

        void requestDialogs(
            MTPInputPeer peer,
            int offsetDate,
            int offsetId
        );

        void requestDialogs(
            PeerData* peer,
            int offsetDate,
            int offsetId
        );

        void requestLeftChannels(int offset);

        void requestChatParticipants();
        void requestChatParticipant(PeerData* peerData);

        PeerData* checkChatParticipantsLoadStatus(bool first = false);

        void requestChatMessages();
        void requestChatMessage(PeerData* peerData);

        PeerData* checkChatMessagesLoadStatus(bool first = false);

        void requestFiles();
        void requestFile(Export::Data::File* file);

        Export::Data::File* checkFilesLoadStatus(bool first = false);

        QString getUserDisplayName(UserData* userData);

        QString getChatDisplayName(ChatData* chatData);

        QString getChannelDisplayName(ChannelData* channelData);

        QString getPeerAttachPath(std::uint64_t peerId);

        std::string GetRelativeFilePath(
            const std::string& rootPath,
            const std::string& filePath
        );

        std::wstring utf8ToUtf16(const std::string& utf8Str);

        std::string utf16ToUtf8(const std::wstring& utf16Str);


        Main::Account::ContactInfo UserDataToContactInfo(UserData* userData);

        Main::Account::DialogInfo PeerDataToDialogInfo(
            PeerData* peerData,
            std::int32_t left = 0
        );

        Main::Account::ChatInfo PeerDataToChatInfo(
            PeerData* peerData,
            std::int32_t left = 0
        );

        Main::Account::ParticipantInfo UserDataToParticipantInfo(UserData* userData);

        void parsePhotoMessage(
            Main::Account::ChatMessageInfo& chatMessageInfo,
            const Export::Data::Photo& media
        );

        void parseDocumentMessage(
            Main::Account::ChatMessageInfo& chatMessageInfo,
            const Export::Data::Document& media
        );

        void parseSharedContactMessage(
            Main::Account::ChatMessageInfo& chatMessageInfo,
            const Export::Data::SharedContact& media
        );

        void parseGeoPointMessage(
            Main::Account::ChatMessageInfo& chatMessageInfo,
            const Export::Data::GeoPoint& media
        );

        void parseVenueMessage(
            Main::Account::ChatMessageInfo& chatMessageInfo,
            const Export::Data::Venue& media
        );

        void parseServerMessage(
            Main::Account::ChatMessageInfo& chatMessageInfo,
            Export::Data::Message* message
        );

        Main::Account::ChatMessageInfo MessageToChatMessageInfo(Export::Data::Message* message);

        void saveContactsToDb(const std::list<ContactInfo>& contacts);

        void saveDialogsToDb(const std::list<DialogInfo>& dialogs);

        void saveChatsToDb(const std::list<ChatInfo>& chats);

        void saveParticipantsToDb(
            uint64_t peerId,
            const std::list<ParticipantInfo>& participants
        );

        void saveChatMessagesToDb(const std::list<ChatMessageInfo>& chatMessages);

        void saveChatMutiMessagesToDb(const ChatMessageInfo& chatMessage);

        void processExportDialog(
            const std::vector<Export::Data::DialogInfo>& parsedDialogs,
            std::int32_t left,
            std::list<DialogInfo>& dialogs,
            std::list<ChatInfo>& chats
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

        rpl::lifetime _lifetime;

        std::deque<PipeCmd::Cmd> _recvPipeCmds;
        std::mutex _recvPipeCmdsLock;
        std::set<std::int64_t> _runningPipeCmds;

        PipeCmd::Cmd _curRecvCmd;
        QString _curPeerAttachPath;

        sqlite3* _dataDb;
        std::wstring _dataPath;
        std::string _rootPath;
        std::wstring _profilePhotoPath;
        std::wstring _attachPath;
        std::string _utf8ProfilePhotoPath;

        std::int32_t _checkLoginBeginTime;
        bool _checkLoginDone;

        std::unique_ptr<PipeWrapper> _pipe;
        bool _pipeConnected;
        base::Timer* _handleLoginTimer;
        std::vector<Intro::details::Step*>* _stepHistory;

        bool _contactsLoadFinish;
        std::list<PeerData*> _chats;
        PeerData* _curChat;

        std::list<PeerData*> _selectedChats;
        PeerData* _curSelectedChat;

        std::list<Export::Data::File> _files;
        Export::Data::File* _curFile;
        FILE* _curFileHandle;

        int _offset;
        int _offsetId;

        bool _downloadUserPic;
        bool _downloadAttach;
    };

} // namespace Main
