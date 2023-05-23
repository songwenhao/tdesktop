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

namespace Intro {
    namespace details {
        class Step;
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

        bool connectPipe();

        bool init();

        bool getRecvPipeCmd(PipeCmd::Cmd& cmd);

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
            }

            std::uint64_t id;
            std::string name;
            std::int32_t date;
            std::int32_t unread_count;
            std::int64_t lastMid;
        };

        struct ChatInfo {
            ChatInfo() {
                id = 0;
                date = 0;
                isChannel = 0;
                membersCount = 0;
            }

            std::uint64_t id;
            std::string title;
            std::int32_t date;
            std::int32_t isChannel;
            std::int32_t membersCount;
            std::string channelName;
            std::string chatType;
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

        void handleLoginPipeCmd();

        void startHandlePipeCmdThd();

        void requestContacts();

        void requestDialogs(
            PeerData* offsetPeer,
            int offsetDate,
            int offsetId
        );

        void requestChatParticipants();
        void requestChatParticipant(
            PeerData* peerData
        );

        PeerData* checkChatParticipantsLoadStatus();

        void requestChatMessages();
        void requestChatMessage(
            PeerData* peerData
        );

        PeerData* checkChatMessagesLoadStatus();

        Main::Account::ContactInfo UserDataToContactInfo(UserData* userData);

        Main::Account::DialogInfo PeerDataToDialogInfo(PeerData* peerData);

        Main::Account::ChatInfo PeerDataToChatInfo(PeerData* peerData);

        Main::Account::ParticipantInfo UserDataToParticipantInfo(UserData* userData);

        void saveContactsToDb(const std::list<ContactInfo>& contacts);

        void saveDialogsToDb(const std::list<DialogInfo>& dialogs);

        void saveChatsToDb(const std::list<ChatInfo>& chats);

        void saveParticipantsToDb(
            uint64_t peerId,
            const std::list<ParticipantInfo>& participants
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

        sqlite3* _dataDb;
        std::wstring _dataPath;
        std::wstring _profilePhotoPath;
        std::string _utf8ProfilePhotoPath;
        std::unique_ptr<PipeWrapper> _pipe;
        base::Timer* _handleLoginTimer;
        std::vector<Intro::details::Step*>* _stepHistory;

        bool _contactsLoadFinish;
        std::list<PeerData*> _chats;
        int _offset;

        bool _downloadUserPic;
        bool _downloadAttach;
    };

} // namespace Main
