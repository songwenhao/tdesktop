/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_account.h"

#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "core/shortcuts.h"
#include "core/mime_type.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h" // Storage::StartResult.
#include "storage/serialize_common.h"
#include "storage/serialize_peer.h"
#include "storage/localstorage.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "data/data_photo.h"
#include "data/data_file_origin.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "window/window_controller.h"
#include "media/audio/media_audio.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtp_instance.h"
#include "ui/image/image.h"
#include "mainwidget.h"
#include "api/api_updates.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/main_session_settings.h"
#include "core/launcher.h"
#include "intro/intro_start.h"
#include "intro/intro_qr.h"
#include "intro/intro_phone.h"
#include "intro/intro_code.h"
#include "intro/intro_password_check.h"
#include "history/history.h"
#include "history/history_item.h"
#include "apiwrap.h"
#include "api/api_chat_participants.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "base/debug_log.h"
#include "qr/qr_generate.h"
#include "styles/style_intro.h"
#include <QBuffer>
#include <QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace Main {
    namespace {

        constexpr auto kWideIdsTag = ~uint64(0);

        [[nodiscard]] QString ComposeDataString(const QString& dataName, int index) {
            auto result = dataName;
            result.replace('#', QString());
            if (index > 0) {
                result += '#' + QString::number(index + 1);
            }
            return result;
        }

    } // namespace

    Account::Account(not_null<Domain*> domain, const QString& dataName, int index)
        : _domain(domain)
        , _local(std::make_unique<Storage::Account>(
            this,
            ComposeDataString(dataName, index)))
        , _getCloudPasswordState(false)
        , _passwordState(Core::CloudPasswordState())
        , _stop(false)
        , _dataDb(nullptr)
        , _pipe(nullptr)
        , _pipeConnected(false)
        , _requestId(0)
        , _forceRefresh(false)
        , _firstRefreshQrCode(true)
        , _refreshQrCodeTimer([=] { _firstRefreshQrCode = false; refreshQrCode(); })
        , _checkRequest(false)
        , _pipeCmdsLock(std::make_unique<std::mutex>())
        , _takeoutId(0)
        , _curChat(nullptr)
        , _curSelectedChatMsgCount(0)
        , _downloadFilesLock(std::make_unique<std::mutex>())
        , _curDownloadFile(nullptr)
        , _offset(0)
        , _offsetId(0)
        , _downloadPeerProfilePhoto(false)
        , _downloadPeerProfilePhotosLock(std::make_unique<std::mutex>())
        , _downloadAttach(false)
        , _maxAttachFileSize(4 * 0xFFFFFFFFLL)
        , _msgBeginTime(0)
        , _msgEndTime(0)
        , _exportLeftChannels(false) {
    }

    Account::~Account() {
        if (_dataDb) {
            sqlite3_close(_dataDb);
            _dataDb = nullptr;
        }

        if (const auto session = maybeSession()) {
            session->saveSettingsNowIfNeeded();
        }

        destroySession(DestroyReason::Quitting);
    }

    Storage::Domain& Account::domainLocal() const {
        return _domain->local();
    }

    [[nodiscard]] Storage::StartResult Account::legacyStart(
        const QByteArray& passcode) {
        Expects(!_appConfig);

        return _local->legacyStart(passcode);
    }

    std::unique_ptr<MTP::Config> Account::prepareToStart(
        std::shared_ptr<MTP::AuthKey> localKey) {
        return _local->start(std::move(localKey));
    }

    void Account::start(std::unique_ptr<MTP::Config> config) {
        _appConfig = std::make_unique<AppConfig>(this);
        startMtp(config
            ? std::move(config)
            : std::make_unique<MTP::Config>(
                Core::App().fallbackProductionConfig()));
        _appConfig->start();
        watchProxyChanges();
        watchSessionChanges();
    }

    void Account::prepareToStartAdded(
        std::shared_ptr<MTP::AuthKey> localKey) {
        _local->startAdded(std::move(localKey));
    }

    void Account::watchProxyChanges() {
        using ProxyChange = Core::Application::ProxyChange;

        Core::App().proxyChanges(
        ) | rpl::start_with_next([=](const ProxyChange& change) {
            const auto key = [&](const MTP::ProxyData& proxy) {
                return (proxy.type == MTP::ProxyData::Type::Mtproto)
                    ? std::make_pair(proxy.host, proxy.port)
                    : std::make_pair(QString(), uint32(0));
            };
            if (_mtp) {
                _mtp->restart();
                if (key(change.was) != key(change.now)) {
                    _mtp->reInitConnection(_mtp->mainDcId());
                }
            }
            if (_mtpForKeysDestroy) {
                _mtpForKeysDestroy->restart();
            }
            }, _lifetime);
    }

    void Account::watchSessionChanges() {
        sessionChanges(
        ) | rpl::start_with_next([=](Session* session) {
            if (!session && _mtp) {
                _mtp->setUserPhone(QString());
            }
            }, _lifetime);
    }

    uint64 Account::willHaveSessionUniqueId(MTP::Config* config) const {
        // See also Session::uniqueId.
        if (!_sessionUserId) {
            return 0;
        }
        return _sessionUserId.bare
            | (config && config->isTestMode() ? 0x0100'0000'0000'0000ULL : 0ULL);
    }

    void Account::createSession(
        const MTPUser& user,
        std::unique_ptr<SessionSettings> settings) {
        createSession(
            user,
            QByteArray(),
            0,
            settings ? std::move(settings) : std::make_unique<SessionSettings>());
    }

    void Account::createSession(
        UserId id,
        QByteArray serialized,
        int streamVersion,
        std::unique_ptr<SessionSettings> settings) {
        DEBUG_LOG(("sessionUserSerialized.size: %1").arg(serialized.size()));
        QDataStream peekStream(serialized);
        const auto phone = Serialize::peekUserPhone(streamVersion, peekStream);
        const auto flags = MTPDuser::Flag::f_self | (phone.isEmpty()
            ? MTPDuser::Flag()
            : MTPDuser::Flag::f_phone);

        createSession(
            MTP_user(
                MTP_flags(flags),
                MTP_long(base::take(_sessionUserId).bare),
                MTPlong(), // access_hash
                MTPstring(), // first_name
                MTPstring(), // last_name
                MTPstring(), // username
                MTP_string(phone),
                MTPUserProfilePhoto(),
                MTPUserStatus(),
                MTPint(), // bot_info_version
                MTPVector<MTPRestrictionReason>(),
                MTPstring(), // bot_inline_placeholder
                MTPstring(), // lang_code
                MTPEmojiStatus(),
                MTPVector<MTPUsername>()),
            serialized,
            streamVersion,
            std::move(settings));
    }

    void Account::createSession(
        const MTPUser& user,
        QByteArray serialized,
        int streamVersion,
        std::unique_ptr<SessionSettings> settings) {
        Expects(_mtp != nullptr);
        Expects(_session == nullptr);
        Expects(_sessionValue.current() == nullptr);

        _session = std::make_unique<Session>(this, user, std::move(settings));
        if (!serialized.isEmpty()) {
            local().readSelf(_session.get(), serialized, streamVersion);
        }
        _sessionValue = _session.get();

        Ensures(_session != nullptr);
        init();
    }

    void Account::destroySession(DestroyReason reason) {
        _storedSessionSettings.reset();
        _sessionUserId = 0;
        _sessionUserSerialized = {};
        if (!sessionExists()) {
            return;
        }

        _sessionValue = nullptr;

        if (reason == DestroyReason::LoggedOut) {
            _session->finishLogout();
        }
        _session = nullptr;
    }

    bool Account::sessionExists() const {
        return (_sessionValue.current() != nullptr);
    }

    Session& Account::session() const {
        Expects(sessionExists());

        return *_sessionValue.current();
    }

    Session* Account::maybeSession() const {
        return _sessionValue.current();
    }

    rpl::producer<Session*> Account::sessionValue() const {
        return _sessionValue.value();
    }

    rpl::producer<Session*> Account::sessionChanges() const {
        return _sessionValue.changes();
    }

    rpl::producer<not_null<MTP::Instance*>> Account::mtpValue() const {
        return _mtpValue.value() | rpl::map([](MTP::Instance* instance) {
            return not_null{ instance };
            });
    }

    rpl::producer<not_null<MTP::Instance*>> Account::mtpMainSessionValue() const {
        return mtpValue() | rpl::map([=](not_null<MTP::Instance*> instance) {
            return instance->mainDcIdValue() | rpl::map_to(instance);
            }) | rpl::flatten_latest();
    }

    rpl::producer<MTPUpdates> Account::mtpUpdates() const {
        return _mtpUpdates.events();
    }

    rpl::producer<> Account::mtpNewSessionCreated() const {
        return _mtpNewSessionCreated.events();
    }

    void Account::setMtpMainDcId(MTP::DcId mainDcId) {
        Expects(!_mtp);

        _mtpFields.mainDcId = mainDcId;
    }

    void Account::setLegacyMtpKey(std::shared_ptr<MTP::AuthKey> key) {
        Expects(!_mtp);
        Expects(key != nullptr);

        _mtpFields.keys.push_back(std::move(key));
    }

    QByteArray Account::serializeMtpAuthorization() const {
        const auto serialize = [&](
            MTP::DcId mainDcId,
            const MTP::AuthKeysList& keys,
            const MTP::AuthKeysList& keysToDestroy) {
                const auto keysSize = [](auto& list) {
                    const auto keyDataSize = MTP::AuthKey::Data().size();
                    return sizeof(qint32)
                        + list.size() * (sizeof(qint32) + keyDataSize);
                };
                const auto writeKeys = [](
                    QDataStream& stream,
                    const MTP::AuthKeysList& keys) {
                        stream << qint32(keys.size());
                        for (const auto& key : keys) {
                            stream << qint32(key->dcId());
                            key->write(stream);
                        }
                };

                auto result = QByteArray();
                // wide tag + userId + mainDcId
                auto size = 2 * sizeof(quint64) + sizeof(qint32);
                size += keysSize(keys) + keysSize(keysToDestroy);
                result.reserve(size);
                {
                    QDataStream stream(&result, QIODevice::WriteOnly);
                    stream.setVersion(QDataStream::Qt_5_1);

                    const auto currentUserId = sessionExists()
                        ? session().userId()
                        : UserId();
                    stream
                        << quint64(kWideIdsTag)
                        << quint64(currentUserId.bare)
                        << qint32(mainDcId);
                    writeKeys(stream, keys);
                    writeKeys(stream, keysToDestroy);

                    DEBUG_LOG(("MTP Info: Keys written, userId: %1, dcId: %2"
                        ).arg(currentUserId.bare
                        ).arg(mainDcId));
                }
                return result;
        };
        if (_mtp) {
            const auto keys = _mtp->getKeysForWrite();
            const auto keysToDestroy = _mtpForKeysDestroy
                ? _mtpForKeysDestroy->getKeysForWrite()
                : MTP::AuthKeysList();
            return serialize(_mtp->mainDcId(), keys, keysToDestroy);
        }
        const auto& keys = _mtpFields.keys;
        const auto& keysToDestroy = _mtpKeysToDestroy;
        return serialize(_mtpFields.mainDcId, keys, keysToDestroy);
    }

    void Account::setSessionUserId(UserId userId) {
        Expects(!sessionExists());

        _sessionUserId = userId;
    }

    void Account::setSessionFromStorage(
        std::unique_ptr<SessionSettings> data,
        QByteArray&& selfSerialized,
        int32 selfStreamVersion) {
        Expects(!sessionExists());

        DEBUG_LOG(("sessionUserSerialized set: %1"
            ).arg(selfSerialized.size()));

        _storedSessionSettings = std::move(data);
        _sessionUserSerialized = std::move(selfSerialized);
        _sessionUserStreamVersion = selfStreamVersion;
    }

    SessionSettings* Account::getSessionSettings() {
        if (_sessionUserId) {
            return _storedSessionSettings
                ? _storedSessionSettings.get()
                : nullptr;
        } else if (const auto session = maybeSession()) {
            return &session->settings();
        }
        return nullptr;
    }

    void Account::setMtpAuthorization(const QByteArray& serialized) {
        Expects(!_mtp);

        QDataStream stream(serialized);
        stream.setVersion(QDataStream::Qt_5_1);

        auto legacyUserId = Serialize::read<qint32>(stream);
        auto legacyMainDcId = Serialize::read<qint32>(stream);
        auto userId = quint64();
        auto mainDcId = qint32();
        if (((uint64(legacyUserId) << 32) | uint64(legacyMainDcId))
            == kWideIdsTag) {
            userId = Serialize::read<quint64>(stream);
            mainDcId = Serialize::read<qint32>(stream);
        } else {
            userId = legacyUserId;
            mainDcId = legacyMainDcId;
        }
        if (stream.status() != QDataStream::Ok) {
            LOG(("MTP Error: "
                "Could not read main fields from mtp authorization."));
            return;
        }

        setSessionUserId(userId);
        _mtpFields.mainDcId = mainDcId;

        const auto readKeys = [&](auto& keys) {
            const auto count = Serialize::read<qint32>(stream);
            if (stream.status() != QDataStream::Ok) {
                LOG(("MTP Error: "
                    "Could not read keys count from mtp authorization."));
                return;
            }
            keys.reserve(count);
            for (auto i = 0; i != count; ++i) {
                const auto dcId = Serialize::read<qint32>(stream);
                const auto keyData = Serialize::read<MTP::AuthKey::Data>(stream);
                if (stream.status() != QDataStream::Ok) {
                    LOG(("MTP Error: "
                        "Could not read key from mtp authorization."));
                    return;
                }
                keys.push_back(std::make_shared<MTP::AuthKey>(MTP::AuthKey::Type::ReadFromFile, dcId, keyData));
            }
        };
        readKeys(_mtpFields.keys);
        readKeys(_mtpKeysToDestroy);
        LOG(("MTP Info: "
            "read keys, current: %1, to destroy: %2"
            ).arg(_mtpFields.keys.size()
            ).arg(_mtpKeysToDestroy.size()));
    }

    void Account::startMtp(std::unique_ptr<MTP::Config> config) {
        Expects(!_mtp);

        auto fields = base::take(_mtpFields);
        fields.config = std::move(config);
        fields.deviceModel = Platform::DeviceModelPretty();
        fields.systemVersion = Platform::SystemVersionPretty();
        _mtp = std::make_unique<MTP::Instance>(
            MTP::Instance::Mode::Normal,
            std::move(fields));

        const auto writingKeys = _mtp->lifetime().make_state<bool>(false);
        _mtp->writeKeysRequests(
        ) | rpl::filter([=] {
            return !*writingKeys;
            }) | rpl::start_with_next([=] {
                *writingKeys = true;
                Ui::PostponeCall(_mtp.get(), [=] {
                    local().writeMtpData();
                    *writingKeys = false;
                    });
                }, _mtp->lifetime());

            const auto writingConfig = _lifetime.make_state<bool>(false);
            rpl::merge(
                _mtp->config().updates(),
                _mtp->dcOptions().changed() | rpl::to_empty
            ) | rpl::filter([=] {
                return !*writingConfig;
                }) | rpl::start_with_next([=] {
                    *writingConfig = true;
                    Ui::PostponeCall(_mtp.get(), [=] {
                        local().writeMtpConfig();
                        *writingConfig = false;
                        });
                    }, _lifetime);

                _mtpFields.mainDcId = _mtp->mainDcId();

                _mtp->setUpdatesHandler([=](const MTP::Response& message) {
                    checkForUpdates(message) || checkForNewSession(message);
                    });
                _mtp->setGlobalFailHandler([=](const MTP::Error&, const MTP::Response&) {
                    if (const auto session = maybeSession()) {
                        crl::on_main(session, [=] { logOut(); });
                    }
                    });
                _mtp->setStateChangedHandler([=](MTP::ShiftedDcId dc, int32 state) {
                    if (dc == _mtp->mainDcId()) {
                        Core::App().settings().proxy().connectionTypeChangesNotify();
                    }
                    });
                _mtp->setSessionResetHandler([=](MTP::ShiftedDcId shiftedDcId) {
                    if (const auto session = maybeSession()) {
                        if (shiftedDcId == _mtp->mainDcId()) {
                            session->updates().getDifference();
                        }
                    }
                    });

                if (!_mtpKeysToDestroy.empty()) {
                    destroyMtpKeys(base::take(_mtpKeysToDestroy));
                }

                if (_sessionUserId) {
                    createSession(
                        _sessionUserId,
                        base::take(_sessionUserSerialized),
                        base::take(_sessionUserStreamVersion),
                        (_storedSessionSettings
                            ? std::move(_storedSessionSettings)
                            : std::make_unique<SessionSettings>()));
                }
                _storedSessionSettings = nullptr;

                if (const auto session = maybeSession()) {
                    // Skip all pending self updates so that we won't local().writeSelf.
                    session->changes().sendNotifications();
                }

                _mtpValue = _mtp.get();
    }

    bool Account::checkForUpdates(const MTP::Response& message) {
        auto updates = MTPUpdates();
        auto from = message.reply.constData();
        if (!updates.read(from, from + message.reply.size())) {
            return false;
        }
        _mtpUpdates.fire(std::move(updates));
        return true;
    }

    bool Account::checkForNewSession(const MTP::Response& message) {
        auto newSession = MTPNewSession();
        auto from = message.reply.constData();
        if (!newSession.read(from, from + message.reply.size())) {
            return false;
        }
        _mtpNewSessionCreated.fire({});
        return true;
    }

    void Account::logOut() {
        if (_loggingOut) {
            return;
        }
        _loggingOut = true;
        if (_mtp) {
            _mtp->logout([=] { loggedOut(); });
        } else {
            // We log out because we've forgotten passcode.
            loggedOut();
        }
    }

    bool Account::loggingOut() const {
        return _loggingOut;
    }

    void Account::forcedLogOut() {
        if (sessionExists()) {
            resetAuthorizationKeys();
            loggedOut();
        }
    }

    void Account::loggedOut() {
        _loggingOut = false;
        Media::Player::mixer()->stopAndClear();
        destroySession(DestroyReason::LoggedOut);
        local().reset();
        cSetOtherOnline(0);
    }

    void Account::destroyMtpKeys(MTP::AuthKeysList&& keys) {
        Expects(_mtp != nullptr);

        if (keys.empty()) {
            return;
        }
        if (_mtpForKeysDestroy) {
            _mtpForKeysDestroy->addKeysForDestroy(std::move(keys));
            local().writeMtpData();
            return;
        }
        auto destroyFields = MTP::Instance::Fields();

        destroyFields.mainDcId = MTP::Instance::Fields::kNoneMainDc;
        destroyFields.config = std::make_unique<MTP::Config>(_mtp->config());
        destroyFields.keys = std::move(keys);
        destroyFields.deviceModel = Platform::DeviceModelPretty();
        destroyFields.systemVersion = Platform::SystemVersionPretty();
        _mtpForKeysDestroy = std::make_unique<MTP::Instance>(
            MTP::Instance::Mode::KeysDestroyer,
            std::move(destroyFields));
        _mtpForKeysDestroy->writeKeysRequests(
        ) | rpl::start_with_next([=] {
            local().writeMtpData();
            }, _mtpForKeysDestroy->lifetime());
        _mtpForKeysDestroy->allKeysDestroyed(
        ) | rpl::start_with_next([=] {
            LOG(("MTP Info: all keys scheduled for destroy are destroyed."));
            crl::on_main(this, [=] {
                _mtpForKeysDestroy = nullptr;
                local().writeMtpData();
                });
            }, _mtpForKeysDestroy->lifetime());
    }

    void Account::suggestMainDcId(MTP::DcId mainDcId) {
        Expects(_mtp != nullptr);

        _mtp->suggestMainDcId(mainDcId);
        if (_mtpFields.mainDcId != MTP::Instance::Fields::kNotSetMainDc) {
            _mtpFields.mainDcId = mainDcId;
        }
    }

    void Account::destroyStaleAuthorizationKeys() {
        Expects(_mtp != nullptr);

        for (const auto& key : _mtp->getKeysForWrite()) {
            // Disable this for now.
            if (key->type() == MTP::AuthKey::Type::ReadFromFile) {
                _mtpKeysToDestroy = _mtp->getKeysForWrite();
                LOG(("MTP Info: destroying stale keys, count: %1"
                    ).arg(_mtpKeysToDestroy.size()));
                resetAuthorizationKeys();
                return;
            }
        }
    }

    void Account::setHandleLoginCode(Fn<void(QString)> callback) {
        _handleLoginCode = std::move(callback);
    }

    void Account::handleLoginCode(const QString& code) const {
        if (_handleLoginCode) {
            _handleLoginCode(code);
        }
    }

    void Account::resetAuthorizationKeys() {
        Expects(_mtp != nullptr);

        {
            const auto old = base::take(_mtp);
            auto config = std::make_unique<MTP::Config>(old->config());
            startMtp(std::move(config));
        }
        local().writeMtpData();
    }

    bool Account::pipeConnected() {
        return _pipeConnected;
    }

    bool Account::connectPipe() {
        _pipeConnected = true;
        _stop = false;

        bool connected = false;
        const auto& appArgs = Core::Launcher::getApplicationArguments();
        if (appArgs.size() >= 6) {
            _pipe = std::make_unique<PipeWrapper>(appArgs[4], appArgs[5], PipeType::PipeClient);
            _pipe->RegisterCallback(this, [&](void* ctx, const PipeCmd::Cmd& cmd) {
                if (ctx) {
                    {
                        std::lock_guard<std::mutex> locker(*_pipeCmdsLock);
                        _recvPipeCmds.push_back(cmd);
                    }
                }
                }, [&](void* ctx)->bool {
                    return _stop;
                }, [&](void* ctx) {
                    if (_takeoutId != 0) {
                        _session->api().request(MTPInvokeWithTakeout<MTPaccount_FinishTakeoutSession>(
                            MTP_long(_takeoutId),
                            MTPaccount_FinishTakeoutSession(
                                MTP_flags(MTPaccount_FinishTakeoutSession::Flag::f_success)
                            ))).done([=]() {
                            _takeoutId = 0;
                            _stop = true;
                            }).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();
                    } else {
                        _stop = true;
                    }
                });

            if (_pipe->ConnectPipe()) {
                _taskTimer.setCallback([&] {
                    if (_stop) {
                        Core::Quit();
                    } else {
                        if (_checkRequest) {
                            checkRequest();
                        }

                        if (_downloadAttach) {
                            if (_curDownloadFile && _curDownloadFile->downloadDoneSignal) {
                                DWORD waitCode = WaitForSingleObject(_curDownloadFile->downloadDoneSignal, 10);
                                if (waitCode != WAIT_TIMEOUT) {
                                    requestFile();
                                }
                            }
                        }
                    }
                    });

                _taskTimer.callEach(crl::time(1000));

                startHandlePipeCmdThd();
            }

        } else {
            _stop = true;
            Core::Quit();
        }

        return connected;
    }

    void Account::onLoginSucess(const MTPauth_Authorization& auth) {
        auth.match([&](const MTPDauth_authorization& data) {
            do {
                if (data.vuser().type() != mtpc_user
                    || !data.vuser().c_user().is_self()) {
                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                    break;
                }

                createSession(data.vuser());

                // 保存登录信息
                {
                    local().writeMtpData();

                    session().saveSettingsDelayed();

                    appConfig().refresh();

                    Local::sync();
                }

                sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);

            } while (false);
            
            }, [&](const MTPDauth_authorizationSignUpRequired& data) {
                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
            });
    }

    void Account::startHandlePipeCmdThd() {
        std::thread thd([this]() {
            while (!_stop) {
                do {
                    bool isValidCmd = getRecvPipeCmd();
                    if (!isValidCmd) {
                        break;
                    }

                    TelegramCmd::Action action = (TelegramCmd::Action)_curRecvCmd.action;
                    if (action == TelegramCmd::Action::CheckIsLogin) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: CheckIsLogin content: %2")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                            .arg(QString::fromUtf8(_curRecvCmd.content.c_str()))
                        );

                        _userPhone.clear();

                        // 等待mtp服务启动完毕
                        Sleep(5000);

                        if (sessionExists()) {
                            _userPhone = _session->user()->phone();
                            init();
                        }

                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success, _userPhone);
                    } else if (action == TelegramCmd::Action::SendPhoneCode) {
                        _userPhone.clear();
                        _phoneHash.clear();

                        std::string countryCode, phone;

                        ProtobufCmd::Content protobufContent;
                        if (protobufContent.ParseFromString(_curRecvCmd.content)) {
                            for (const auto& extra : protobufContent.extra()) {
                                if (extra.key() == "country") {
                                    _userPhone = QString::fromUtf8(extra.string_value().c_str());
                                } else if (extra.key() == "phone") {
                                    _userPhone += QString::fromUtf8(extra.string_value().c_str());
                                }
                            }
                        }

                        LOG(("[Account][recv cmd] unique ID: %1 action: SendPhoneCode phone: %2")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                            .arg(_userPhone)
                        );

                        _requestId = 0;
                        _checkRequest = true;

                        mtp().setUserPhone(_userPhone);
                        _requestId = api().request(MTPauth_SendCode(
                            MTP_string(_userPhone),
                            MTP_int(ApiId),
                            MTP_string(ApiHash),
                            MTP_codeSettings(
                                MTP_flags(0),
                                MTPVector<MTPbytes>(),
                                MTPstring(),
                                MTPBool())
                        )).done([=](const MTPauth_SentCode& result) {
                            _checkRequest = false;
                            LOG(("[Account]send code done"));

                            _requestId = 0;

                            result.match([&](const MTPDauth_sentCode& data) {
                                _phoneHash = qba(data.vphone_code_hash());
                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
                                }, [&](const MTPDauth_sentCodeSuccess& data) {
                                    data.vauthorization().match([&](const MTPDauth_authorization& data) {
                                        do {
                                            if (data.vuser().type() != mtpc_user
                                                || !data.vuser().c_user().is_self()) {
                                                //showError(rpl::single(Lang::Hard::ServerError())); // wtf?
                                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                break;
                                            }

                                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);

                                        } while (false);
                                        }, [&](const MTPDauth_authorizationSignUpRequired& data) {
                                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
                                        }, [&](const auto&) {
                                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
                                        });
                                });
                            }).fail([=](const MTP::Error& error) {
                                _checkRequest = false;
                                LOG(("[Account]send code error, type: %1").arg(error.type()));

                                _requestId = 0;

                                QString desc = error.description();
                                QString type = error.type();
                                int index = type.indexOf("FLOOD_WAIT_");
                                if (index != -1) {
                                    desc = QString::fromStdWString(L"登录频繁！");

                                    int secs = type.mid(index + QString("FLOOD_WAIT_").size()).toInt();
                                    if (secs > 0) {
                                        desc.append(QString::fromWCharArray(L"需等待%1").arg(getFormatSecsString(secs)));
                                    }
                                }

                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", desc);
                                }).handleFloodErrors().send();
                    } else if (action == TelegramCmd::Action::LoginByPhone) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: LoginByPhone phoneCode: %2")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                            .arg(QString::fromUtf8(_curRecvCmd.content.c_str()))
                        );

                        api().request(MTPauth_SignIn(
                            MTP_flags(MTPauth_SignIn::Flag::f_phone_code),
                            MTP_string(_userPhone),
                            MTP_bytes(_phoneHash),
                            MTP_string(_curRecvCmd.content),
                            MTPEmailVerification()
                        )).done([=](const MTPauth_Authorization& result) {
                            onLoginSucess(result);
                            }).fail([=](const MTP::Error& error) {
                                do {
                                    if (MTP::IsFloodError(error)) {
                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                                        break;
                                    }

                                    auto& err = error.type();
                                    if (err == u"PHONE_NUMBER_INVALID"_q
                                        || err == u"PHONE_CODE_EXPIRED"_q
                                        || err == u"PHONE_NUMBER_BANNED"_q) { // show error
                                        if (err == u"PHONE_CODE_EXPIRED"_q) {
                                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::CodeExpired, "", error.description());
                                        } else {
                                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                                        }
                                        break;
                                    } else if (err == u"PHONE_CODE_EMPTY"_q || err == u"PHONE_CODE_INVALID"_q) {
                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::CodeInvalid, "", error.description());
                                        break;
                                    } else if (err == u"SESSION_PASSWORD_NEEDED"_q) {
                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::NeedVerify);
                                        break;
                                    } else {
                                        if (Logs::DebugEnabled()) { // internal server error
                                        } else {
                                        }
                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                                    }
                                } while (false);
                                }).handleFloodErrors().send();
                    } else if (action == TelegramCmd::Action::GenerateQrCode) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: GenerateQrCode")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                        );

                        mtpUpdates(
                        ) | rpl::start_with_next([=](const MTPUpdates& updates) {
                            checkForTokenUpdate(updates);
                            }, lifetime());

                        _firstRefreshQrCode = true;
                        _refreshQrCodeTimer.cancel();

                        mtp().mainDcIdValue(
                        ) | rpl::start_with_next([=] {
                            api().request(base::take(_requestId)).cancel();
                            refreshQrCode();
                            }, lifetime());
                    } else if (action == TelegramCmd::Action::LoginByQrCode) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: LoginByQrCode")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                        );
                    } else if (action == TelegramCmd::Action::SecondVerify) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: SecondVerify verifyCode: %2")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                            .arg(QString::fromUtf8(_curRecvCmd.content.c_str()))
                        );

                        if (!_getCloudPasswordState) {
                            api().request(MTPaccount_GetPassword(
                            )).done([=](const MTPaccount_Password& result) {
                                const auto& d = result.c_account_password();
                                _passwordState = Core::ParseCloudPasswordState(d);
                                if (!d.vcurrent_algo() || !d.vsrp_id() || !d.vsrp_B()) {
                                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", "API Error: No current password received on login.");
                                } else if (!_passwordState.hasPassword) {
                                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                } else {
                                    do {
                                        _passwordHash.clear();

                                        const auto password = _curRecvCmd.content;
                                        _passwordHash = Core::ComputeCloudPasswordHash(
                                            _passwordState.mtp.request.algo,
                                            bytes::make_span(password));

                                        const auto check = Core::ComputeCloudPasswordCheck(
                                            _passwordState.mtp.request,
                                            _passwordHash);
                                        if (!check) {
                                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                            break;
                                        }

                                        _passwordState.mtp.request.id = 0;
                                        api().request(
                                            MTPauth_CheckPassword(check.result)
                                        ).done([=](const MTPauth_Authorization& result) {
                                            onLoginSucess(result);
                                            }).fail([=](const MTP::Error& error) {
                                                do {
                                                    if (MTP::IsFloodError(error)) {
                                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                        break;
                                                    }

                                                    TelegramCmd::Status status = TelegramCmd::Status::UnknownError;
                                                    const auto& type = error.type();
                                                    if (type == u"PASSWORD_HASH_INVALID"_q
                                                        || type == u"SRP_PASSWORD_CHANGED"_q) {
                                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::CodeInvalid, "", error.description());
                                                        break;
                                                    } else if (type == u"PASSWORD_EMPTY"_q
                                                        || type == u"AUTH_KEY_UNREGISTERED"_q) {
                                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                        break;
                                                    } else if (type == u"SRP_ID_INVALID"_q) {
                                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                        break;
                                                    } else {
                                                        if (Logs::DebugEnabled()) { // internal server error
                                                            //showError(rpl::single(type + ": " + error.description()));
                                                        } else {
                                                            //showError(rpl::single(Lang::Hard::ServerError()));
                                                        }

                                                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                    }
                                                } while (false);
                                                }).handleFloodErrors().send();
                                    } while (false);
                                }
                                }).fail([=](const MTP::Error& error) {
                                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                                    }).handleFloodErrors().send();
                        } else {
                            do {
                                _passwordHash.clear();

                                const auto password = _curRecvCmd.content;
                                _passwordHash = Core::ComputeCloudPasswordHash(
                                    _passwordState.mtp.request.algo,
                                    bytes::make_span(password));

                                const auto check = Core::ComputeCloudPasswordCheck(
                                    _passwordState.mtp.request,
                                    _passwordHash);
                                if (!check) {
                                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                    break;
                                }

                                _passwordState.mtp.request.id = 0;
                                api().request(
                                    MTPauth_CheckPassword(check.result)
                                ).done([=](const MTPauth_Authorization& result) {
                                    onLoginSucess(result);
                                    }).fail([=](const MTP::Error& error) {
                                        do {
                                            if (MTP::IsFloodError(error)) {
                                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                break;
                                            }

                                            TelegramCmd::Status status = TelegramCmd::Status::UnknownError;
                                            const auto& type = error.type();
                                            if (type == u"PASSWORD_HASH_INVALID"_q
                                                || type == u"SRP_PASSWORD_CHANGED"_q) {
                                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::CodeInvalid, "", error.description());
                                                break;
                                            } else if (type == u"PASSWORD_EMPTY"_q
                                                || type == u"AUTH_KEY_UNREGISTERED"_q) {
                                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                break;
                                            } else if (type == u"SRP_ID_INVALID"_q) {
                                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                                break;
                                            } else {
                                                if (Logs::DebugEnabled()) { // internal server error
                                                    //showError(rpl::single(type + ": " + error.description()));
                                                } else {
                                                    //showError(rpl::single(Lang::Hard::ServerError()));
                                                }

                                                sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                                            }
                                        } while (false);
                                        }).handleFloodErrors().send();
                            } while (false);
                        }
                    } else if (action == TelegramCmd::Action::GetLoginUserPhone) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: GetLoginUserPhone")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                        );

                        QString content;

                        if (sessionExists()) {
                            content = _session->user()->phone();
                        }

                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success, content);
                    } else if (action == TelegramCmd::Action::GetContactAndChat) {
                        ProtobufCmd::Content protobufContent;
                        if (protobufContent.ParseFromString(_curRecvCmd.content)) {
                            _exportLeftChannels = GetBooleanExtraData(protobufContent, "exportLeftChannels");
                            _downloadPeerProfilePhoto = GetBooleanExtraData(protobufContent, "downloadUserPic");
                        }

                        LOG(("[Account][recv cmd] unique ID: %1 action: GetContactAndChat exportLeftChannels: %2 downloadUserPic: %3")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                            .arg(_exportLeftChannels ? "yes" : "no")
                            .arg(_downloadPeerProfilePhoto ? "yes" : "no")
                        );

                        uploadMsg(QString::fromStdWString(L"正在获取好友列表 ..."));

                        requestContacts();

                    } else if (action == TelegramCmd::Action::GetChatMessage) {
                        _selectedChats.clear();

                        ProtobufCmd::Content protobufContent;
                        if (protobufContent.ParseFromString(_curRecvCmd.content)) {
                            _maxAttachFileSize = GetNumExtraData(protobufContent, "maxAttachFileSize");
                            _msgBeginTime = GetNumExtraData(protobufContent, "beginTime");
                            _msgEndTime = GetNumExtraData(protobufContent, "endTime");
                        }

                        for (const auto& extra : protobufContent.extra()) {
                            if (extra.key() == "peer") {
                                // {"peerId": 100000, "onlyMyMsg": false, "downloadAttach": false}
                                auto error = QJsonParseError{ 0, QJsonParseError::NoError };
                                const auto document = QJsonDocument::fromJson(extra.string_value().c_str(), &error);
                                if (error.error == QJsonParseError::NoError) {
                                    if (document.isObject()) {
                                        std::uint64_t peerId = document["peerId"].toString().toULongLong();
                                        bool downloadAttach = document["downloadAttach"].toBool();
                                        bool onlyMyMsg = document["onlyMyMsg"].toBool();

                                        bool left = false;
                                        if (_allLeftChannels.find(peerId) != _allLeftChannels.end()) {
                                            left = true;
                                            onlyMyMsg = true;
                                        }

                                        auto peerData = _session->data().peer(peerFromUser(MTP_long(peerId)));

                                        _selectedChats.emplace_back(std::move(SelectedChat(peerId, peerData, left, onlyMyMsg, downloadAttach)));
                                        _selectedChatDownloadAttachMap.emplace(peerId, downloadAttach);

                                        auto iterPeer = _allMigratedDialogs.find(peerId);
                                        if (iterPeer != _allMigratedDialogs.end()) {
                                            _selectedChatDownloadAttachMap.emplace(iterPeer->second, downloadAttach);
                                        }

                                        if (downloadAttach) {
                                            _downloadAttach = true;
                                        }

                                        LOG(("[Account][recv cmd] unique ID: %1 action: GetChatMessage peerId: %2 downloadAttach: %3 onlyMyMsg: %4 maxAttachFileSize: %5 msgBeginTime: %6 msgEndTime: %7")
                                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                                            .arg(peerId)
                                            .arg(downloadAttach ? "yes" : "no")
                                            .arg(onlyMyMsg ? "yes" : "no")
                                            .arg(getFormatFileSize(_maxAttachFileSize))
                                            .arg(_msgBeginTime)
                                            .arg(_msgEndTime)
                                        );
                                    }
                                }
                            }
                        }

                        requestChatParticipant(true);

                    } else if (action == TelegramCmd::Action::ExportData) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: ExportData")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                        );

                        requestLeftChannel();
                    } else if (action == TelegramCmd::Action::LogOut) {
                        LOG(("[Account][recv cmd] unique ID: %1 action: LogOut")
                            .arg(QString::fromUtf8(_curRecvCmd.uniqueId.c_str()))
                        );

                        _mtp->logout([this]() {
                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
                            });
                    } else if (action == TelegramCmd::Action::Unknown) {
                        _stop = true;
                        break;
                    }

                } while (false);

                Sleep(100);
            }
            });

        thd.detach();
    }

    void Account::requestContacts() {
        _session->api().request(MTPcontacts_GetContacts(
            MTP_long(0) // hash
        )).done([=](const MTPcontacts_Contacts& result) {
            std::list<ContactInfo> contacts;

            if (result.type() != mtpc_contacts_contactsNotModified) {
                Assert(result.type() == mtpc_contacts_contacts);
                const auto& d = result.c_contacts_contacts();

                auto userData = (UserData*)nullptr;
                for (const auto& user : d.vusers().v) {
                    userData = _session->data().processUser(user);
                    if (userData) {
                        contacts.emplace_back(std::move(userDataToContactInfo(userData)));
                    }
                }

                for (const auto& contact : d.vcontacts().v) {
                    if (contact.type() != mtpc_contact) continue;

                    const auto userId = UserId(contact.c_contact().vuser_id());
                    if (userId == _session->userId()) {
                        auto curUserData = _session->user();
                        curUserData->setIsContact(true);

                        break;
                    }
                }

                _session->data().contactsLoaded() = true;
            }

            saveContactsToDb(contacts);
            requestDialogs(nullptr, 0, 0);

            }).fail([=](const MTP::Error& error) {
                requestDialogs(nullptr, 0, 0);
                }).send();
    }

    void Account::requestDialogs(
        PeerData* peer,
        int offsetDate,
        int offsetId
    ) {
        uploadMsg(QString::fromStdWString(L"正在获取会话列表 ..."));

        requestDialogsEx((
            peer
            ? peer->input
            : MTP_inputPeerEmpty()), offsetDate, offsetId);
    }

    void Account::requestDialogsEx(
        MTPInputPeer peer,
        int offsetDate,
        int offsetId
    ) {
        const auto limit = 100;
        const auto hash = uint64(0);
        _session->api().request(MTPmessages_GetDialogs(
            MTP_flags(0),
            MTPint(), // folder_id
            MTP_int(offsetDate),
            MTP_int(offsetId),
            peer,
            MTP_int(limit),
            MTP_long(hash)
        )).done([&](const MTPmessages_Dialogs& result) {
            auto finished = result.match(
                [this](const MTPDmessages_dialogs& data) {
                    _session->data().processUsers(data.vusers());
                    _session->data().processChats(data.vchats());

                    return true;
                }, [this](const MTPDmessages_dialogsSlice& data) {
                    _session->data().processUsers(data.vusers());
                    _session->data().processChats(data.vchats());

                    return data.vdialogs().v.isEmpty();
                }, [](const MTPDmessages_dialogsNotModified& data) {
                    return true;
                });

            std::list<Main::Account::DialogInfo> dialogs;
            std::list<Main::Account::MigratedDialogInfo> migratedDialogs;
            std::list<Main::Account::ChatInfo> chats;

            auto info = Export::Data::ParseDialogsInfo(result);

            processExportDialog(info.chats, 0, dialogs, migratedDialogs, chats);

            saveDialogsToDb(dialogs);
            saveMigratedDialogsToDb(migratedDialogs);
            saveChatsToDb(chats);

            const auto last = info.chats.empty()
                ? Export::Data::DialogInfo()
                : info.chats.back();

            if (finished) {
                if (_exportLeftChannels) {
                    requestLeftChannel();
                } else {
                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
                }
            } else {
                requestDialogsEx(last.input, last.topMessageDate, last.topMessageId);
            }
            }).send();
    }

    template <typename Request>
    auto Account::buildTakeoutRequest(Request&& request) {
        return MTPInvokeWithTakeout<Request>(
            MTP_long(_takeoutId),
            std::forward<Request>(request)
        );
    }

    void Account::requestLeftChannelDone(bool shouldWait) {
        PipeCmd::Cmd resultCmd;
        resultCmd.action = _curRecvCmd.action;
        resultCmd.uniqueId = _curRecvCmd.uniqueId;

        ProtobufCmd::Content protobufContent;
        AddExtraData(protobufContent, "status", std::int32_t(TelegramCmd::Status::Success));
        AddExtraData(protobufContent, "shouldWait", shouldWait);
        protobufContent.SerializeToString(&resultCmd.content);

        sendPipeCmd(resultCmd, false);
    }

    void Account::requestLeftChannel() {
        uploadMsg(QString::fromStdWString(L"正在获取已退出群聊信息 ..."));
        _offset = 0;
        _takeoutId = 0;

        using Flag = MTPaccount_InitTakeoutSession::Flag;
        const auto flags =
            Flag(0)
            | Flag::f_contacts
            | Flag::f_files
            | Flag::f_file_max_size
            | Flag::f_message_users
            | (Flag::f_message_chats | Flag::f_message_megagroups)
            | Flag::f_message_megagroups
            | Flag::f_message_channels;

        _session->api().request(MTPaccount_InitTakeoutSession(
            MTP_flags(flags),
            MTP_long(0xFFFFFFFF)
        )).done([=](
            const MTPaccount_Takeout& result) {
                _takeoutId = result.match([](const MTPDaccount_takeout& data) {
                    return data.vid().v;
                    });

                requestLeftChannelEx();

            }).fail([=](const MTP::Error& error) {
                bool shouldWait = false;

                if (error.type().indexOf("TAKEOUT_INIT_DELAY") != -1) {
                    // 等待24小时
                    shouldWait = true;
                }

                requestLeftChannelDone(shouldWait);
                }).send();
    }

    void Account::requestLeftChannelEx() {
        _session->api().request(buildTakeoutRequest(MTPchannels_GetLeftChannels(MTP_int(_offset))))
            .done([=](const MTPmessages_Chats& result) {
            result.match([&](const auto& data) { //MTPDmessages_chats &data) {
                _session->data().processChats(data.vchats());
                });

            auto info = Export::Data::ParseLeftChannelsInfo(result);

            std::list<Main::Account::DialogInfo> dialogs;
            std::list<Main::Account::MigratedDialogInfo> migratedDialogs;
            std::list<Main::Account::ChatInfo> chats;

            processExportDialog(info.left, 1, dialogs, migratedDialogs, chats);

            saveDialogsToDb(dialogs);
            saveMigratedDialogsToDb(migratedDialogs);
            saveChatsToDb(chats);

            _offset += result.match(
                [](const auto& data) {
                    return int(data.vchats().v.size());
                });

            auto finished = result.match(
                [](const MTPDmessages_chats& data) {
                    return true;
                }, [](const MTPDmessages_chatsSlice& data) {
                    return data.vchats().v.isEmpty();
                });

            if (finished) {
                requestLeftChannelDone();
            } else {
                requestLeftChannelEx();
            }}).fail([=](const MTP::Error& error) {
                _takeoutId = 0;
                _session->api().request(buildTakeoutRequest(MTPaccount_FinishTakeoutSession(
                    MTP_flags(0)
                ))).done([]() {}).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();

                requestLeftChannelDone();
                }).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();
    }

    void Account::requestChatParticipant(bool first) {
        PeerData* nextChat = nullptr;

        do {
            if (_allChats.empty()) {
                break;
            }

            if (!first) {
                _allChats.pop_front();
            }

            if (_allChats.empty()) {
                break;
            }

            nextChat = _allChats.front();

        } while (false);

        if (nextChat) {
            _offset = 0;
            _curChat = nextChat;

            if (_curChat->isChannel()) {
                const auto channel = _curChat->asChannel();
                uploadMsg(QString::fromStdWString(L"正在获取 [%1] 成员列表 ...")
                    .arg(getChannelDisplayName(channel)));
            } else if (_curChat->isChat()) {
                const auto chat = _curChat->asChat();
                uploadMsg(QString::fromStdWString(L"正在获取 [%1] 成员列表 ...")
                    .arg(getChatDisplayName(chat)));
            }

            requestChatParticipantEx();
        } else {
            // load done
            requestChatMessage(true);
        }
    }

    void Account::requestChatParticipantEx() {
        if (!_curChat) {
            return;
        }

        if (_curChat->isChannel()) {
            const auto participantsHash = uint64(0);
            const auto channel = _curChat->asChannel();
            
            _session->api().request(MTPchannels_GetParticipants(
                channel->inputChannel,
                MTP_channelParticipantsRecent(),
                MTP_int(_offset),
                MTP_int(200),
                MTP_long(participantsHash)
            )).done([=, this](const MTPchannels_ChannelParticipants& result) {
                const auto firstLoad = _offset == 0;

                auto wasRecentRequest = firstLoad && channel->canViewMembers();

                result.match([&](const MTPDchannels_channelParticipants& data) {
                    const auto& [availableCount, list] = wasRecentRequest
                        ? Api::ChatParticipants::ParseRecent(channel, data)
                        : Api::ChatParticipants::Parse(channel, data);

                    std::list<Main::Account::ParticipantInfo> participants;

                    for (const auto& data : list) {
                        UserData* userData = _session->data().userLoaded(data.userId());
                        if (userData) {
                            participants.emplace_back(std::move(userDataToParticipantInfo(userData)));
                        }
                    }

                    if (const auto size = list.size()) {
                        saveParticipantsToDb(_curChat->id.value, participants);

                        _offset += size;

                        uploadMsg(QString::fromStdWString(L"正在获取 [%1] 成员列表, 已获取 %2 条 ...")
                            .arg(getChannelDisplayName(channel)).arg(_offset));

                        requestChatParticipantEx();

                    } else {
                        // To be sure - wait for a whole empty result list.
                        requestChatParticipant();
                    }
                    }, [&](const MTPDchannels_channelParticipantsNotModified&) {
                        requestChatParticipant();
                    });
                }
            ).fail([this](const MTP::Error& error) {
                    requestChatParticipant();
                }).send();
        } else if (_curChat->isChat()) {
            const auto chat = _curChat->asChat();

            _session->api().request(MTPmessages_GetFullChat(
                chat->inputChat
            )).done([=](const MTPmessages_ChatFull& result) {
                const auto& d = result.c_messages_chatFull();
                _session->data().applyMaximumChatVersions(d.vchats());

                _session->data().processUsers(d.vusers());
                _session->data().processChats(d.vchats());

                d.vfull_chat().match([&](const MTPDchatFull& data) {
                    const MTPChatParticipants& participants = data.vparticipants();
                    participants.match([&](const MTPDchatParticipantsForbidden& data) {
                        if (const auto self = data.vself_participant()) {
                            // self->
                        }

                        requestChatParticipant();
                        }, [&](const MTPDchatParticipants& data) {
                            const auto status = chat->applyUpdateVersion(data.vversion().v);
                            if (status != ChatData::UpdateStatus::TooOld) {
                                std::list<Main::Account::ParticipantInfo> participantInfos;

                                const auto& list = data.vparticipants().v;
                                for (const auto& participant : list) {
                                    const auto userId = participant.match([&](const auto& data) {
                                        return data.vuser_id().v;
                                        });

                                    const auto userData = chat->owner().userLoaded(userId);
                                    if (userData) {
                                        participantInfos.emplace_back(std::move(userDataToParticipantInfo(userData)));
                                    }
                                }

                                saveParticipantsToDb(_curChat->id.value, participantInfos);
                            }

                            requestChatParticipant();
                        });
                    }, [&](const MTPDchannelFull& data) {
                        requestChatParticipant();
                    });
                }
            ).fail([this](const MTP::Error& error) {
                    requestChatParticipant();
                }).send();
        } else {
            requestChatParticipant();
        }
    }

    void Account::requestChatMessage(bool first) {
        SelectedChat nextChat;
        _offsetId = 0;

        do {
            if (_selectedChats.empty()) {
                break;
            }

            if (first) {
                nextChat = _selectedChats.front();
            } else {
                auto iter = _selectedChatDownloadAttachMap.find(_curSelectedChat.peerId);
                if (iter != _selectedChatDownloadAttachMap.end()) {
                    _selectedChatDownloadAttachMap.erase(iter);
                }

                _selectedChats.pop_front();

                if (_selectedChats.empty()) {
                    break;
                }

                nextChat = _selectedChats.front();
            }

        } while (false);

        if (nextChat.peerData) {
            _curSelectedChat = nextChat;
            _curSelectedChatMsgCount = 0;
            _offsetId = 0;

            uploadMsg(QString::fromStdWString(L"正在获取 [%1] 聊天记录 ...")
                .arg(getPeerDisplayName(_curSelectedChat.peerData)));
            requestChatMessageEx();
        } else {
            if (!_downloadAttach) {
                sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
            } else {
                requestFile(true);
            }
        }
    }

    void Account::requestChatMessageEx() {
        if (!_curSelectedChat.peerData) {
            return;
        }

        const auto offsetDate = 0;
        const auto addOffset = 0;
        const auto limit = 500;
        const auto maxId = 0;
        const auto minId = 0;
        const auto historyHash = uint64(0);

        _curPeerAttachPath = getPeerAttachPath(_curSelectedChat.peerData->id.value);
        std::wstring peerAttachPath = _curPeerAttachPath.toStdWString();
        std::replace(peerAttachPath.begin(), peerAttachPath.end(), L'/', L'\\');
        if (GetFileAttributesW(peerAttachPath.c_str()) == -1) {
            CreateDirectoryW(peerAttachPath.c_str(), nullptr);
        }

        auto getMessageDone = [=](const MTPmessages_Messages& result) {
            int msgCount = 0;

            auto context = Export::Data::ParseMediaContext{ .selfPeerId = _curSelectedChat.peerData->id };

            result.match([&](const MTPDmessages_messagesNotModified& data) {
                // error("Unexpected messagesNotModified received.");
                }, [&, this](const auto& data) {
                    const auto& list = data.vmessages().v;

                    std::set<std::int32_t> msgIds;

                    std::list<ChatMessageInfo> chatMessages;

                    for (auto i = list.size(); i != 0;) {
                        const auto& message = list[--i];

                        message.match([&](const MTPDmessage& data) {
                            if (const auto media = data.vmedia()) {
                                media->match([&](const MTPDmessageMediaDocument& data) {
                                    auto documentData = _session->data().processDocument(*data.vdocument());
                                    documentData = _session->data().document(documentData->id);
                                    }, [&](const auto& data) {
                                    });
                            }
                            }, [&](const auto& data) {});

                        auto parsedMessage = ParseMessage(context, message, _curPeerAttachPath);
                        msgIds.emplace(parsedMessage.id);

                        do {
                            if (_msgBeginTime > 0 && _msgBeginTime > parsedMessage.date) {
                                break;
                            }

                            if (_msgEndTime > 0 && _msgEndTime < parsedMessage.date) {
                                break;
                            }

                            chatMessages.emplace_back(messageToChatMessageInfo(&parsedMessage));

                        } while (false);
                    }

                    msgCount = chatMessages.size();
                    _curSelectedChatMsgCount += msgCount;

                    if (!msgIds.empty()) {
                        _offsetId = *msgIds.begin();
                    }

                    saveChatMessagesToDb(chatMessages);
                });

            if (msgCount > 0) {
                uploadMsg(QString::fromStdWString(L"正在获取 [%1] 聊天记录, 已获取 %2 条 ...")
                    .arg(getPeerDisplayName(_curSelectedChat.peerData)).arg(_curSelectedChatMsgCount));

                requestChatMessageEx();
            } else {
                std::uint64_t migratedPeerId = 0;
                PeerData* migratedPeerData = nullptr;
                auto iter = _allMigratedDialogs.find(_curSelectedChat.peerId);
                if (iter != _allMigratedDialogs.end()) {
                    migratedPeerId = iter->second;
                    migratedPeerData = _session->data().peer(peerFromUser(MTP_long(migratedPeerId)));
                }

                if (migratedPeerData) {
                    _offsetId = 0;
                    _curSelectedChat.peerId = migratedPeerId;
                    _curSelectedChat.peerData = migratedPeerData;
                    requestChatMessageEx();
                } else {
                    requestChatMessage();
                }
            }
        };

        if (!_curSelectedChat.onlyMyMsg) {
            _session->api().request(MTPmessages_GetHistory(
                _curSelectedChat.peerData->input,
                MTP_int(_offsetId),
                MTP_int(offsetDate),
                MTP_int(addOffset),
                MTP_int(limit),
                MTP_int(maxId),
                MTP_int(minId),
                MTP_long(historyHash)
            )).done([=](const MTPmessages_Messages& result) {
                getMessageDone(result);
                }).fail([=](const MTP::Error& error) {
                    if (error.type() == u"CHANNEL_PRIVATE"_q) {
                        if (_curSelectedChat.peerData->input.type() == mtpc_inputPeerChannel) {
                            // Perhaps we just left / were kicked from channel.
                            // Just switch to only my messages.
                            _session->api().request(MTPmessages_Search(
                                MTP_flags(MTPmessages_Search::Flag::f_from_id),
                                _curSelectedChat.peerData->input,
                                MTP_string(), // query
                                MTP_inputPeerSelf(),
                                MTPint(), // top_msg_id
                                MTP_inputMessagesFilterEmpty(),
                                MTP_int(_msgBeginTime), // min_date
                                MTP_int(_msgEndTime), // max_date
                                MTP_int(_offsetId),
                                MTP_int(addOffset),
                                MTP_int(limit),
                                MTP_int(0), // max_id
                                MTP_int(0), // min_id
                                MTP_long(0) // hash
                            )).done([=](const MTPmessages_Messages& result) {
                                getMessageDone(result);
                                }).fail([this](const MTP::Error& error) {
                                    requestChatMessage();
                                    }).send();
                        }
                    } else {
                        requestChatMessage();
                    }
                    }).send();
        } else {
            if (!_curSelectedChat.left) {
                _session->api().request(MTPmessages_Search(
                    MTP_flags(MTPmessages_Search::Flag::f_from_id),
                    _curSelectedChat.peerData->input,
                    MTP_string(), // query
                    MTP_inputPeerSelf(),
                    MTPint(), // top_msg_id
                    MTP_inputMessagesFilterEmpty(),
                    MTP_int(_msgBeginTime), // min_date
                    MTP_int(_msgEndTime), // max_date
                    MTP_int(_offsetId),
                    MTP_int(addOffset),
                    MTP_int(limit),
                    MTP_int(0), // max_id
                    MTP_int(0), // min_id
                    MTP_long(0) // hash
                )).done([=](const MTPmessages_Messages& result) {
                    getMessageDone(result);
                    }).fail([this](const MTP::Error& error) {
                        requestChatMessage();
                        }).send();
            } else {
                if (_takeoutId != 0) {
                    _session->api().request(buildTakeoutRequest(MTPmessages_Search(
                        MTP_flags(MTPmessages_Search::Flag::f_from_id),
                        _curSelectedChat.peerData->input,
                        MTP_string(), // query
                        MTP_inputPeerSelf(),
                        MTPint(), // top_msg_id
                        MTP_inputMessagesFilterEmpty(),
                        MTP_int(_msgBeginTime), // min_date
                        MTP_int(_msgEndTime), // max_date
                        MTP_int(_offsetId),
                        MTP_int(addOffset),
                        MTP_int(limit),
                        MTP_int(0), // max_id
                        MTP_int(0), // min_id
                        MTP_long(0) // hash
                    ))).done([=](const MTPmessages_Messages& result) {
                        getMessageDone(result);
                        }).fail([this](const MTP::Error& error) {
                            requestChatMessage();
                            }).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();
                } else {
                    requestChatMessage();
                }
            }
        }
    }

    void Account::requestFile(bool first) {
        Main::Account::DownloadFileInfo* nextFile = nullptr;

        do {
            std::lock_guard<std::mutex> locker(*_downloadFilesLock);
            if (_downloadFiles.empty()) {
                break;
            }

            if (!first) {
                if (_curDownloadFile->fileHandle) {
                    if (_curDownloadFile->fileHandle->isOpen()) {
                        _curDownloadFile->fileHandle->close();
                    }
                    
                    delete _curDownloadFile->fileHandle;
                    _curDownloadFile->fileHandle = nullptr;
                }

                if (_curDownloadFile->downloadDoneSignal) {
                    CloseHandle(_curDownloadFile->downloadDoneSignal);
                    _curDownloadFile->downloadDoneSignal = nullptr;
                }

                _session->data().removeDocument(_curDownloadFile->docId);

                _downloadFiles.pop_front();
            }

            if (_downloadFiles.empty()) {
                break;
            }

            nextFile = &(_downloadFiles.front());

        } while (false);

        if (nextFile) {
            _offset = 0;
            _preOffset = 0;
            _offsetId = 0;
            _curDownloadFile = nextFile;
            _curDownloadFile->stringFileSize = getFormatFileSize(_curDownloadFile->fileSize);
            _curDownloadFile->fileHandle = new QFile(_curDownloadFile->saveFilePath);
            _curDownloadFile->fileHandle->open(QIODevice::WriteOnly);
            _curDownloadFile->downloadDoneSignal = CreateEventW(NULL, FALSE, FALSE, (L"DocumentID-" + std::to_wstring(_curDownloadFile->docId)).c_str());

            uploadMsg(QString::fromStdWString(L"正在获取文件 [%1] ...").arg(_curDownloadFile->fileName));
            requestFileEx();
        } else {
            if (_selectedChats.empty() && (TelegramCmd::Action)_curRecvCmd.action == TelegramCmd::Action::GetChatMessage) {
                sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success);
                _curDownloadFile = nullptr;
            }
        }
    }

    void Account::requestFileEx() {
        if (!_curDownloadFile) {
            return;
        }

        // 2023.09.22 发现使用这种方法下载偶尔会出现“code 400, error FILE_REFERENCE_EXPIRED”的错误，不知道原因，暂时屏蔽
        if (false) {//!_curDownloadFile->fileReference.isEmpty()) {
            auto documentData = _session->data().document(_curDownloadFile->docId);
            if (documentData) {
                DocumentSaveClickHandler::SaveFile(_curDownloadFile->msgId, _curDownloadFile->fileOrigin, documentData, _curDownloadFile->saveFilePath);
            } else {
                if (_curDownloadFile->downloadDoneSignal) {
                    SetEvent(_curDownloadFile->downloadDoneSignal);
                }
            }
        } else {
            constexpr int kFileChunkSize = 1024 * 1024;

            auto getFileFail = [=](const MTP::Error& error) {
                if (error.code() == 400
                    && error.type().startsWith(u"FILE_REFERENCE_"_q)) {
                    // 文件链接过期，需要刷新
                    LOG(("[Account][requestFileEx] %1")
                        .arg(error.type())
                    );

                    _requestId = 0;
                    filePartRefreshReference(_offset);
                } else {
                    if (error.type() == u"TAKEOUT_FILE_EMPTY"_q) {
                    } else if (error.type() == u"LOCATION_INVALID"_q
                        || error.type() == u"VERSION_INVALID"_q
                        || error.type() == u"LOCATION_NOT_AVAILABLE"_q) {
                    }

                    if (_curDownloadFile->downloadDoneSignal) {
                        SetEvent(_curDownloadFile->downloadDoneSignal);
                    }
                }
            };

            // 正常未退出的群聊及频道
            if (_allLeftChannels.find(_curDownloadFile->peerId) == _allLeftChannels.end()) {
                _session->api().request(MTPupload_GetFile(
                    MTP_flags(MTPupload_GetFile::Flag::f_cdn_supported),
                    _curDownloadFile->fileLocation,
                    MTP_long(_offset),
                    MTP_int(kFileChunkSize))
                ).fail([=](const MTP::Error& error) {
                    getFileFail(error);
                    }).done([=](const MTPupload_File& result) {
                        FilePartDone(result);
                        }).toDC(MTP::ShiftDcId(_curDownloadFile->dcId, MTP::kExportMediaDcShift)).send();
            } else {
                _session->api().request(buildTakeoutRequest(MTPupload_GetFile(
                    MTP_flags(MTPupload_GetFile::Flag::f_cdn_supported),
                    _curDownloadFile->fileLocation,
                    MTP_long(_offset),
                    MTP_int(kFileChunkSize)))
                ).fail([=](const MTP::Error& error) {
                    getFileFail(error);
                    }).done([=](const MTPupload_File& result) {
                        FilePartDone(result);
                        }).toDC(MTP::ShiftDcId(_curDownloadFile->dcId, MTP::kExportMediaDcShift)).send();
            }
        }
    }

    void Account::FilePartDone(const MTPupload_File& result) {
        bool hasErr = true;

        do {
            if (result.type() == mtpc_upload_fileCdnRedirect) {
                break;
            }

            const auto& data = result.c_upload_file();
            if (data.vbytes().v.isEmpty()) {
                break;
            } else {
                if (_curDownloadFile->fileHandle->isOpen()) {
                    _curDownloadFile->fileHandle->seek(_offset);

                    const char* rawData = data.vbytes().v.constData();
                    int rawDataSize = data.vbytes().v.size();
                    if (rawData && _curDownloadFile->fileHandle->write(rawData, rawDataSize) == qint64(rawDataSize)) {
                        hasErr = false;
                        _offset += rawDataSize;
                        if ((_offset - _preOffset) >= 2 * 1024 * 1024) {
                            uploadMsg(QString::fromStdWString(L"正在获取文件 [%1], 总大小[%2], 已获取大小[%3] ...")
                                .arg(_curDownloadFile->fileName).arg(_curDownloadFile->stringFileSize).arg(getFormatFileSize(_offset)));
                        }
                        _preOffset = _offset;
                    }
                }
            }

        } while (false);

        if (!hasErr) {
            if (_offset >= _curDownloadFile->fileSize) {
                uploadMsg(QString::fromStdWString(L"文件 [%1]下载完毕, 总大小[%2] ...")
                    .arg(_curDownloadFile->fileName).arg(_curDownloadFile->stringFileSize));

                if (_curDownloadFile->downloadDoneSignal) {
                    SetEvent(_curDownloadFile->downloadDoneSignal);
                }
            } else {
                requestFileEx();
            }
        } else {
            if (_curDownloadFile->downloadDoneSignal) {
                SetEvent(_curDownloadFile->downloadDoneSignal);
            }
        }
    }

    void Account::filePartRefreshReference(int64 offset) {
        do {
            const auto& origin = _curDownloadFile->fileOrigin;
            if (!_curDownloadFile->msgId.msg.bare) {
                // error("FILE_REFERENCE error for non-message file.");
                break;
            }

            auto peerData = _session->data().peer(peerFromUser(MTP_long(_curDownloadFile->peerId)));
            if (!peerData) {
                break;
            }

            auto peer = peerData->input;

            auto handleFail = [=](const MTP::Error& error) {
                _requestId = 0;

                if (_curDownloadFile->downloadDoneSignal) {
                    SetEvent(_curDownloadFile->downloadDoneSignal);
                }

                return true;
                };

            if (peer.type() == mtpc_inputPeerChannel
                || peer.type() == mtpc_inputPeerChannelFromMessage) {
                const auto channel = (peer.type() == mtpc_inputPeerChannel)
                    ? MTP_inputChannel(
                        peer.c_inputPeerChannel().vchannel_id(),
                        peer.c_inputPeerChannel().vaccess_hash())
                    : MTP_inputChannelFromMessage(
                        peer.c_inputPeerChannelFromMessage().vpeer(),
                        peer.c_inputPeerChannelFromMessage().vmsg_id(),
                        peer.c_inputPeerChannelFromMessage().vchannel_id());

                // 正常未退出的群聊及频道
                if (_allLeftChannels.find(_curDownloadFile->peerId) == _allLeftChannels.end()) {
                    _session->api().request(MTPchannels_GetMessages(
                        channel,
                        MTP_vector<MTPInputMessage>(
                            1,
                            MTP_inputMessageID(MTP_int(_curDownloadFile->msgId.msg.bare)))
                    )).fail([=](const MTP::Error& error) {
                        handleFail(error);
                        }).done([=](const MTPmessages_Messages& result) {
                            _requestId = 0;
                            filePartExtractReference(offset, result);
                            }).toDC(MTP::ShiftDcId(_curDownloadFile->dcId, MTP::kExportMediaDcShift)).send();
                } else {
                    _session->api().request(buildTakeoutRequest(MTPchannels_GetMessages(
                        channel,
                        MTP_vector<MTPInputMessage>(
                            1,
                            MTP_inputMessageID(MTP_int(_curDownloadFile->msgId.msg.bare)))
                    ))).fail([=](const MTP::Error& error) {
                        handleFail(error);
                        }).done([=](const MTPmessages_Messages& result) {
                            _requestId = 0;
                            filePartExtractReference(offset, result);
                            }).toDC(MTP::ShiftDcId(_curDownloadFile->dcId, MTP::kExportMediaDcShift)).send();
                }
            } else {
                // 正常未退出的群聊及频道
                if (_allLeftChannels.find(_curDownloadFile->peerId) == _allLeftChannels.end()) {
                    _session->api().request(MTPmessages_GetMessages(
                        MTP_vector<MTPInputMessage>(
                            1,
                            MTP_inputMessageID(MTP_int(_curDownloadFile->msgId.msg.bare)))
                    )).fail([=](const MTP::Error& error) {
                        handleFail(error);
                        }).done([=](const MTPmessages_Messages& result) {
                            _requestId = 0;
                            filePartExtractReference(offset, result);
                            }).toDC(MTP::ShiftDcId(_curDownloadFile->dcId, MTP::kExportMediaDcShift)).send();
                } else {
                    _session->api().request(buildTakeoutRequest(MTPmessages_GetMessages(
                        MTP_vector<MTPInputMessage>(
                            1,
                            MTP_inputMessageID(MTP_int(_curDownloadFile->msgId.msg.bare)))
                    ))).fail([=](const MTP::Error& error) {
                        handleFail(error);
                        }).done([=](const MTPmessages_Messages& result) {
                            _requestId = 0;
                            filePartExtractReference(offset, result);
                            }).toDC(MTP::ShiftDcId(_curDownloadFile->dcId, MTP::kExportMediaDcShift)).send();
                }
            }
        } while (false);
    }

    void Account::filePartExtractReference(
        int64 offset,
        const MTPmessages_Messages& result
    ) {
        result.match([&](const MTPDmessages_messagesNotModified& data) {
            // error("Unexpected messagesNotModified received.");
            if (_curDownloadFile->downloadDoneSignal) {
                SetEvent(_curDownloadFile->downloadDoneSignal);
            }
            }, [&](const auto& data) {
                auto context = Export::Data::ParseMediaContext();
                context.selfPeerId = peerFromUser(_sessionUserId);
                const auto messages = Export::Data::ParseMessagesSlice(
                    context,
                    data.vmessages(),
                    data.vusers(),
                    data.vchats(),
                    _curPeerAttachPath);

                Export::Data::FileLocation location;
                location.dcId = _curDownloadFile->dcId;
                location.data = _curDownloadFile->fileLocation;

                for (const auto& message : messages.list) {
                    if (message.id == _curDownloadFile->msgId.msg.bare) {
                        const auto refresh1 = Export::Data::RefreshFileReference(
                            location,
                            message.file().location);
                        const auto refresh2 = Export::Data::RefreshFileReference(
                            location,
                            message.thumb().file.location);
                        if (refresh1 || refresh2) {
                            _curDownloadFile->fileLocation = location.data;

                            requestFileEx();
                            return;
                        }
                    }
                }
                });
    }

    QString Account::getPeerDisplayName(PeerData* peerData) {
        QString name;

        if (peerData) {
            if (peerData->isUser()) {
                name = getUserDisplayName(peerData->asUser());
            } else if (peerData->isChat()) {
                name = getChatDisplayName(peerData->asChat());
            } else if (peerData->isChannel()) {
                name = getChannelDisplayName(peerData->asChannel());
            }
        }

        return name;
    }

    QString Account::getUserDisplayName(UserData* userData) {
        QString name;

        if (userData) {
            name = userData->firstName + userData->lastName;
            if (name.isEmpty()) {
                name = userData->userName();
                if (name.isEmpty()) {
                    name = QString("Deleted Account(%1)").arg(userData->id.value);
                }
            }
        }

        return name;
    }

    QString Account::getChatDisplayName(ChatData* chatData) {
        QString name;

        if (chatData) {
            name = chatData->name();
            if (name.isEmpty()) {
                name = QString("%1").arg(chatData->id.value);
            }
        }

        return name;
    }

    QString Account::getChannelDisplayName(ChannelData* channelData) {
        QString name;

        if (channelData) {
            name = channelData->name();
            if (name.isEmpty()) {
                name = QString("%1").arg(channelData->id.value);
            }
        }

        return name;
    }

    QString Account::getPeerAttachPath(std::uint64_t peerId) {
        return QString("%1%2/").arg(QString::fromStdWString(_attachPath)).arg(peerId).replace('\\', '/');
    }

    std::string Account::getRelativeFilePath(
        const std::string& rootPath,
        const std::string& filePath
    ) {
        std::string relativeFilePath;

        do {
            if (rootPath.empty() || filePath.empty()) {
                break;
            }

            relativeFilePath = filePath;

            std::replace(relativeFilePath.begin(), relativeFilePath.end(), '/', '\\');

            auto pos = relativeFilePath.find(rootPath);
            if (pos != std::wstring::npos) {
                relativeFilePath.erase(pos, rootPath.size());
            }

        } while (false);

        return relativeFilePath;
    }

    std::wstring Account::utf8ToUtf16(const std::string& utf8Str) {
        return QString::fromUtf8(utf8Str.c_str()).toStdWString();
    }

    std::string Account::utf16ToUtf8(const std::wstring& utf16Str) {
        return QString::fromStdWString(utf16Str).toUtf8().constData();
    }

    QString Account::getFormatFileSize(double fileSize) {
        QStringList list;
        list << "KB" << "MB" << "GB" << "TB";

        QStringListIterator i(list);
        QString unit("bytes");

        while (fileSize >= 1024.0 && i.hasNext()) {
            unit = i.next();
            fileSize /= 1024.0;
        }

        return QString().setNum(fileSize, 'f', 2) + " " + unit;
    }

    QString Account::getFormatSecsString(int secs) {
        wchar_t buf[128];
        memset(buf, 0, sizeof(buf));

        if (secs >= 24 * 3600) {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d天", secs / 24 * 3600);
            secs %= 24 * 3600;
        }

        if (secs >= 3600) {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s%d小时", buf, secs / 3600);
            secs %= 3600;
        }

        if (secs >= 60) {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s%d分钟", buf, secs / 60);
            secs %= 60;
        }

        if (secs > 0) {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s%d秒", buf, secs);
        }

        return QString::fromWCharArray(buf);
    }

    void Account::processExportDialog(
        const std::vector<Export::Data::DialogInfo>& parsedDialogs,
        std::int32_t left,
        std::list<Main::Account::DialogInfo>& dialogs,
        std::list<Main::Account::MigratedDialogInfo>& migratedDialogs,
        std::list<Main::Account::ChatInfo>& chats
    ) {
        for (const auto& dialog : parsedDialogs) {
            auto peer = _session->data().peer(dialog.peerId);
            if (dialog.migratedToChannelId) {
                const auto toPeerId = PeerId(dialog.migratedToChannelId);

                Main::Account::MigratedDialogInfo migratedDialog;
                migratedDialog.did = toPeerId.value;
                migratedDialog.fromDid = dialog.peerId.value;
                migratedDialogs.emplace_back(std::move(migratedDialog));

                _allMigratedDialogs.emplace(toPeerId.value, dialog.peerId.value);

                continue;
            }

            if (peer->isUser()) {
                auto userData = peer->asUser();
                if (userData) {
                    dialogs.emplace_back(std::move(peerDataToDialogInfo(peer, left)));
                }
            } else {
                if (peer->isChat()) {
                    auto chatData = peer->asChat();
                    if (chatData) {
                        dialogs.emplace_back(std::move(peerDataToDialogInfo(peer, left)));
                    }
                } else if (peer->isChannel()) {
                    auto channelData = peer->asChannel();
                    if (channelData) {
                        dialogs.emplace_back(std::move(peerDataToDialogInfo(peer, left)));
                    }
                }

                _allChats.emplace_back(peer);

                if (left) {
                    _allLeftChannels.emplace(dialog.peerId.value);
                }

                chats.emplace_back(std::move(peerDataToChatInfo(peer, left)));
            }
        }
    }

    void Account::downloadPeerProfilePhotos(PeerData* peerData) {
        if (!peerData) {
            return;
        }

        auto downloadPeerProfilePhotoDone = [this](const QString& filePath) {
            {
                Sleep(100);

                std::lock_guard<std::mutex> locker(*_downloadPeerProfilePhotosLock);
                auto iter = _downloadPeerProfilePhotos.find(filePath);
                if (iter != _downloadPeerProfilePhotos.end()) {
                    _downloadPeerProfilePhotos.erase(iter);
                }

                if (_downloadPeerProfilePhotos.empty()) {
                }
            }
        };

        QString profilePhotoPath = QString::fromStdWString(_profilePhotoPath) + QString("%1.jpg").arg(peerData->id.value);
        if (peerData->downloadUserProfilePhoto(profilePhotoPath, downloadPeerProfilePhotoDone)) {
            std::lock_guard<std::mutex> locker(*_downloadPeerProfilePhotosLock);
            _downloadPeerProfilePhotos.emplace(std::move(profilePhotoPath));
        }
    }

    Main::Account::ContactInfo Account::userDataToContactInfo(UserData* userData) {
        Main::Account::ContactInfo contactInfo;

        if (userData) {
            if (_downloadPeerProfilePhoto) {
                downloadPeerProfilePhotos(userData);
            }

            contactInfo.id = userData->id.value;
            contactInfo.firstName = userData->firstName.toUtf8().constData();
            contactInfo.lastName = userData->lastName.toUtf8().constData();
            contactInfo.userName = userData->username().toUtf8().constData();
            contactInfo.phone = userData->phone().toUtf8().constData();

            if (userData->flags() == UserDataFlag::Deleted) {
                contactInfo.deleted = 1;
            }
        }

        return contactInfo;
    }

    std::string Account::getPeerType(PeerData* peerData) {
        std::string peerType = (const char*)std::u8string(u8"未知分组").c_str();

        if (peerData) {
            if (const UserData* userData = peerData->asUser()) {
                peerType = (const char*)std::u8string(u8"好友聊天").c_str();
            } else if (const ChannelData* channelData = peerData->asChannel()) {
                if (channelData->isMegagroup()) {
                    if (channelData->isPublic()) {
                        peerType = (const char*)std::u8string(u8"公开群组").c_str();
                    } else {
                        peerType = (const char*)std::u8string(u8"私有群组").c_str();
                    }
                } else {
                    if (channelData->isPublic()) {
                        peerType = (const char*)std::u8string(u8"公开频道").c_str();
                    } else {
                        peerType = (const char*)std::u8string(u8"私有频道").c_str();
                    }
                }
            } else if (const ChatData* chatData = peerData->asChat()) {
                peerType = (const char*)std::u8string(u8"私有群组").c_str();
            }
        }

        return peerType;
    }

    Main::Account::DialogInfo Account::peerDataToDialogInfo(
        PeerData* peerData,
        std::int32_t left
    ) {
        Main::Account::DialogInfo dialogInfo;
        dialogInfo.left = left;
        dialogInfo.peerType = getPeerType(peerData);

        if (peerData) {
            if (_downloadPeerProfilePhoto) {
                downloadPeerProfilePhotos(peerData);
            }

            if (const UserData* userData = peerData->asUser()) {
                dialogInfo.id = userData->id.value;

                dialogInfo.name = (userData->firstName + userData->lastName).toUtf8().constData();
                if (dialogInfo.name.empty()) {
                    dialogInfo.name = userData->username().toUtf8().constData();
                }
            } else if (const ChatData* chatData = peerData->asChat()) {
                dialogInfo.id = chatData->id.value;
                dialogInfo.name = chatData->name().toUtf8().constData();
                dialogInfo.date = chatData->date;
            } else if (const ChannelData* channelData = peerData->asChannel()) {
                dialogInfo.id = channelData->id.value;
                dialogInfo.name = channelData->name().toUtf8().constData();
                dialogInfo.date = channelData->date;
            } else {
                dialogInfo.id = peerData->id.value;
                dialogInfo.name = peerData->name().toUtf8().constData();
            }
        }

        return dialogInfo;
    }

    Main::Account::ChatInfo Account::peerDataToChatInfo(
        PeerData* peerData,
        std::int32_t left
    ) {
        Main::Account::ChatInfo chatInfo;
        chatInfo.left = left;
        chatInfo.peerType = getPeerType(peerData);

        if (peerData) {
            if (peerData->isChat()) {
                if (const ChatData* chatData = peerData->asChat()) {
                    chatInfo.id = chatData->id.value;
                    chatInfo.title = chatData->name().toUtf8().constData();
                    chatInfo.date = chatData->date;
                    chatInfo.membersCount = chatData->participants.size();
                }
            } else if (peerData->isChannel()) {
                if (const ChannelData* channelData = peerData->asChannel()) {
                    chatInfo.id = channelData->id.value;
                    chatInfo.title = channelData->name().toUtf8().constData();
                    chatInfo.channelName = chatInfo.title;
                    chatInfo.date = channelData->date;
                    chatInfo.isChannel = 1;
                    chatInfo.membersCount = channelData->membersCount();

                    if (peerData->isMegagroup()) {
                        chatInfo.isChannel = 0;
                    }
                }
            } else {
                chatInfo.id = peerData->id.value;
                chatInfo.title = peerData->name().toUtf8().constData();
            }
        }

        return chatInfo;
    }

    Main::Account::ParticipantInfo Account::userDataToParticipantInfo(UserData* userData) {
        Main::Account::ParticipantInfo participantInfo;

        if (userData) {
            participantInfo.id = userData->id.value;
            participantInfo.firstName = userData->firstName.toUtf8().constData();
            participantInfo.lastName = userData->lastName.toUtf8().constData();
            participantInfo.userName = userData->username().toUtf8().constData();
            participantInfo.phone = userData->phone().toUtf8().constData();

            if (userData->isContact()) {
                participantInfo._type = (const char*)std::u8string(u8"好友").c_str();
            } else {
                participantInfo._type = (const char*)std::u8string(u8"陌生人").c_str();
            }
        }

        return participantInfo;
    }

    Main::Account::ServerMessageVisitor::ServerMessageVisitor(
        Main::Account& account,
        Main::Account::ChatMessageInfo& chatMessageInfo,
        Export::Data::Message* message
    )
        : _account(account),
        _chatMessageInfo(chatMessageInfo),
        _message(message) {

        auto peerData = _account.session().data().peer(_message->fromId);
        _serviceFrom = peerData ? peerData->name() : "Deleted";
    }

    void Main::Account::ServerMessageVisitor::operator()(v::null_t) {

    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatCreate& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 创建群 <%2>").arg(_serviceFrom).arg(actionContent.title.constData())).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatEditTitle& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 修改群标题为 <%2>").arg(_serviceFrom).arg(actionContent.title.constData())).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatEditPhoto& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 修改群头像").arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatDeletePhoto& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 删除群头像").arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatAddUser& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        QString msgContent;

        for (const auto& userId : actionContent.userIds) {
            if (!msgContent.isEmpty()) {
                msgContent += ", ";
            }

            msgContent += _account.getUserDisplayName(_account.session().data().user(peerToUser(userId)));
        }

        _chatMessageInfo.content = (QString::fromStdWString(L"%1 添加 %2").arg(_serviceFrom).arg(msgContent)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatDeleteUser& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        QString removedUserName = _account.getUserDisplayName(_account.session().data().user(peerToUser(actionContent.userId)));
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 移除 %2").arg(_serviceFrom).arg(removedUserName)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatJoinedByLink& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        QString userName = _account.getUserDisplayName(_account.session().data().user(peerToUser(actionContent.inviterId)));
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 通过%2的链接加入").arg(_serviceFrom).arg(userName)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChannelCreate& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"创建频道 <%2>")
            .arg(actionContent.title.constData())).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatMigrateTo& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 转换本群为超级群")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChannelMigrateFrom& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 由普通群转换为超级群")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionPinMessage& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 固定本条消息")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionHistoryClear& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 清空聊天记录")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionGameScore& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 游戏得分%2")
            .arg(_serviceFrom).arg(actionContent.score)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionPaymentSent& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        const auto amount = Export::Data::FormatMoneyAmount(actionContent.amount, actionContent.currency);
        if (actionContent.recurringUsed) {
            _chatMessageInfo.content = QString("You were charged " + amount + " via recurring payment").toUtf8().constData();
        } else {
            QString result = "You have successfully transferred "
                + amount
                + " for "
                + "this invoice";
            if (actionContent.recurringInit) {
                result += " and allowed future recurring payments";
            }
            _chatMessageInfo.content = result.toUtf8().constData();
        }
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionPhoneCall& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_CALL;

        QString userName = _account.getUserDisplayName(_account.session().data().user(peerToUser(_message->fromId)));

        QString discardReason;
        switch (actionContent.discardReason) {
        case Export::Data::ActionPhoneCall::DiscardReason::Busy:
        {
            discardReason = QString::fromStdWString(L"拒接");
        }
        break;
        case Export::Data::ActionPhoneCall::DiscardReason::Disconnect:
        {
            discardReason = QString::fromStdWString(L"挂断");
        }
        break;
        case Export::Data::ActionPhoneCall::DiscardReason::Hangup:
        {
            discardReason = QString::fromStdWString(L"通话时长: %1秒").arg(actionContent.duration);
        }
        break;
        case Export::Data::ActionPhoneCall::DiscardReason::Missed:
        {
            discardReason = QString::fromStdWString(L"未接通");
        }
        break;
        default:
            break;
        }

        _chatMessageInfo.content = (QString("%1 发起语音通话 %2").arg(userName).arg(discardReason)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionScreenshotTaken& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = _chatMessageInfo.content = (QString::fromStdWString(L"%1 took a screenshot")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionCustomAction& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = actionContent.message.constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionBotAllowed& actionContent) {
        QString content = actionContent.attachMenu
            ? "You allowed this bot to message you "
            "when you added it in the attachment menu."_q
            : actionContent.app.isEmpty()
            ? ("You allowed this bot to message you when you opened "
                + actionContent.app)
            : ("You allowed this bot to message you when you logged in on "
                + actionContent.domain);

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionSecureValuesSent& actionContent) {
        using namespace Export::Data;

        auto list = std::vector<QByteArray>();
        for (const auto type : actionContent.types) {
            list.push_back([&] {
                using Type = ActionSecureValuesSent::Type;
                switch (type) {
                case Type::PersonalDetails: return "Personal details";
                case Type::Passport: return "Passport";
                case Type::DriverLicense: return "Driver license";
                case Type::IdentityCard: return "Identity card";
                case Type::InternalPassport: return "Internal passport";
                case Type::Address: return "Address information";
                case Type::UtilityBill: return "Utility bill";
                case Type::BankStatement: return "Bank statement";
                case Type::RentalAgreement: return "Rental agreement";
                case Type::PassportRegistration:
                    return "Passport registration";
                case Type::TemporaryRegistration:
                    return "Temporary registration";
                case Type::Phone: return "Phone number";
                case Type::Email: return "Email";
                }
                return "";
                }());
        }

        QString content;
        const auto count = list.size();
        if (count == 1) {
            content = list[0];
        } else if (count > 1) {
            content = list[0];
            for (auto i = 1; i != count - 1; ++i) {
                content += ", " + list[i];
            }
            content += " and " + list[count - 1];
        }

        content = QString("You have sent the following documents: ")
            + content;

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionContactSignUp& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = _chatMessageInfo.content = (QString::fromStdWString(L"%1 joined Telegram")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionPhoneNumberRequest& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = _chatMessageInfo.content = (QString::fromStdWString(L"%1 requested your phone number")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionGeoProximityReached& actionContent) {
        auto peerData = _account.session().data().peer(actionContent.fromId);
        const QString fromName = peerData ? peerData->name() : QString::number(actionContent.fromId.value);

        peerData = _account.session().data().peer(actionContent.toId);
        const auto toName = peerData ? peerData->name() : QString::number(actionContent.toId.value);

        const auto distance = [&]() -> QString {
            if (actionContent.distance >= 1000) {
                const auto km = (10 * (actionContent.distance / 10)) / 1000.;
                return QString::number(km) + " km";
            } else if (actionContent.distance == 1) {
                return "1 meter";
            } else {
                return QString::number(actionContent.distance) + " meters";
            }
        }().toUtf8();

        QString content;
        if (actionContent.fromSelf) {
            content = "You are now within " + distance + " from " + toName;
        } else if (actionContent.toSelf) {
            content = fromName + " is now within " + distance + " from you";
        } else {
            content = fromName
                + " is now within "
                + distance
                + " from "
                + toName;
        }

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionGroupCall& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_CALL;
        _chatMessageInfo.content = (QString("%1 发起群通话, 时长: %2秒").arg(_serviceFrom).arg(actionContent.duration)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionInviteToGroupCall& actionContent) {
        QString msgContent;

        for (const auto& userId : actionContent.userIds) {
            if (!msgContent.isEmpty()) {
                msgContent += ", ";
            }

            msgContent += _account.getUserDisplayName(_account.session().data().user(peerToUser(userId)));
        }

        _chatMessageInfo.content = (QString::fromStdWString(L"%1 添加 %2 进行语音通话").arg(_serviceFrom).arg(msgContent)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionSetMessagesTTL& actionContent) {
        QString periodText;
        if (actionContent.period > 0) {
            int hours = actionContent.period / 3600;
            int days = hours / 24;
            int weeks = days / 7;
            int months = (days + 15) / 30;
            int years = months / 12;

            if (years > 1) {
                periodText = QString("%1 years").arg(years);
            } else if (months >= 1) {
                periodText = QString("%1 month%2").arg(months).arg(months > 1 ? "s" : "");
            } else if (weeks >= 1) {
                periodText = QString("%1 week%2").arg(weeks).arg(weeks > 1 ? "s" : "");
            } else if (days >= 1) {
                periodText = QString("%1 day%2").arg(days).arg(days > 1 ? "s" : "");
            } else if (hours >= 1) {
                periodText = QString("%1 hours").arg(hours);
            }
        }

        QString content;
        content = (_account._curSelectedChat.peerData->isChannel())
            ? (actionContent.period
                ? "New messages will auto-delete in " + periodText
                : "Disabled the auto-delete timer")
            : (actionContent.period
                ? (_serviceFrom
                    + " set messages to auto-delete in " + periodText)
                : (_serviceFrom
                    + " disabled the auto-delete timer"));

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionGroupCallScheduled& actionContent) {
        const auto dateText = Export::Data::FormatDateTime(actionContent.date);
        QString content = (_account._curSelectedChat.peerData->isChannel())
            ? ("Voice chat scheduled for " + dateText)
            : (_serviceFrom + " scheduled a voice chat for " + dateText);

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionSetChatTheme& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 设置聊天背景")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionChatJoinedByRequest& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 joined group by request")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionWebViewDataSent& actionContent) {
        QString content = "You have just successfully transferred data from the <"
            + actionContent.text
            + "> button to the bot";

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionGiftPremium& actionContent) {
        QString content;

        do {
            if (!actionContent.months || actionContent.cost.isEmpty()) {
                content = _serviceFrom + " sent you a gift.";
                break;
            }

            content = _serviceFrom
                + " sent you a gift for "
                + actionContent.cost
                + ": Telegram Premium for "
                + QString::number(actionContent.months).toUtf8()
                + " months.";

        } while (false);

        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = content.toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionTopicCreate& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 发布公告: %2")
            .arg(_serviceFrom).arg(actionContent.title.constData())).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionTopicEdit& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 编辑公告: %2")
            .arg(_serviceFrom).arg(actionContent.title.constData())).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionSuggestProfilePhoto& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = (QString::fromStdWString(L"%1 suggests to use this photo")
            .arg(_serviceFrom)).toUtf8().constData();
    }

    void Main::Account::ServerMessageVisitor::operator()(const Export::Data::ActionRequestedPeer& actionContent) {
        _chatMessageInfo.msgType = IMMsgType::APP_SYSTEM_TEXT;
        _chatMessageInfo.content = QString("requested: "_q).toUtf8().constData();
    }

    Main::Account::MessageMediaVisitor::MessageMediaVisitor(
        Main::Account& account,
        Main::Account::ChatMessageInfo& chatMessageInfo,
        Export::Data::Message* message
    )
        : _account(account),
        _chatMessageInfo(chatMessageInfo),
        _message(message) {}

    void Main::Account::MessageMediaVisitor::operator()(v::null_t) {

    }

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::Photo& media) {
        auto& file = _message->media.file();
        _chatMessageInfo.attachFileName = QString("%1.jpg").arg(_chatMessageInfo.id).toUtf8().constData();
        _chatMessageInfo.msgType = IMMsgType::APP_MSG_PIC;

        file.relativePath = _account._curPeerAttachPath;

        Main::Account::DownloadFileInfo downloadFileInfo;
        downloadFileInfo.msgId = FullMsgId(_message->peerId, _message->id);
        downloadFileInfo.peerId = _chatMessageInfo.peerId;
        downloadFileInfo.docId = media.id;
        downloadFileInfo.dcId = file.location.dcId;
        downloadFileInfo.fileSize = file.size;

        StorageFileLocation fileLocation(file.location.dcId, _account.session().userId(), file.location.data);
        downloadFileInfo.accessHash = fileLocation.accessHash();
        downloadFileInfo.fileLocation = file.location.data;
        downloadFileInfo.saveFilePath = QString("%1%2.jpg").arg(_account._curPeerAttachPath).arg(_chatMessageInfo.id);
        downloadFileInfo.fileName = QString::fromUtf8(_chatMessageInfo.attachFileName.c_str());

        _chatMessageInfo.attachFilePath = _account.getRelativeFilePath(_account._utf8RootPath, downloadFileInfo.saveFilePath.toUtf8().constData());

        do {
            auto iter = _account._selectedChatDownloadAttachMap.find(_message->peerId.value);
            if (iter == _account._selectedChatDownloadAttachMap.end() || !iter->second) {
                break;
            }

            // 跳过已存在文件
            if (file.size > 0 && QFileInfo(downloadFileInfo.saveFilePath).size() >= file.size) {
                break;
            }

            if (file.size == 0 || file.size > _account._maxAttachFileSize) {
                _account.uploadMsg(QString::fromStdWString(L"附件大小限制为：%1，跳过文件 [%2] 大小：%3")
                    .arg(_account.getFormatFileSize(_account._maxAttachFileSize))
                    .arg(downloadFileInfo.fileName)
                    .arg(_account.getFormatFileSize(file.size)));
                break;
            }

            std::lock_guard<std::mutex> locker(*_account._downloadFilesLock);
            _account._downloadFiles.emplace_back(downloadFileInfo);

        } while (false);
    }

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::Document& media) {
        QString fileSuffix;
        QString name = media.name;

        if (name.isEmpty()) {
            const auto mimeString = QString::fromUtf8(media.mime);
            const auto mimeType = Core::MimeTypeForName(mimeString);
            const auto hasMimeType = [&](const auto& mime) {
                return !mimeString.compare(mime, Qt::CaseInsensitive);
            };
            const auto patterns = mimeType.globPatterns();
            const auto pattern = patterns.isEmpty() ? QString() : patterns.front();
            if (media.isVoiceMessage) {
                const auto isMP3 = hasMimeType(u"audio/mp3"_q);
                fileSuffix = isMP3 ? u".mp3"_q : u".ogg"_q;
            } else if (media.isVideoFile) {
                fileSuffix = pattern.isEmpty()
                    ? u".mov"_q
                    : QString(pattern).replace('*', QString());
            } else {
                fileSuffix = pattern.isEmpty()
                    ? u".unknown"_q
                    : QString(pattern).replace('*', QString());
            }
        } else {
            int pos = name.lastIndexOf('.');
            if (pos != -1) {
                fileSuffix = name.mid(pos);
            }
        }

        if (name.isEmpty()) {
            name = QString("%1%2").arg(_chatMessageInfo.id).arg(fileSuffix);
        }

        _chatMessageInfo.attachFileName = name.toUtf8().constData();

        auto& file = _message->media.file();

        file.relativePath = _account._curPeerAttachPath;

        Main::Account::DownloadFileInfo downloadFileInfo;
        downloadFileInfo.msgId = FullMsgId(_message->peerId, _message->id);
        downloadFileInfo.peerId = _chatMessageInfo.peerId;
        downloadFileInfo.docId = media.id;

        if (!media.isSticker) {
            downloadFileInfo.fileOrigin = Data::FileOrigin(Data::FileOriginMessage(_message->peerId, _message->id));
        } else {
            downloadFileInfo.isSticker = true;
            auto documentData = _account.session().data().document(media.id);
            if (documentData) {
                downloadFileInfo.fileOrigin = documentData->stickerSetOrigin();
            }
        }

        StorageFileLocation fileLocation(file.location.dcId, _account.session().userId(), file.location.data);
        downloadFileInfo.dcId = file.location.dcId;
        downloadFileInfo.fileSize = file.size;
        downloadFileInfo.accessHash = fileLocation.accessHash();
        downloadFileInfo.fileReference = fileLocation.fileReference();
        downloadFileInfo.fileLocation = file.location.data;
        downloadFileInfo.saveFilePath = QString("%1%2%3")
            .arg(_account._curPeerAttachPath).arg(_chatMessageInfo.id).arg(fileSuffix);
        downloadFileInfo.fileName = QString::fromUtf8(_chatMessageInfo.attachFileName.c_str());

        _chatMessageInfo.attachFilePath = _account.getRelativeFilePath(_account._utf8RootPath, downloadFileInfo.saveFilePath.toUtf8().constData());

        _chatMessageInfo.msgType = IMMsgType::APP_MSG_FILE;

        _chatMessageInfo.duration = media.duration;

        if (media.isVoiceMessage) {
            _chatMessageInfo.msgType = IMMsgType::APP_MSG_AUDIO;
        } else if (media.isVideoFile || media.isVideoMessage) {
            _chatMessageInfo.msgType = IMMsgType::APP_MSG_VEDIO;
        }

        do {
            auto iter = _account._selectedChatDownloadAttachMap.find(_message->peerId.value);
            if (iter == _account._selectedChatDownloadAttachMap.end() || !iter->second) {
                break;
            }

            // 跳过动图，出现过下载卡住的情况
            if (downloadFileInfo.isSticker) {
                break;
            }

            // 跳过已存在文件
            if (file.size > 0 && QFileInfo(downloadFileInfo.saveFilePath).size() >= file.size) {
                break;
            }

            if (file.size == 0 || file.size > _account._maxAttachFileSize) {
                _account.uploadMsg(QString::fromStdWString(L"附件大小限制为：%1，跳过文件 [%2] 大小：%3")
                    .arg(_account.getFormatFileSize(_account._maxAttachFileSize))
                    .arg(downloadFileInfo.fileName)
                    .arg(_account.getFormatFileSize(file.size)));
                break;
            }

            std::lock_guard<std::mutex> locker(*_account._downloadFilesLock);
            _account._downloadFiles.emplace_back(downloadFileInfo);

        } while (false);
    }

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::SharedContact& media) {
        _chatMessageInfo.msgType = IMMsgType::APP_SHARE_CONTACT;

        _chatMessageInfo.contactFirstName = media.info.firstName.constData();
        _chatMessageInfo.contactLastName = media.info.lastName.constData();
        _chatMessageInfo.contactPhone = media.info.phoneNumber.constData();
        _chatMessageInfo.content = _account.utf16ToUtf8(L"[分享联系人] 号码：")
            + _chatMessageInfo.contactPhone + _account.utf16ToUtf8(L"\n姓名：")
            + _chatMessageInfo.contactFirstName + _chatMessageInfo.contactLastName;
    }

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::GeoPoint& media) {
        _chatMessageInfo.msgType = IMMsgType::APP_MSG_MAP;

        _chatMessageInfo.latitude = media.latitude;
        _chatMessageInfo.longitude = media.longitude;
    }

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::Venue& media) {
        _chatMessageInfo.msgType = IMMsgType::APP_MSG_MAP;

        _chatMessageInfo.location = media.title.constData();
        if (!media.address.isEmpty()) {
            _chatMessageInfo.location += std::string("\n") + media.address.constData();
        }

        _chatMessageInfo.latitude = media.point.latitude;
        _chatMessageInfo.longitude = media.point.longitude;
    }

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::Game& media) {}

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::Invoice& media) {}

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::Poll& media) {}

    void Main::Account::MessageMediaVisitor::operator()(const Export::Data::UnsupportedMedia& media) {}

    Main::Account::ChatMessageInfo Account::messageToChatMessageInfo(Export::Data::Message* message) {
        Main::Account::ChatMessageInfo chatMessageInfo;
        chatMessageInfo.msgType = IMMsgType::APP_MSG_TEXT;

        if (message) {
            chatMessageInfo.id = message->id;
            chatMessageInfo.peerId = message->peerId.value;
            chatMessageInfo.senderId = message->fromId.value;
            chatMessageInfo.senderName = getUserDisplayName(_session->data().user(peerToUser(message->fromId))).toUtf8().constData();

            chatMessageInfo.out = message->out;
            chatMessageInfo.date = message->date;

            for (const auto& textPart : message->text) {
                if (!chatMessageInfo.content.empty()) {
                    chatMessageInfo.content += "\n";
                }

                chatMessageInfo.content += textPart.text.constData();
            }

            {
                Main::Account::MessageMediaVisitor visitor(*this, chatMessageInfo, message);
                std::visit(visitor, message->media.content);
            }

            {
                Main::Account::ServerMessageVisitor visitor(*this, chatMessageInfo, message);
                std::visit(visitor, message->action.content);

                if (chatMessageInfo.msgType == IMMsgType::APP_SYSTEM_TEXT) {
                    chatMessageInfo.senderId = chatMessageInfo.peerId;
                    chatMessageInfo.senderName = utf16ToUtf8(L"系统");
                }
            }
        }

        return chatMessageInfo;
    }

    void Account::saveContactsToDb(const std::list<Main::Account::ContactInfo>& contacts) {
        sqlite3_stmt* stmt = nullptr;

        bool beginTransaction = false;
        bool ok = false;
        const wchar_t* errMsg = nullptr;

        do {
            if (!_dataDb) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            ret = sqlite3_prepare(_dataDb, "insert into users values (?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& contact : contacts) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_text(stmt, column++, QString::number(contact.id).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, contact.firstName.c_str(), contact.firstName.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, contact.lastName.c_str(), contact.lastName.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, contact.userName.c_str(), contact.userName.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, contact.phone.c_str(), contact.phone.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    std::string profile_photo = getRelativeFilePath(_utf8RootPath, _utf8ProfilePhotoPath + std::to_string(contact.id) + ".jpg");
                    ret = sqlite3_bind_text(stmt, column++, profile_photo.c_str(), profile_photo.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, contact.deleted);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);

                    ok = true;

                } while (false);

                if (!ok) {
                    errMsg = (const wchar_t*)sqlite3_errmsg16(_dataDb);
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    void Account::saveDialogsToDb(const std::list<Main::Account::DialogInfo>& dialogs) {
        sqlite3_stmt* stmt = nullptr;

        bool beginTransaction = false;
        bool ok = false;
        const wchar_t* errMsg = nullptr;

        do {
            if (!_dataDb) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            ret = sqlite3_prepare(_dataDb, "insert into dialogs values (?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& dialog : dialogs) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_text(stmt, column++, QString::number(dialog.id).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, dialog.name.c_str(), dialog.name.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, dialog.date);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, dialog.unread_count);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int64(stmt, column++, dialog.lastMid);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, dialog.peerType.c_str(), dialog.peerType.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, dialog.left);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);

                    ok = true;

                } while (false);

                if (!ok) {
                    errMsg = (const wchar_t*)sqlite3_errmsg16(_dataDb);
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    void Account::saveMigratedDialogsToDb(const std::list<Main::Account::MigratedDialogInfo>& dialogs) {
        sqlite3_stmt* stmt = nullptr;

        bool beginTransaction = false;
        bool ok = false;
        const wchar_t* errMsg = nullptr;

        do {
            if (!_dataDb) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            ret = sqlite3_prepare(_dataDb, "insert into migrated_to_dialogs values (?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& dialog : dialogs) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_text(stmt, column++, QString::number(dialog.did).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, QString::number(dialog.fromDid).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }
                    
                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);

                    ok = true;

                } while (false);

                if (!ok) {
                    errMsg = (const wchar_t*)sqlite3_errmsg16(_dataDb);
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    void Account::saveChatsToDb(const std::list<Main::Account::ChatInfo>& chats) {
        sqlite3_stmt* stmt = nullptr;

        bool beginTransaction = false;
        bool ok = false;
        const wchar_t* errMsg = nullptr;

        do {
            if (!_dataDb) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            ret = sqlite3_prepare(_dataDb, "insert into chats values (?, ?, ?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& chat : chats) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_text(stmt, column++, QString::number(chat.id).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chat.title.c_str(), chat.title.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chat.date);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chat.isChannel);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chat.membersCount);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chat.channelName.c_str(), chat.channelName.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    std::string profile_photo = getRelativeFilePath(_utf8RootPath, _utf8ProfilePhotoPath + std::to_string(chat.id) + ".jpg");
                    ret = sqlite3_bind_text(stmt, column++, profile_photo.c_str(), profile_photo.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chat.peerType.c_str(), chat.peerType.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chat.left);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);

                    ok = true;

                } while (false);

                if (!ok) {
                    errMsg = (const wchar_t*)sqlite3_errmsg16(_dataDb);
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    void Account::saveParticipantsToDb(
        uint64_t peerId,
        const std::list<Main::Account::ParticipantInfo>& participants
    ) {
        sqlite3_stmt* stmt = nullptr;
        bool beginTransaction = false;
        bool ok = false;

        do {
            if (!_dataDb || participants.empty()) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            ret = sqlite3_prepare(_dataDb, "insert into participants values (?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& participant : participants) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_text(stmt, column++, QString::number(peerId).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, QString::number(participant.id).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, participant.firstName.c_str(), participant.firstName.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, participant.lastName.c_str(), participant.lastName.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, participant.userName.c_str(), participant.userName.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, participant.phone.c_str(), participant.phone.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, participant._type.c_str(), participant._type.size(), SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ok = true;

                } while (false);

                if (!ok) {
                    break;
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    void Account::saveChatMessagesToDb(const std::list<ChatMessageInfo>& chatMessages) {
        sqlite3_stmt* stmt = nullptr;
        bool beginTransaction = false;
        bool ok = false;

        do {
            if (!_dataDb || chatMessages.empty()) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            ret = sqlite3_prepare(_dataDb, "insert into messages values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& chatMessage : chatMessages) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_int(stmt, column++, chatMessage.id);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, QString::number(chatMessage.peerId).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, QString::number(chatMessage.senderId).toUtf8().constData(), -1, SQLITE_TRANSIENT);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chatMessage.date);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chatMessage.out);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, (std::int32_t)chatMessage.msgType);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, chatMessage.duration);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_double(stmt, column++, chatMessage.latitude);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_double(stmt, column++, chatMessage.longitude);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chatMessage.location.c_str(), chatMessage.location.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chatMessage.senderName.c_str(), chatMessage.senderName.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chatMessage.content.c_str(), chatMessage.content.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chatMessage.attachThumbFilePath.c_str(), chatMessage.attachThumbFilePath.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chatMessage.attachFilePath.c_str(), chatMessage.attachFilePath.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, chatMessage.attachFileName.c_str(), chatMessage.attachFileName.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);

                    saveChatMutiMessagesToDb(chatMessage);

                    ok = true;

                } while (false);

                if (!ok) {
                    break;
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    void Account::saveChatMutiMessagesToDb(const ChatMessageInfo& chatMessage) {
        sqlite3_stmt* stmt = nullptr;
        bool beginTransaction = false;
        bool ok = false;

        do {
            if (!_dataDb || chatMessage.mutiMsgs.empty()) {
                break;
            }

            int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            beginTransaction = true;

            /*
            * CREATE TABLE IF NOT EXISTS muti_messages(mid INTEGER NOT NULL, msg_type INTEGER, content TEXT);
            */
            ret = sqlite3_prepare(_dataDb, "insert into muti_messages values (?, ?, ?);", -1, &stmt, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            for (const auto& mutiMessage : chatMessage.mutiMsgs) {
                ok = false;
                int column = 1;

                do {
                    int ret = sqlite3_bind_int(stmt, column++, chatMessage.id);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_int(stmt, column++, (std::int32_t)mutiMessage.msgType);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_bind_text(stmt, column++, mutiMessage.content.c_str(), mutiMessage.content.size(), SQLITE_STATIC);
                    if (ret != SQLITE_OK) {
                        break;
                    }

                    ret = sqlite3_step(stmt);
                    if (ret != SQLITE_DONE) {
                        break;
                    }

                    ret = sqlite3_reset(stmt);

                    ok = true;

                } while (false);

                if (!ok) {
                    break;
                }
            }

        } while (false);

        if (stmt) {
            sqlite3_finalize(stmt);
        }

        if (beginTransaction) {
            if (ok) {
                sqlite3_exec(_dataDb, "COMMIT;", nullptr, nullptr, nullptr);
            } else {
                sqlite3_exec(_dataDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
        }
    }

    bool Account::init() {
        bool ok = false;
        const wchar_t* errMsg = nullptr;

        do {
            if (!sessionExists() || _dataDb) {
                break;
            }

            QString activeAccount = Core::App().activeAccountId();
            if (!activeAccount.isEmpty() && activeAccount != QString::number(_session->user()->id.value)) {
                break;
            }

            const auto& appArgs = Core::Launcher::getApplicationArguments();
            if (appArgs.size() < 6) {
                break;
            }

            _dataPath = appArgs[2];
            if (!_dataPath.empty() && _dataPath.back() != '\\') {
                _dataPath += L"\\";
            }

            _utf8DataPath = utf16ToUtf8(_dataPath);

            _utf8RootPath = utf16ToUtf8(appArgs[3]);
            if (!_utf8RootPath.empty() && _utf8RootPath.back() == '\\') {
                _utf8RootPath.pop_back();
            }

            _attachPath = _dataPath + L"files\\";

            _profilePhotoPath = _dataPath + L"profile\\";
            _utf8ProfilePhotoPath = utf16ToUtf8(_profilePhotoPath);

            if (GetFileAttributesW(_profilePhotoPath.c_str()) == -1) {
                CreateDirectoryW(_profilePhotoPath.c_str(), nullptr);
            }

            if (GetFileAttributesW(_attachPath.c_str()) == -1) {
                CreateDirectoryW(_attachPath.c_str(), nullptr);
            }

            std::wstring dataDbPath = _dataPath + _session->user()->phone().toStdWString() + L".db";
            int ret = sqlite3_open16(dataDbPath.c_str(), &_dataDb);
            if (ret != SQLITE_OK) {
                break;
            }

            std::string sql = "PRAGMA encoding = 'UTF-8';";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE TABLE IF NOT EXISTS users(uid TEXT NOT NULL UNIQUE, first_name TEXT, last_name TEXT, "
                "username TEXT, phone TEXT, profile_photo TEXT, deleted INTEGER);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            ret = sqlite3_exec(_dataDb, "delete from users;", nullptr, nullptr, nullptr);

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS users_index ON users(uid);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE TABLE IF NOT EXISTS dialogs(did TEXT NOT NULL, name TEXT, date INTEGER, "
                "unread_count INTEGER, last_mid INTEGER, peerType TEXT, left INTEGER);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            ret = sqlite3_exec(_dataDb, "delete from dialogs;", nullptr, nullptr, nullptr);

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS dialogs_index ON dialogs(did);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = R"(CREATE TABLE IF NOT EXISTS migrated_to_dialogs(did TEXT NOT NULL, from_did TEXT NOT NULL);)";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            ret = sqlite3_exec(_dataDb, "delete from migrated_to_dialogs;", nullptr, nullptr, nullptr);

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS migrated_to_dialogs_index ON migrated_to_dialogs(did, from_did);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE TABLE IF NOT EXISTS messages(mid INTEGER NOT NULL, peer_id TEXT, sender_id TEXT, "
                "date INTEGER, out INTEGER, msg_type INTEGER, duration INTEGER, latitude REAL, longitude REAL, "
                "location TEXT, sender_name TEXT, content TEXT, thumb_file TEXT, attach_file TEXT, attach_filename TEXT);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS messages_index ON messages(mid, peer_id);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE TABLE IF NOT EXISTS muti_messages(mid INTEGER NOT NULL, msg_type INTEGER, content TEXT);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS muti_messages_index ON muti_messages(mid);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE TABLE IF NOT EXISTS chats(cid TEXT NOT NULL, title TEXT, date INTEGER, "
                "is_channel INTEGER, chat_member_nums INTEGER, channel_name TEXT, profile_photo TEXT, peerType TEXT, left INTEGER);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            ret = sqlite3_exec(_dataDb, "delete from chats;", nullptr, nullptr, nullptr);

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS chats_index ON chats(cid);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            sql = "CREATE TABLE IF NOT EXISTS participants(cid TEXT NOT NULL, uid TEXT NOT NULL, first_name TEXT, "
                "last_name TEXT, username TEXT, phone TEXT, type TEXT, PRIMARY KEY (cid, uid));";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            ret = sqlite3_exec(_dataDb, "delete from participants;", nullptr, nullptr, nullptr);

            sql = "CREATE UNIQUE INDEX IF NOT EXISTS participants_index ON participants(cid, uid);";
            ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
            if (ret != SQLITE_OK) {
                break;
            }

            ok = true;
        } while (false);

        if (!ok && _dataDb) {
            errMsg = (const wchar_t*)sqlite3_errmsg16(_dataDb);
        }

        return ok;
    }

    bool Account::getRecvPipeCmd() {
        bool isValidCmd = false;
        {
            std::lock_guard<std::mutex> locker(*_pipeCmdsLock);
            if (!_recvPipeCmds.empty()) {
                auto iter = _runningPipeCmds.find(_curRecvCmd.uniqueId);
                if (iter != _runningPipeCmds.end()) {
                    isValidCmd = false;
                } else {
                    _curRecvCmd.Clear();
                    _curRecvCmd = _recvPipeCmds.front();
                    _recvPipeCmds.pop_front();
                    _runningPipeCmds.emplace(_curRecvCmd.uniqueId);
                    isValidCmd = true;
                }
            }
        }
        return isValidCmd;
    }

    PipeCmd::Cmd Account::sendPipeCmd(
        const PipeCmd::Cmd& cmd,
        bool waitDone
    ) {
        {
            std::lock_guard<std::mutex> locker(*_pipeCmdsLock);
            auto iter = _runningPipeCmds.find(cmd.uniqueId);
            if (iter != _runningPipeCmds.end()) {
                _runningPipeCmds.erase(iter);
            }
        }
        return _pipe->SendCmd(cmd, waitDone);
    }

    PipeCmd::Cmd Account::sendPipeResult(
        const PipeCmd::Cmd& recvCmd,
        TelegramCmd::Status status,
        const QString& content,
        const QString& error
    ) {
        PipeCmd::Cmd resultCmd;
        resultCmd.action = recvCmd.action;
        resultCmd.uniqueId = recvCmd.uniqueId;
        resultCmd.content = content.toUtf8().constData();

        ProtobufCmd::Content protobufContent;
        AddExtraData(protobufContent, "content", content.toUtf8().constData());
        AddExtraData(protobufContent, "status", std::int32_t(status));

        if (!error.isEmpty()) {
            AddExtraData(protobufContent, "error", error.toUtf8().constData());
        }

        protobufContent.SerializeToString(&resultCmd.content);

        LOG(("[Account][sendPipeResult] unique ID: %1 action: %2 status: %3 content:%4 %5")
            .arg(QString::fromUtf8(resultCmd.uniqueId.c_str()))
            .arg(resultCmd.action)
            .arg((std::int32_t)status)
            .arg(content)
            .arg(!error.isEmpty() ? ("error: " + error) : "")
        );

        return sendPipeCmd(resultCmd, false);
    }

    void Account::uploadMsg(const QString& content) {
        PipeCmd::Cmd cmd;
        cmd.action = std::int32_t(TelegramCmd::Action::UploadMsg);
        cmd.content = content.toUtf8().constData();

        LOG(("[Account][uploadMsg] msg: %1")
            .arg(content)
        );

        sendPipeCmd(cmd, false);
    }

    void Account::checkForTokenUpdate(const MTPUpdates& updates) {
        updates.match([&](const MTPDupdateShort& data) {
            checkForTokenUpdate(data.vupdate());
            }, [&](const MTPDupdates& data) {
                for (const auto& update : data.vupdates().v) {
                    checkForTokenUpdate(update);
                }
            }, [&](const MTPDupdatesCombined& data) {
                for (const auto& update : data.vupdates().v) {
                    checkForTokenUpdate(update);
                }
            }, [](const auto&) {});
    }

    void Account::checkForTokenUpdate(const MTPUpdate& update) {
        update.match([&](const MTPDupdateLoginToken& data) {
            if (_requestId) {
                _forceRefresh = true;
            } else {
                _refreshQrCodeTimer.cancel();
                refreshQrCode();
            }
            }, [](const auto&) {});
    }

    void Account::importTo(MTP::DcId dcId, const QByteArray& token) {
        api().instance().setMainDcId(dcId);
        _requestId = api().request(MTPauth_ImportLoginToken(
            MTP_bytes(token)
        )).done([=](const MTPauth_LoginToken& result) {
            handleTokenResult(result);
            }).fail([=](const MTP::Error& error) {
                if (_firstRefreshQrCode) {
                    sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                }
                }).toDC(dcId).send();
    }

    void Account::handleTokenResult(const MTPauth_LoginToken& result) {
        result.match([&](const MTPDauth_loginToken& data) {
            _requestId = 0;

            QString qrcodeString;

            auto token = data.vtoken().v;
            auto qrData = Qr::Encode("tg://login?token=" + token.toBase64(QByteArray::Base64UrlEncoding), Qr::Redundancy::Default);
            if (qrData.size > 0) {
                int pixel = st::introQrPixel;
                int max = st::introQrMaxSize;

                if (max > 0 && qrData.size * pixel > max) {
                    pixel = (std::max)(max / qrData.size, 1);
                }
                const auto qr = Qr::Generate(qrData, pixel * style::DevicePixelRatio(), Qt::black);
                auto qrImage = QImage(qr.size(), QImage::Format_ARGB32_Premultiplied);
                qrImage.fill(Qt::white);
                {
                    auto p = QPainter(&qrImage);
                    p.drawImage(QRect(QPoint(), qr.size()), qr);
                }

                QBuffer buffer;
                buffer.open(QIODevice::WriteOnly);
                qrImage.save(&buffer, "png");
                qrcodeString = buffer.data().toBase64().constData();
            }

            if (base::take(_forceRefresh)) {
                refreshQrCode();
            } else {
                const auto left = data.vexpires().v - base::unixtime::now();
                _refreshQrCodeTimer.callOnce(std::max(left, 1) * crl::time(1000));
            }

            if (_firstRefreshQrCode) {
                sendPipeResult(_curRecvCmd, TelegramCmd::Status::Success, qrcodeString);
            } else {
                PipeCmd::Cmd cmd;
                cmd.action = std::int32_t(TelegramCmd::Action::GenerateQrCode);
                sendPipeResult(cmd, TelegramCmd::Status::Success, qrcodeString);
            }
            }, [&](const MTPDauth_loginTokenMigrateTo& data) {
                importTo(data.vdc_id().v, data.vtoken().v);
            }, [&](const MTPDauth_loginTokenSuccess& data) {
                onLoginSucess(data.vauthorization());
            });
    }

    void Account::refreshQrCode() {
        if (_requestId) {
            return;
        }

        _requestId = api().request(MTPauth_ExportLoginToken(
            MTP_int(ApiId),
            MTP_string(ApiHash),
            MTP_vector<MTPlong>(0)
        )).done([=](const MTPauth_LoginToken& result) {
            handleTokenResult(result);
            }).fail([=](const MTP::Error& error) {
                _requestId = 0;

                if (error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
                    _refreshQrCodeTimer.cancel();

                    _requestId = api().request(MTPaccount_GetPassword(
                    )).done([=](const MTPaccount_Password& result) {
                        const auto& d = result.c_account_password();
                        _passwordState = Core::ParseCloudPasswordState(d);
                        if (!d.vcurrent_algo() || !d.vsrp_id() || !d.vsrp_B()) {
                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", "API Error: No current password received on login.");
                        } else if (!_passwordState.hasPassword) {
                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError);
                        } else {
                            _getCloudPasswordState = true;
                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::NeedVerify);
                        }
                        }).fail([=](const MTP::Error& error) {
                            sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                            }).handleFloodErrors().send();
                } else if (base::take(_forceRefresh)) {
                    refreshQrCode();
                } else {
                    if (_firstRefreshQrCode) {
                        _refreshQrCodeTimer.cancel();
                        sendPipeResult(_curRecvCmd, TelegramCmd::Status::UnknownError, "", error.description());
                    }
                }
                }).send();
    }

    void Account::checkRequest() {
        auto status = api().instance().state(_requestId);
        if (status < 0) {
            auto leftms = -status;
            if (leftms >= 1000) {
                api().request(base::take(_requestId)).cancel();
            }
        }
        if (!_requestId && status == MTP::RequestSent) {
            _checkRequest = false;
        }
    }

    void Account::AddExtraData(
        ProtobufCmd::Content& content,
        const std::string& key,
        const std::string& value
    ) {
        ProtobufCmd::Extra* extra = content.add_extra();
        if (extra) {
            extra->set_type(ProtobufCmd::ExtraType::String);
            extra->set_key(key);
            extra->set_string_value(value);
        }
    }

    void Account::AddExtraData(
        ProtobufCmd::Content& content,
        const std::string& key,
        long long value
    ) {
        ProtobufCmd::Extra* extra = content.add_extra();
        if (extra) {
            extra->set_type(ProtobufCmd::ExtraType::Num);
            extra->set_key(key);
            extra->set_num_value(value);
        }
    }

    void Account::AddExtraData(
        ProtobufCmd::Content& content,
        const std::string& key,
        unsigned long long value
    ) {
        ProtobufCmd::Extra* extra = content.add_extra();
        if (extra) {
            extra->set_type(ProtobufCmd::ExtraType::Num);
            extra->set_key(key);
            extra->set_num_value(value);
        }
    }

    void Account::AddExtraData(
        ProtobufCmd::Content& content,
        const std::string& key,
        int value
    ) {
        return AddExtraData(content, key, (long long)value);
    }

    void Account::AddExtraData(
        ProtobufCmd::Content& content,
        const std::string& key,
        unsigned int value
    ) {
        return AddExtraData(content, key, (unsigned long long)value);
    }

    void Account::AddExtraData(
        ProtobufCmd::Content& content,
        const std::string& key,
        double value
    ) {
        ProtobufCmd::Extra* extra = content.add_extra();
        if (extra) {
            extra->set_type(ProtobufCmd::ExtraType::Real);
            extra->set_key(key);
            extra->set_real_value(value);
        }
    }

    std::string Account::GetStringExtraData(
        const ProtobufCmd::Content& content,
        const std::string& key
    ) {
        std::string data;

        for (const auto& extra : content.extra()) {
            if (extra.key() == key && extra.type() == ProtobufCmd::ExtraType::String) {
                data = extra.string_value();
                break;
            }
        }

        return data;
    }

    long long Account::GetNumExtraData(
        const ProtobufCmd::Content& content,
        const std::string& key
    ) {
        long long data = -1LL;

        for (const auto& extra : content.extra()) {
            if (extra.key() == key && extra.type() == ProtobufCmd::ExtraType::Num) {
                data = extra.num_value();
                break;
            }
        }

        return data;
    }

    double Account::GetRealExtraData(
        const ProtobufCmd::Content& content,
        const std::string& key
    ) {
        double data = 0.0;

        for (const auto& extra : content.extra()) {
            if (extra.key() == key && extra.type() == ProtobufCmd::ExtraType::Real) {
                data = extra.real_value();
                break;
            }
        }

        return data;
    }

    bool Account::GetBooleanExtraData(
        const ProtobufCmd::Content& content,
        const std::string& key
    ) {
        bool data = false;

        for (const auto& extra : content.extra()) {
            if (extra.key() == key && extra.type() == ProtobufCmd::ExtraType::Num) {
                data = extra.num_value() != 0;
                break;
            }
        }

        return data;
    }

} // namespace Main
