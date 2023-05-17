/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/style/style_core_types.h"
#include "ui/effects/animations.h"
#include "base/weak_ptr.h"

#include <crl/crl_time.h>
#include <QtCore/QByteArray>
#include <optional>

namespace Ui {

class FrameGenerator;

struct AnimatedIconDescriptor {
	FnMut<std::unique_ptr<FrameGenerator>()> generator;
	QSize sizeOverride;
};

class AnimatedIcon final : public base::has_weak_ptr {
public:
	explicit AnimatedIcon(AnimatedIconDescriptor &&descriptor);
	AnimatedIcon(const AnimatedIcon &other) = delete;
	AnimatedIcon &operator=(const AnimatedIcon &other) = delete;
	AnimatedIcon(AnimatedIcon &&other) = delete; // _animation captures this.
	AnimatedIcon &operator=(AnimatedIcon &&other) = delete;

	[[nodiscard]] bool valid() const;
	[[nodiscard]] int frameIndex() const;
	[[nodiscard]] int framesCount() const;
	[[nodiscard]] QImage frame() const;
	[[nodiscard]] int width() const;
	[[nodiscard]] int height() const;
	[[nodiscard]] QSize size() const;

	struct ResizedFrame {
		QImage image;
		bool scaled = false;
	};
	[[nodiscard]] ResizedFrame frame(
		QSize desiredSize,
		Fn<void()> updateWithPerfect) const;

	void animate(Fn<void()> update);
	void jumpToStart(Fn<void()> update);

	void paint(QPainter &p, int x, int y);
	void paintInCenter(QPainter &p, QRect rect);

	[[nodiscard]] bool animating() const;

private:
	struct Frame;
	class Impl;
	friend class Impl;

	void wait() const;
	[[nodiscard]] int wantedFrameIndex(
		crl::time now,
		const Frame *resolvedCurrent = nullptr) const;
	void preloadNextFrame(
		crl::time now,
		const Frame *resolvedCurrent = nullptr,
		QSize updatedDesiredSize = QSize()) const;
	void frameJumpFinished();
	void continueAnimation(crl::time now);

	std::shared_ptr<Impl> _impl;
	crl::time _animationStartTime = 0;
	crl::time _animationStarted = 0;
	mutable Animations::Simple _animation;
	mutable Fn<void()> _repaint;
	mutable crl::time _animationDuration = 0;
	mutable crl::time _animationCurrentStart = 0;
	mutable crl::time _animationNextStart = 0;
	mutable int _animationCurrentIndex = 0;

};

[[nodiscard]] std::unique_ptr<AnimatedIcon> MakeAnimatedIcon(
	AnimatedIconDescriptor &&descriptor);

} // namespace Ui
