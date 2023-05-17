// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "base/platform/win/base_url_scheme_win.h"

#include "base/platform/win/base_windows_h.h"

#include <QtCore/QDir>
#include <array>

namespace base::Platform {
namespace {

enum class Mode {
	Check,
	Write,
};

bool OpenRegKey(Mode mode, const QString &key, PHKEY rkey) {
	const auto wkey = key.toStdWString();
	const auto opened = RegOpenKeyEx(
		HKEY_CURRENT_USER,
		wkey.c_str(),
		0,
		KEY_QUERY_VALUE | KEY_WRITE,
		rkey);
	if (opened == ERROR_SUCCESS) {
		return true;
	} else if (mode != Mode::Write) {
		return false;
	}

	const auto created = RegCreateKeyEx(
		HKEY_CURRENT_USER,
		wkey.c_str(),
		0,
		nullptr,
		REG_OPTION_NON_VOLATILE,
		KEY_QUERY_VALUE | KEY_WRITE,
		nullptr,
		rkey,
		nullptr);
	if (created == ERROR_SUCCESS) {
		return true;
	}
	return false;
}

bool RemoveRegKey(const QString &key) {
	const auto wkey = key.toStdWString();
	return (RegDeleteKey(HKEY_CURRENT_USER, wkey.c_str()) == ERROR_SUCCESS);
}

bool SetKeyValue(Mode mode, HKEY rkey, const QString &name, QString value) {
	static_assert(sizeof(WCHAR) == 2);
	static_assert(sizeof(wchar_t) == 2);

	constexpr auto kBufferSize = 4096;
	constexpr auto kBufferByteSize = kBufferSize * 2;
	auto defaultType = DWORD();
	auto defaultByteSize = DWORD(kBufferByteSize);
	auto defaultByteValue = std::array<BYTE, kBufferByteSize>{ 0 };
	const auto wname = name.toStdWString();
	const auto queried = RegQueryValueEx(
		rkey,
		wname.c_str(),
		nullptr,
		&defaultType,
		defaultByteValue.data(),
		&defaultByteSize);
	auto defaultValue = std::array<WCHAR, kBufferSize>{ 0 };
	memcpy(defaultValue.data(), defaultByteValue.data(), kBufferByteSize);
	if ((queried == ERROR_SUCCESS)
		&& (defaultType == REG_SZ)
		&& (defaultByteSize == (value.size() + 1) * 2)
		&& (QString::fromWCharArray(
			defaultValue.data(),
			value.size()) == value)) {
		return true;
	} else if (mode != Mode::Write) {
		return false;
	}

	const auto wvalue = value.toStdWString();
	auto byteValue = std::vector<BYTE>((wvalue.size() + 1) * 2, 0);
	memcpy(byteValue.data(), wvalue.data(), wvalue.size() * 2);
	const auto written = RegSetValueEx(
		rkey,
		wname.c_str(),
		0,
		REG_SZ,
		byteValue.data(),
		byteValue.size());
	if (written == ERROR_SUCCESS) {
		return true;
	}
	return false;
}

bool RemoveKeyValue(HKEY rkey, const QString &name) {
	const auto wname = name.toStdWString();
	return (RegDeleteValue(rkey, wname.c_str()) == ERROR_SUCCESS);
}

bool RegisterLegacy(Mode mode, const UrlSchemeDescriptor &d) {
	auto rkey = HKEY();
	const auto exe = QDir::toNativeSeparators(d.executable);
	const auto keyBase = "Software\\Classes\\" + d.protocol;
	const auto command = '"' + exe + "\" " + d.arguments + " -- \"%1\"";

	return OpenRegKey(mode, keyBase, &rkey)
		&& SetKeyValue(mode, rkey, "URL Protocol", QString())
		&& SetKeyValue(mode, rkey, nullptr, "URL:" + d.protocolName)
		&& OpenRegKey(mode, keyBase + "\\DefaultIcon", &rkey)
		&& SetKeyValue(mode, rkey, nullptr, '"' + exe + ",1\"")
		&& OpenRegKey(mode, keyBase + "\\shell", &rkey)
		&& OpenRegKey(mode, keyBase + "\\shell\\open", &rkey)
		&& OpenRegKey(mode, keyBase + "\\shell\\open\\command", &rkey)
		&& SetKeyValue(mode, rkey, nullptr, command);
}

void UnregisterLegacy(const UrlSchemeDescriptor &d) {
	const auto keyBase = "Software\\Classes\\" + d.protocol;
	RemoveRegKey(keyBase + "\\shell\\open\\command");
	RemoveRegKey(keyBase + "\\shell\\open");
	RemoveRegKey(keyBase + "\\shell");
	RemoveRegKey(keyBase + "\\DefaultIcon");
	RemoveRegKey(keyBase);
}

bool RegisterDefaultProgram(Mode mode, const UrlSchemeDescriptor &d) {
	auto rkey = HKEY();
	const auto exe = QDir::toNativeSeparators(d.executable);
	const auto namedProtocol = d.shortAppName + '.' + d.protocol;
	const auto namedBase = "Software\\Classes\\" + namedProtocol;
	const auto longNamedBase = "Software\\" + d.longAppName;
	const auto registered = "SOFTWARE\\" + d.longAppName + "\\Capabilities";
	const auto command = '"' + exe + "\" " + d.arguments + " -- \"%1\"";

	return OpenRegKey(mode, namedBase, &rkey)
		&& OpenRegKey(mode, namedBase + "\\DefaultIcon", &rkey)
		&& SetKeyValue(mode, rkey, nullptr, '"' + exe + ",1\"")
		&& OpenRegKey(mode, namedBase + "\\shell", &rkey)
		&& OpenRegKey(mode, namedBase + "\\shell\\open", &rkey)
		&& OpenRegKey(mode, namedBase + "\\shell\\open\\command", &rkey)
		&& SetKeyValue(mode, rkey, nullptr, command)
		&& OpenRegKey(mode, longNamedBase, &rkey)
		&& OpenRegKey(mode, longNamedBase + "\\Capabilities", &rkey)
		&& SetKeyValue(mode, rkey, "ApplicationName", d.displayAppName)
		&& SetKeyValue(
			mode,
			rkey,
			"ApplicationDescription",
			d.displayAppDescription)
		&& OpenRegKey(
			mode,
			longNamedBase + "\\Capabilities\\UrlAssociations",
			&rkey)
		&& SetKeyValue(mode, rkey, d.protocol, namedProtocol)
		&& OpenRegKey(mode, "Software\\RegisteredApplications", &rkey)
		&& SetKeyValue(mode, rkey, d.displayAppName, registered);
}

void UnregisterDefaultProgram(const UrlSchemeDescriptor &d) {
	auto rkey = HKEY();
	const auto namedProtocol = d.shortAppName + '.' + d.protocol;
	const auto namedBase = "Software\\Classes\\" + namedProtocol;
	const auto longNamedBase = "Software\\" + d.longAppName;
	if (OpenRegKey(Mode::Check, "Software\\RegisteredApplications", &rkey)) {
		RemoveKeyValue(rkey, d.displayAppName);
	}
	RemoveRegKey(longNamedBase + "\\Capabilities\\UrlAssociations");
	RemoveRegKey(longNamedBase + "\\Capabilities");
	RemoveRegKey(longNamedBase);
	RemoveRegKey(namedBase + "\\shell\\open\\command");
	RemoveRegKey(namedBase + "\\shell\\open");
	RemoveRegKey(namedBase + "\\shell");
	RemoveRegKey(namedBase + "\\DefaultIcon");
	RemoveRegKey(namedBase);
}

bool FullRegister(Mode mode, const UrlSchemeDescriptor &descriptor) {
	return RegisterDefaultProgram(mode, descriptor)
		&& RegisterLegacy(mode, descriptor);
}

} // namespace

bool CheckUrlScheme(const UrlSchemeDescriptor &descriptor) {
	return FullRegister(Mode::Check, descriptor);
}

void RegisterUrlScheme(const UrlSchemeDescriptor &descriptor) {
	FullRegister(Mode::Write, descriptor);
}

void UnregisterUrlScheme(const UrlSchemeDescriptor &descriptor) {
	if (CheckUrlScheme(descriptor)) {
		UnregisterDefaultProgram(descriptor);
		UnregisterLegacy(descriptor);
	}
}

} // namespace base::Platform
