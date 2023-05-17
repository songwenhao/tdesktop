// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "storage/cache/storage_cache_types.h"
#include "base/basic_types.h"
#include <crl/crl_object_on_queue.h>
#include <crl/crl_time.h>
#include <rpl/producer.h>
#include <QtCore/QString>

namespace Storage {
class EncryptionKey;
namespace Cache {
namespace details {
class DatabaseObject;
} // namespace details

class Database {
public:
	using Settings = details::Settings;
	using SettingsUpdate = details::SettingsUpdate;
	Database(const QString &path, const Settings &settings);

	void reconfigure(const Settings &settings);
	void updateSettings(const SettingsUpdate &update);

	void open(EncryptionKey &&key, FnMut<void(Error)> &&done = nullptr);
	void close(FnMut<void()> &&done = nullptr);

	void put(
		const Key &key,
		QByteArray &&value,
		FnMut<void(Error)> &&done = nullptr);
	void get(const Key &key, FnMut<void(QByteArray&&)> &&done);
	void remove(const Key &key, FnMut<void(Error)> &&done = nullptr);

	void putIfEmpty(
		const Key &key,
		QByteArray &&value,
		FnMut<void(Error)> &&done = nullptr);
	void copyIfEmpty(
		const Key &from,
		const Key &to,
		FnMut<void(Error)> &&done = nullptr);
	void moveIfEmpty(
		const Key &from,
		const Key &to,
		FnMut<void(Error)> &&done = nullptr);

	using TaggedValue = details::TaggedValue;
	void put(
		const Key &key,
		TaggedValue &&value,
		FnMut<void(Error)> &&done = nullptr);
	void putIfEmpty(
		const Key &key,
		TaggedValue &&value,
		FnMut<void(Error)> &&done = nullptr);
	void getWithTag(const Key &key, FnMut<void(TaggedValue&&)> &&done);

	void getWithSizes(
		const Key &key,
		std::vector<Key> &&keys,
		FnMut<void(QByteArray&&, std::vector<int>&&)> &&done);

	using Stats = details::Stats;
	using TaggedSummary = details::TaggedSummary;
	rpl::producer<Stats> statsOnMain() const;

	void clear(FnMut<void(Error)> &&done = nullptr);
	void clearByTag(uint8 tag, FnMut<void(Error)> &&done = nullptr);
	void waitForCleaner(FnMut<void()> &&done = nullptr);

	void sync();

	~Database();

private:
	using Implementation = details::DatabaseObject;
	crl::object_on_queue<Implementation> _wrapped;

};

} // namespace Cache
} // namespace Storage
