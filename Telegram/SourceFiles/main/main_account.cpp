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
#include "pipe/telegram_cmd.h"

namespace Main {
namespace {

constexpr auto kWideIdsTag = ~uint64(0);

[[nodiscard]] QString ComposeDataString(const QString &dataName, int index) {
	auto result = dataName;
	result.replace('#', QString());
	if (index > 0) {
		result += '#' + QString::number(index + 1);
	}
	return result;
}

} // namespace

Account::Account(not_null<Domain*> domain, const QString &dataName, int index)
: _domain(domain)
, _local(std::make_unique<Storage::Account>(
	this,
	ComposeDataString(dataName, index)))
, _dataDb(nullptr)
, _pipe(nullptr)
, _handlePipeCmdTimer([this] { handlePipeCmd(); }) {
	_handlePipeCmdTimer.callEach(1000);
}

Account::~Account() {
	if (const auto session = maybeSession()) {
		session->saveSettingsNowIfNeeded();
	}
	destroySession(DestroyReason::Quitting);
    if (_dataDb) {
        sqlite3_close(_dataDb);
        _dataDb = nullptr;
    }
}

Storage::Domain &Account::domainLocal() const {
	return _domain->local();
}

[[nodiscard]] Storage::StartResult Account::legacyStart(
		const QByteArray &passcode) {
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
	) | rpl::start_with_next([=](const ProxyChange &change) {
		const auto key = [&](const MTP::ProxyData &proxy) {
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
	) | rpl::start_with_next([=](Session *session) {
		if (!session && _mtp) {
			_mtp->setUserPhone(QString());
		}
	}, _lifetime);
}

uint64 Account::willHaveSessionUniqueId(MTP::Config *config) const {
	// See also Session::uniqueId.
	if (!_sessionUserId) {
		return 0;
	}
	return _sessionUserId.bare
		| (config && config->isTestMode() ? 0x0100'0000'0000'0000ULL : 0ULL);
}

void Account::createSession(
		const MTPUser &user,
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
		const MTPUser &user,
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

Session &Account::session() const {
	Expects(sessionExists());

	return *_sessionValue.current();
}

Session *Account::maybeSession() const {
	return _sessionValue.current();
}

rpl::producer<Session*> Account::sessionValue() const {
	return _sessionValue.value();
}

rpl::producer<Session*> Account::sessionChanges() const {
	return _sessionValue.changes();
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpValue() const {
	return _mtpValue.value() | rpl::map([](MTP::Instance *instance) {
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
			const MTP::AuthKeysList &keys,
			const MTP::AuthKeysList &keysToDestroy) {
		const auto keysSize = [](auto &list) {
			const auto keyDataSize = MTP::AuthKey::Data().size();
			return sizeof(qint32)
				+ list.size() * (sizeof(qint32) + keyDataSize);
		};
		const auto writeKeys = [](
				QDataStream &stream,
				const MTP::AuthKeysList &keys) {
			stream << qint32(keys.size());
			for (const auto &key : keys) {
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
	const auto &keys = _mtpFields.keys;
	const auto &keysToDestroy = _mtpKeysToDestroy;
	return serialize(_mtpFields.mainDcId, keys, keysToDestroy);
}

void Account::setSessionUserId(UserId userId) {
	Expects(!sessionExists());

	_sessionUserId = userId;
}

void Account::setSessionFromStorage(
		std::unique_ptr<SessionSettings> data,
		QByteArray &&selfSerialized,
		int32 selfStreamVersion) {
	Expects(!sessionExists());

	DEBUG_LOG(("sessionUserSerialized set: %1"
		).arg(selfSerialized.size()));

	_storedSessionSettings = std::move(data);
	_sessionUserSerialized = std::move(selfSerialized);
	_sessionUserStreamVersion = selfStreamVersion;
}

SessionSettings *Account::getSessionSettings() {
	if (_sessionUserId) {
		return _storedSessionSettings
			? _storedSessionSettings.get()
			: nullptr;
	} else if (const auto session = maybeSession()) {
		return &session->settings();
	}
	return nullptr;
}

void Account::setMtpAuthorization(const QByteArray &serialized) {
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

	const auto readKeys = [&](auto &keys) {
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

	_mtp->setUpdatesHandler([=](const MTP::Response &message) {
		checkForUpdates(message) || checkForNewSession(message);
	});
	_mtp->setGlobalFailHandler([=](const MTP::Error &, const MTP::Response &) {
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

bool Account::checkForUpdates(const MTP::Response &message) {
	auto updates = MTPUpdates();
	auto from = message.reply.constData();
	if (!updates.read(from, from + message.reply.size())) {
		return false;
	}
	_mtpUpdates.fire(std::move(updates));
	return true;
}

bool Account::checkForNewSession(const MTP::Response &message) {
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

void Account::destroyMtpKeys(MTP::AuthKeysList &&keys) {
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

	for (const auto &key : _mtp->getKeysForWrite()) {
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

void Account::handleLoginCode(const QString &code) const {
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

void Account::setIntroStepWidgets(std::vector<Intro::details::Step*>* stepHistory) {
	_stepHistory = stepHistory;
}

bool Account::connectPipe() {
	bool connected = false;
	const auto& appArgs = Core::Launcher::getApplicationArguments();
	if (appArgs.size() >= 5) {
		_dataPath = appArgs[1];
		_pipe = std::make_unique<PipeWrapper>(appArgs[3], appArgs[4], PipeType::PipeClient);
		_pipe->RegisterCallback(this, [](void* ctx, const PipeCmd::Cmd& cmd) {
			if (ctx) {
				Account* account = (Account*)ctx;
                {
                    std::lock_guard<std::mutex> locker(account->_recvPipeCmdsLock);
					account->_recvPipeCmds.push_back(cmd);
                }
			}
			}, [](void* ctx)->bool {
				if (ctx) {
					if (((Account*)ctx)->_loggingOut) {
						Core::Quit();
					}

					return ((Account*)ctx)->_loggingOut;
				}
				return false;
			});

		_prevRequestDone = true;
        _contactsAndChatsLoadFinished = false;
        if (_pipe->ConnectPipe(30 * 1000)) {
            _handlePipeCmdTimer.callEach(1000);
        }

    }
	return connected;
}

void Account::handlePipeCmd() {
    do {
		auto action = (TelegramCmd::Action)_curCmd.action();
		if (action == TelegramCmd::Action::GetContactAndChat) {
			saveNewContactsToDb();

			do {
				ParticipantsLoadStatus* participantsLoadStatus = nullptr;

				{
					std::lock_guard<std::mutex> locker(_participantsLoadStatusListLock);
					if (!_participantsLoadStatusList.empty()) {
						participantsLoadStatus = &_participantsLoadStatusList.front();
					}
				}

				if (!participantsLoadStatus) {
					if (_contactsAndChatsLoadFinished) {
						sendPipeResult(_curCmd, std::int32_t(TelegramCmd::LoginStatus::Success));
					}
					break;
				}

				if (!_prevRequestDone) {
					break;
				}

				_prevRequestDone = false;

                if (participantsLoadStatus->peerData->isChannel()) {
                    const auto participantsHash = uint64(0);
                    const auto channel = participantsLoadStatus->peerData->asChannel();

                    _session->api().request(MTPchannels_GetParticipants(
                        channel->inputChannel,
                        MTP_channelParticipantsRecent(),
                        MTP_int(participantsLoadStatus->loadOffset),
                        MTP_int(200),
                        MTP_long(participantsHash)
                    )).done([=, this](const MTPchannels_ChannelParticipants& result) {
                        ParticipantsLoadStatus* participantsLoadStatus = nullptr;

                        {
                            std::lock_guard<std::mutex> locker(_participantsLoadStatusListLock);
                            if (!_participantsLoadStatusList.empty()) {
                                participantsLoadStatus = &_participantsLoadStatusList.front();
                            }
                        }

                        const auto firstLoad = participantsLoadStatus ? !participantsLoadStatus->loadOffset : true;

                        auto wasRecentRequest = firstLoad && channel->canViewMembers();

                        result.match([&](const MTPDchannels_channelParticipants& data) {
                            const auto& [availableCount, list] = wasRecentRequest
                                ? Api::ChatParticipants::ParseRecent(channel, data)
                                : Api::ChatParticipants::Parse(channel, data);

                            std::list<ParticipantInfo> participants;

                            for (const auto& data : list) {
                                UserData* userData = _session->data().userLoaded(data.userId());
                                if (userData) {
                                    participants.emplace_back(std::move(UserDataToParticipantInfo(userData)));
                                }
                            }

                            if (const auto size = list.size()) {
                                saveParticipantsToDb(participants);

                                {
                                    std::lock_guard<std::mutex> locker(_participantsLoadStatusListLock);
                                    if (!_participantsLoadStatusList.empty()) {
										auto& var = _participantsLoadStatusList.front();
										var.loadOffset += size;
                                    }
                                }
                            } else {
                                // To be sure - wait for a whole empty result list.
								removeLoadFinishedParticipants();
                            }
                            }, [&](const MTPDchannels_channelParticipantsNotModified&) {
                                removeLoadFinishedParticipants();
                            });

                        _prevRequestDone = true;
                        }
					).fail([this] {
                            _prevRequestDone = true;
                            removeLoadFinishedParticipants();
                            }).send();
                } else if (participantsLoadStatus->peerData->isChat()) {
                    const auto chat = participantsLoadStatus->peerData->asChat();
                    _session->api().request(MTPmessages_GetFullChat(
                        chat->inputChat
                    )).done([=](const MTPmessages_ChatFull& result) {
                        std::list<ParticipantInfo> participantInfos;

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

                                _prevRequestDone = true;
                                removeLoadFinishedParticipants();
                                }, [&](const MTPDchatParticipants& data) {
                                    const auto status = chat->applyUpdateVersion(data.vversion().v);
                                    if (status == ChatData::UpdateStatus::TooOld) {
                                        return;
                                    }

                                    const auto& list = data.vparticipants().v;
                                    for (const auto& participant : list) {
                                        const auto userId = participant.match([&](const auto& data) {
                                            return data.vuser_id().v;
                                            });

                                        const auto userData = chat->owner().userLoaded(userId);
                                        if (userData) {
											participantInfos.emplace_back(std::move(UserDataToParticipantInfo(userData)));
										}
                                    }

                                    saveParticipantsToDb(participantInfos);

                                    _prevRequestDone = true;
                                    removeLoadFinishedParticipants();
                                });
							}, [&](const MTPDchannelFull& data) {
                                _prevRequestDone = true;
                                removeLoadFinishedParticipants();
							});
                        }
					).fail([this] {
                            _prevRequestDone = true;
                            removeLoadFinishedParticipants();
                    }).send();
				} else {
                    _prevRequestDone = true;
                    removeLoadFinishedParticipants();
				}

            } while (false);
        }

        PipeCmd::Cmd recvCmd;
        bool isValidCmd = getRecvPipeCmd(recvCmd);
        if (!isValidCmd) {
            break;
        }

		_curCmd = recvCmd;

        action = (TelegramCmd::Action)_curCmd.action();
        if (action == TelegramCmd::Action::CheckIsLogin) {
			std::string content;

            if (sessionExists()) {
				content = _session->user()->phone().toUtf8().constData();
            }

            sendPipeResult(_curCmd, std::int32_t(TelegramCmd::LoginStatus::Success), content);
        } else if (action == TelegramCmd::Action::SendPhoneCode) {
            if (_stepHistory) {
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::StartWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->submit();
                    }
                }
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::QrWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->submit();
                    }
                }
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::PhoneWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->setPhoneNumber(_curCmd);
                    }
                }
            }
        } else if (action == TelegramCmd::Action::LoginByPhone) {
            if (_stepHistory) {
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::CodeWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->setPhoneCode(_curCmd);
                    }
                }
            }
        } else if (action == TelegramCmd::Action::GenerateQrCode) {
            if (_stepHistory) {
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::StartWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->submit();
                    }
                    if (!_stepHistory->empty()) {
                        auto widget = dynamic_cast<Intro::details::QrWidget*>(_stepHistory->back());
                        if (widget) {
                            widget->setPipeCmd(_curCmd);
                        }
                    }
                }
            }
        } else if (action == TelegramCmd::Action::LoginByQrCode) {
            if (_stepHistory) {
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::QrWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->setPipeCmd(_curCmd);
                    }
                }
            }
        } else if (action == TelegramCmd::Action::SecondVerify) {
            if (_stepHistory) {
                if (!_stepHistory->empty()) {
                    auto widget = dynamic_cast<Intro::details::PasswordCheckWidget*>(_stepHistory->back());
                    if (widget) {
                        widget->setPassword(_curCmd);
                    }
                }
            }
        } else if (action == TelegramCmd::Action::GetContactAndChat) {
            _session->api().requestContactsAndDialogs();
        } else if (action == TelegramCmd::Action::LogOut) {
            _mtp->logout([this]() {
				sendPipeResult(_curCmd, std::int32_t(TelegramCmd::LoginStatus::Success));
                });
        } else if (action == TelegramCmd::Action::Unknown) {
            Core::Quit();
        }
    } while (false);
}

void Account::saveNewContactsToDb() {
	sqlite3_stmt* stmt = nullptr;
	bool beginTransaction = false;
    bool ok = false;

	do {
		if (!_dataDb) {
			break;
		}

		std::list<ContactInfo> newContacts;
		{
			std::lock_guard<std::mutex> locker(_newContactsLock);
			if (_newContacts.empty()) {
				break;
			}

			newContacts.swap(_newContacts);
		}

		int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
		if (ret != SQLITE_OK) {
			break;
		}

		beginTransaction = true;

		/*
		* CREATE TABLE IF NOT EXISTS users(uid INTEGER PRIMARY KEY NOT NULL, first_name TEXT
		, last_name TEXT, username TEXT, phone TEXT, profile_photo TEXT, deleted INTEGER);
		*/
		ret = sqlite3_prepare(_dataDb, "insert into users values (?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
		if (ret != SQLITE_OK) {
			break;
		}

		for (const auto& contact : newContacts) {
            ok = false;
            int column = 1;

            do {

                int ret = sqlite3_bind_int64(stmt, column++, contact.uid);
                if (ret != SQLITE_OK) {
                    break;
                }

                ret = sqlite3_bind_text(stmt, column++, contact.firstName.c_str(), contact.firstName.size(), SQLITE_TRANSIENT);
                if (ret != SQLITE_OK) {
                    break;
                }

                ret = sqlite3_bind_text(stmt, column++, contact.lastName.c_str(), contact.lastName.size(), SQLITE_TRANSIENT);
                if (ret != SQLITE_OK) {
                    break;
                }

                ret = sqlite3_bind_text(stmt, column++, contact.userName.c_str(), contact.userName.size(), SQLITE_TRANSIENT);
                if (ret != SQLITE_OK) {
                    break;
                }

                ret = sqlite3_bind_text(stmt, column++, contact.phone.c_str(), contact.phone.size(), SQLITE_TRANSIENT);
                if (ret != SQLITE_OK) {
                    break;
                }

                ret = sqlite3_bind_text(stmt, column++, "", -1, SQLITE_TRANSIENT);
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

void Account::saveParticipantsToDb(
    const std::list<ParticipantInfo>& participants
) {
	sqlite3_stmt* stmt = nullptr;
	bool beginTransaction = false;
	bool ok = false;

	do {
		if (!_dataDb || participants.empty()) {
			break;
		}

		uint64_t peerId = -1;

		{
			std::lock_guard<std::mutex> locker(_participantsLoadStatusListLock);
			if (!_participantsLoadStatusList.empty()) {
				peerId = _participantsLoadStatusList.front().peerData->id.value;
			}
		}

		if (peerId == -1) {
			break;
		}

		int ret = sqlite3_exec(_dataDb, "BEGIN;", nullptr, nullptr, nullptr);
		if (ret != SQLITE_OK) {
			break;
		}

		beginTransaction = true;

		/*
        * CREATE TABLE IF NOT EXISTS participants(cid INTEGER NOT NULL, uid INTEGER NOT NULL,
		first_name TEXT, last_name TEXT, username TEXT, phone TEXT, type TEXT, PRIMARY KEY (cid, uid));
		*/
		ret = sqlite3_prepare(_dataDb, "insert into participants values (?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
		if (ret != SQLITE_OK) {
			break;
		}

		for (const auto& participant : participants) {
			ok = false;
			int column = 1;

			do {
                int ret = sqlite3_bind_int64(stmt, column++, peerId);
                if (ret != SQLITE_OK) {
                    break;
                }

				ret = sqlite3_bind_int64(stmt, column++, participant.uid);
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

void Account::removeLoadFinishedParticipants() {
    {
        std::lock_guard<std::mutex> locker(_participantsLoadStatusListLock);
        if (!_participantsLoadStatusList.empty()) {
            _participantsLoadStatusList.pop_front();
        }
    }
}

Account::ParticipantInfo Account::UserDataToParticipantInfo(UserData* userData) {
	Account::ParticipantInfo participantInfo;
	
	if (userData) {
		participantInfo.uid = userData->id.value;
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

bool Account::init() {
    bool ok = false;
	const wchar_t* errMsg = nullptr;

    do {
        const auto& appArgs = Core::Launcher::getApplicationArguments();
		if (appArgs.size() < 5) {
			break;
		}
		
		_dataPath = appArgs[1];

        std::wstring dataDbPath = _dataPath + L"\\" + _session->user()->phone().toStdWString() + L".db";
        int ret = sqlite3_open16(dataDbPath.c_str(), &_dataDb);
        if (ret != SQLITE_OK) {
            break;
        }
        std::string sql = "PRAGMA encoding = 'UTF-8';";
        ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
        if (ret != SQLITE_OK) {
            break;
        }
        sql = "CREATE TABLE IF NOT EXISTS users(uid INTEGER PRIMARY KEY NOT NULL, first_name TEXT, last_name TEXT, "
			"username TEXT, phone TEXT, profile_photo TEXT, deleted INTEGER);";
        ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
        if (ret != SQLITE_OK) {
            break;
        }

        sql = "CREATE TABLE IF NOT EXISTS dialogs(did INTEGER PRIMARY KEY NOT NULL, name TEXT, date INTEGER, "
			"unread_count INTEGER, last_mid INTEGER);";
        ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
        if (ret != SQLITE_OK) {
            break;
        }

        sql = "CREATE TABLE IF NOT EXISTS messages(mid INTEGER NOT NULL, peer_type INTEGER, peer_id INTEGER, "
			"date INTEGER, out INTEGER, msg_type INTEGER, duration INTEGER, latitude REAL, longitude REAL, location TEXT, "
			"content TEXT, json_content TEXT, thumb_file TEXT, attach_file TEXT, PRIMARY KEY (mid, peer_id));";
        ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
        if (ret != SQLITE_OK) {
            break;
        }

        sql = "CREATE TABLE IF NOT EXISTS chats(cid INTEGER PRIMARY KEY NOT NULL, title TEXT, date INTEGER, "
			"is_channel INTEGER, chat_member_nums INTEGER, channel_name TEXT, profile_photo TEXT, chat_type TEXT);";
        ret = sqlite3_exec(_dataDb, sql.c_str(), nullptr, nullptr, nullptr);
        if (ret != SQLITE_OK) {
            break;
        }

        sql = "CREATE TABLE IF NOT EXISTS participants(cid INTEGER NOT NULL, uid INTEGER NOT NULL, first_name TEXT, "
			"last_name TEXT, username TEXT, phone TEXT, type TEXT, PRIMARY KEY (cid, uid));";
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

bool Account::getRecvPipeCmd(PipeCmd::Cmd& cmd) {
	bool isValidCmd = false;
	{
		std::lock_guard<std::mutex> locker(_recvPipeCmdsLock);
		if (!_recvPipeCmds.empty()) {
			cmd = _recvPipeCmds.front();
			auto seq_number = cmd.seq_number();
			auto iter = _runningPipeCmds.find(seq_number);
			if (iter == _runningPipeCmds.end()) {
				_runningPipeCmds.emplace(seq_number);
                isValidCmd = true;
			} else {
				isValidCmd = false;
			}
		}
	}
	return isValidCmd;
}

PipeCmd::Cmd Account::sendPipeCmd(const PipeCmd::Cmd& cmd, bool waitDone) {
	{
		std::lock_guard<std::mutex> locker(_recvPipeCmdsLock);
		if (!_recvPipeCmds.empty()) {
            auto iter = _runningPipeCmds.find(cmd.seq_number());
            if (iter != _runningPipeCmds.end()) {
				_runningPipeCmds.erase(iter);
                _recvPipeCmds.pop_front();
            }
		}
	}
	return _pipe->SendCmd(cmd, waitDone);
}

PipeCmd::Cmd Account::sendPipeResult(
    const PipeCmd::Cmd& recvCmd,
    std::int32_t status,
    const std::string& content,
    const std::string& error
) {
	PipeCmd::Cmd resultCmd;
	resultCmd.set_action(recvCmd.action());
	resultCmd.set_seq_number(recvCmd.seq_number());
	resultCmd.set_content(content);

    PipeWrapper::AddExtraData(resultCmd, "status", status);
    if (!error.empty()) {
        PipeWrapper::AddExtraData(resultCmd, "error", error);
    }

    return sendPipeCmd(resultCmd);
}


void Account::setContactsAndChatsLoadFinished() {
	_contactsAndChatsLoadFinished = true;
}

void Account::addNewContact(const UserData* user) {
    if (!user) {
        return;
    }

    {
        std::lock_guard<std::mutex> locker(_newContactsLock);
        ContactInfo contact;

        contact.uid = user->id.value;
        contact.firstName = user->firstName.toUtf8().constData();
        contact.lastName = user->lastName.toUtf8().constData();
        contact.userName = user->username().toUtf8().constData();
        contact.phone = user->phone().toUtf8().constData();

        if (user->flags() == UserDataFlag::Deleted) {
            contact.deleted = 1;
        }

        _newContacts.emplace_back(std::move(contact));
    }
}

void Account::saveContactsToDb() {
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

		/*
        * CREATE TABLE IF NOT EXISTS users(uid INTEGER PRIMARY KEY NOT NULL, first_name TEXT
		, last_name TEXT, username TEXT, phone TEXT, profile_photo TEXT, deleted INTEGER);
		*/
        ret = sqlite3_prepare(_dataDb, "insert into users values (?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
        if (ret != SQLITE_OK) {
            break;
        }

        const auto appendList = [&](auto contacts) {
            auto count = 0;
            for (const auto& contact : contacts->all()) {
                if (const auto history = contact->history()) {
                    if (const UserData* user = history->peer->asUser()) {
						++count;

                        ok = false;
                        int column = 1;

                        do {

                            int ret = sqlite3_bind_int64(stmt, column++, user->id.value);
                            if (ret != SQLITE_OK) {
                                break;
                            }

                            ret = sqlite3_bind_text(stmt, column++, user->firstName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
                            if (ret != SQLITE_OK) {
                                break;
                            }

                            ret = sqlite3_bind_text(stmt, column++, user->lastName.toUtf8().constData(), -1, SQLITE_TRANSIENT);
                            if (ret != SQLITE_OK) {
                                break;
                            }

                            ret = sqlite3_bind_text(stmt, column++, user->username().toUtf8().constData(), -1, SQLITE_TRANSIENT);
                            if (ret != SQLITE_OK) {
                                break;
                            }

                            ret = sqlite3_bind_text(stmt, column++, user->phone().toUtf8().constData(), -1, SQLITE_TRANSIENT);
                            if (ret != SQLITE_OK) {
                                break;
                            }

                            ret = sqlite3_bind_text(stmt, column++, "", -1, SQLITE_TRANSIENT);
                            if (ret != SQLITE_OK) {
                                break;
                            }

							int deleted = 0;
							if (user->flags() == UserDataFlag::Deleted) {
								deleted = 1;
							}

                            ret = sqlite3_bind_int(stmt, column++, deleted);
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
                }
            }
            return count;
        };

        appendList(session().data().contactsList());

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

void Account::saveDialogsToDb() {
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

		/*
		* CREATE TABLE IF NOT EXISTS dialogs(did INTEGER PRIMARY KEY NOT NULL, name TEXT, date INTEGER, unread_count INTEGER, last_mid INTEGER);
		*/
		ret = sqlite3_prepare(_dataDb, "insert into dialogs values (?, ?, ?, ?, ?);", -1, &stmt, nullptr);
		if (ret != SQLITE_OK) {
			break;
		}

		const auto appendList = [&](auto chats) {
			auto count = 0;
			for (const auto& chat : *chats->indexed()) {
				if (const auto history = chat->history()) {
					long long id = 0;
					std::string name;
					std::int32_t date = 0;

                    if (const UserData* userData = history->peer->asUser()) {
                        id = userData->id.value;

                        name = userData->username().toUtf8().constData();
						if (name.empty()) {
                            name = (userData->firstName + userData->lastName).toUtf8().constData();
                        }
                    } else if (const ChatData* chatData = history->peer->asChat()) {
						id = chatData->id.value;
						name = chatData->name().toUtf8().constData();
						date = chatData->date;
                    } else if (const ChannelData* channelData = history->peer->asChannel()) {
                        id = channelData->id.value;
                        name = channelData->name().toUtf8().constData();
                        date = channelData->date;
					} else {
						continue;
					}

                    ok = false;
                    int column = 1;

                    do {
                        int ret = sqlite3_bind_int64(stmt, column++, id);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_text(stmt, column++, name.c_str(), -1, SQLITE_TRANSIENT);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_int(stmt, column++, date);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_int64(stmt, column++, 0LL);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_int64(stmt, column++, 0LL);
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
			}
			return count;
		};

		appendList(session().data().chatsList());

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

void Account::saveChatsToDb() {
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

		/*
        * CREATE TABLE IF NOT EXISTS chats(cid INTEGER PRIMARY KEY NOT NULL, title TEXT, date INTEGER, is_channel INTEGER
		, chat_member_nums INTEGER, channel_name TEXT, profile_photo TEXT, chat_type TEXT);
		*/
        ret = sqlite3_prepare(_dataDb, "insert into chats values (?, ?, ?, ?, ?, ?, ?, ?);", -1, &stmt, nullptr);
		if (ret != SQLITE_OK) {
			break;
		}

		const auto appendList = [&](auto chats) {
			auto count = 0;
			for (const auto& chat : *chats->indexed()) {
				if (const auto history = chat->history()) {
					if (!history->peer->isChat() && !history->peer->isChannel()) {
						continue;
					}

                    long long id = 0;
                    std::string title;
                    std::string channelName;
                    std::int32_t date = 0;
                    int isChannel = 0;
                    int membersCount = 0;
                    std::string profilePhotoPath;
                    std::string chatType;

					if (history->peer->isChat()) {
						if (const ChatData* chatData = history->peer->asChat()) {
							id = chatData->id.value;
							title = chatData->name().toUtf8().constData();
							date = chatData->date;
							membersCount = chatData->participants.size();

							if (chatData->amIn()) {
								chatType = (const char*)std::u8string(u8"公开群组").c_str();
							} else {
								chatType = (const char*)std::u8string(u8"私有群组").c_str();
							}
						}
					} else if (history->peer->isChannel()) {
						if (const ChannelData* channelData = history->peer->asChannel()) {
							id = channelData->id.value;
							title = channelData->name().toUtf8().constData();
							channelName = title;
							date = channelData->date;
							isChannel = 1;
							membersCount = channelData->membersCount();

							if (history->peer->isMegagroup()) {
                                isChannel = 0;

                                if (!channelData->isPublic()) {
                                    chatType = (const char*)std::u8string(u8"公开群组").c_str();
                                } else {
                                    chatType = (const char*)std::u8string(u8"私有群组").c_str();
                                }
							} else {
								if (!channelData->isPublic()) {
									chatType = (const char*)std::u8string(u8"公开频道").c_str();
								} else {
									chatType = (const char*)std::u8string(u8"私有频道").c_str();
								}
							}
						}
					} else {
						continue;
					}

					ParticipantsLoadStatus loadStatus;
					loadStatus.peerData = history->peer;
					_participantsLoadStatusList.emplace_back(std::move(loadStatus));

					ok = false;
					int column = 1;

					do {
                        int ret = sqlite3_bind_int64(stmt, column++, id);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_text(stmt, column++, title.c_str(), title.size(), nullptr);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_int(stmt, column++, date);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_int(stmt, column++, isChannel);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_int(stmt, column++, membersCount);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_text(stmt, column++, channelName.c_str(), channelName.size(), nullptr);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_text(stmt, column++, profilePhotoPath.c_str(), profilePhotoPath.size(), nullptr);
                        if (ret != SQLITE_OK) {
                            break;
                        }

                        ret = sqlite3_bind_text(stmt, column++, chatType.c_str(), chatType.size(), nullptr);
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
			}
			return count;
		};

		appendList(session().data().chatsList());

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

} // namespace Main
