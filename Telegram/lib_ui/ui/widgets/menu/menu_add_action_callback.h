/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/style/style_core.h"

namespace Ui {
class PopupMenu;
} // namespace Ui

namespace Ui::Menu {

struct MenuCallback final {
public:
	struct Args {
		QString text;
		Fn<void()> handler;
		const style::icon *icon;
		Fn<void(not_null<Ui::PopupMenu*>)> fillSubmenu;
		int addTopShift = 0;
		bool isSeparator = false;
		bool isAttention = false;
	};
	using Callback = Fn<QAction*(Args&&)>;

	explicit MenuCallback(Callback callback);

	QAction *operator()(Args &&args) const;
	QAction *operator()(
		const QString &text,
		Fn<void()> handler,
		const style::icon *icon) const;
private:
	Callback _callback;
};

} // namespace Ui::Menu
