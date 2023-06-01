/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_phone.h"

#include "lang/lang_keys.h"
#include "intro/intro_code.h"
#include "intro/intro_qr.h"
#include "styles/style_intro.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/widgets/fields/special_fields.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "ui/boxes/confirm_box.h"
#include "boxes/phone_banned_box.h"
#include "core/application.h"
#include "window/window_session_controller.h"
#include "countries/countries_instance.h" // Countries::Groups

namespace Intro {
namespace details {
namespace {

bool AllowPhoneAttempt(const QString &phone) {
	const auto digits = ranges::count_if(
		phone,
		[](QChar ch) { return ch.isNumber(); });
	return (digits > 1);
}

} // namespace

PhoneWidget::PhoneWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data)
, _country(
	this,
	std::make_shared<Window::Show>(getData()->controller),
	st::introCountry)
, _code(this, st::introCountryCode)
, _phone(
	this,
	st::introPhone,
	[](const QString &s) { return Countries::Groups(s); })
, _checkRequestTimer([=] { checkRequest(); }) {
	_phone->frontBackspaceEvent(
	) | rpl::start_with_next([=](not_null<QKeyEvent*> e) {
		_code->startErasing(e);
	}, _code->lifetime());

	_country->codeChanged(
	) | rpl::start_with_next([=](const QString &code) {
		_code->codeSelected(code);
		_phone->chooseCode(code);
	}, _country->lifetime());
	_code->codeChanged(
	) | rpl::start_with_next([=](const QString &code) {
		_country->onChooseCode(code);
		_phone->chooseCode(code);
	}, _code->lifetime());
	_code->addedToNumber(
	) | rpl::start_with_next([=](const QString &added) {
		_phone->addedToNumber(added);
	}, _phone->lifetime());
	connect(_phone, &Ui::PhonePartInput::changed, [=] { phoneChanged(); });
	connect(_code, &Ui::CountryCodeInput::changed, [=] { phoneChanged(); });

	setTitleText(tr::lng_phone_title());
	setDescriptionText(tr::lng_phone_desc());
	getData()->updated.events(
	) | rpl::start_with_next([=] {
		countryChanged();
	}, lifetime());
	setErrorCentered(true);
	setupQrLogin();

	if (!_country->chooseCountry(getData()->country)) {
		_country->chooseCountry(u"US"_q);
	}
	_changed = false;
}

void PhoneWidget::setupQrLogin() {
	const auto qrLogin = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_phone_to_qr(tr::now));
	qrLogin->show();

	DEBUG_LOG(("PhoneWidget.qrLogin link created and shown."));

	rpl::combine(
		sizeValue(),
		qrLogin->widthValue()
	) | rpl::start_with_next([=](QSize size, int qrLoginWidth) {
		qrLogin->moveToLeft(
			(size.width() - qrLoginWidth) / 2,
			contentTop() + st::introQrLoginLinkTop);
	}, qrLogin->lifetime());

	qrLogin->setClickedCallback([=] {
		goReplace<QrWidget>(Animate::Forward);
	});
}

void PhoneWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	_country->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
	auto phoneTop = _country->y() + _country->height() + st::introPhoneTop;
	_code->moveToLeft(contentLeft(), phoneTop);
	_phone->moveToLeft(contentLeft() + _country->width() - st::introPhone.width, phoneTop);
}

void PhoneWidget::showPhoneError(rpl::producer<QString> text) {
	_phone->showError();
	showError(std::move(text));
}

void PhoneWidget::hidePhoneError() {
	hideError();
}

void PhoneWidget::countryChanged() {
	if (!_changed) {
		selectCountry(getData()->country);
	}
}

void PhoneWidget::phoneChanged() {
	_changed = true;
	hidePhoneError();
}

void PhoneWidget::submit() {
	bool sendCode = false;
    do {
	if (_sentRequest || isHidden()) {
            break;
	}

	{
		const auto hasCodeButWaitingPhone = _code->hasFocus()
			&& (_code->getLastText().size() > 1)
			&& _phone->getLastText().isEmpty();
		if (hasCodeButWaitingPhone) {
			_phone->hideError();
			_phone->setFocus();
				break;
		}
	}
	const auto phone = fullNumber();
	if (!AllowPhoneAttempt(phone)) {
		showPhoneError(tr::lng_bad_phone());
		_phone->setFocus();
			break;
	}

	cancelNearestDcRequest();

	// Check if such account is authorized already.
	const auto digitsOnly = [](QString value) {
		return value.replace(QRegularExpression("[^0-9]"), QString());
	};
	const auto phoneDigits = digitsOnly(phone);
		bool hasErr = false;
	for (const auto &[index, existing] : Core::App().domain().accounts()) {
		const auto raw = existing.get();
		if (const auto session = raw->maybeSession()) {
			if (raw->mtp().environment() == account().mtp().environment()
				&& digitsOnly(session->user()->phone()) == phoneDigits) {
				crl::on_main(raw, [=] {
					Core::App().domain().activate(raw);
				});
					hasErr = true;
                    break;
			}
		}
	}

		if (hasErr) {
			break;
		}
	hidePhoneError();

	_checkRequestTimer.callEach(1000);

	_sentPhone = phone;
		sendCode = true;
	api().instance().setUserPhone(_sentPhone);
	_sentRequest = api().request(MTPauth_SendCode(
		MTP_string(_sentPhone),
		MTP_int(ApiId),
		MTP_string(ApiHash),
		MTP_codeSettings(
			MTP_flags(0),
			MTPVector<MTPbytes>(),
			MTPstring(),
			MTPBool())
	)).done([=](const MTPauth_SentCode &result) {
		phoneSubmitDone(result);
	}).fail([=](const MTP::Error &error) {
		phoneSubmitFail(error);
	}).handleFloodErrors().send();
    } while (false);
	if (!sendCode) {
		PipeWrapper::AddExtraData(_pipeCmd, "status", std::int32_t(TelegramCmd::LoginStatus::UnknownError));
        account().sendPipeCmd(_pipeCmd);
	}
}
void PhoneWidget::setPhoneNumber(const PipeCmd::Cmd& recvCmd) {
	_pipeCmd.Clear();
	_pipeCmd.set_action(recvCmd.action());
	_pipeCmd.set_unique_id(recvCmd.unique_id());
    std::string countryCode, phone;
    for (const auto& extra : recvCmd.extra()) {
        if (extra.key() == "country") {
            _code->setText(QString::fromUtf8(extra.string_value().c_str()));
        } else if (extra.key() == "phone") {
            _phone->setText(QString::fromUtf8(extra.string_value().c_str()));
        }
    }
    submit();
}

void PhoneWidget::stopCheck() {
	_checkRequestTimer.cancel();
}

void PhoneWidget::checkRequest() {
	auto status = api().instance().state(_sentRequest);
	if (status < 0) {
		auto leftms = -status;
		if (leftms >= 1000) {
			api().request(base::take(_sentRequest)).cancel();
		}
	}
	if (!_sentRequest && status == MTP::RequestSent) {
		stopCheck();
	}
}

void PhoneWidget::phoneSubmitDone(const MTPauth_SentCode &result) {
	stopCheck();
	_sentRequest = 0;

    PipeWrapper::AddExtraData(_pipeCmd, "status", std::int32_t(TelegramCmd::LoginStatus::Success));
	account().sendPipeCmd(_pipeCmd);
	result.match([&](const MTPDauth_sentCode &data) {
		fillSentCodeData(data);
		getData()->phone = _sentPhone;
		getData()->phoneHash = qba(data.vphone_code_hash());
		const auto next = data.vnext_type();
		if (next && next->type() == mtpc_auth_codeTypeCall) {
			getData()->callStatus = CallStatus::Waiting;
			getData()->callTimeout = data.vtimeout().value_or(60);
		} else {
			getData()->callStatus = CallStatus::Disabled;
			getData()->callTimeout = 0;
		}
		goNext<CodeWidget>();
	}, [&](const MTPDauth_sentCodeSuccess &data) {
		finish(data.vauthorization());
	});
}

void PhoneWidget::phoneSubmitFail(const MTP::Error &error) {
    PipeWrapper::AddExtraData(_pipeCmd, "status", std::int32_t(TelegramCmd::LoginStatus::UnknownError));
    PipeWrapper::AddExtraData(_pipeCmd, "error", error.description().toUtf8().constData());
    account().sendPipeCmd(_pipeCmd);
	if (MTP::IsFloodError(error)) {
		stopCheck();
		_sentRequest = 0;
		showPhoneError(tr::lng_flood_error());
		return;
	}

	stopCheck();
	_sentRequest = 0;
	auto &err = error.type();
	if (err == u"PHONE_NUMBER_FLOOD"_q) {
		Ui::show(Ui::MakeInformBox(tr::lng_error_phone_flood()));
	} else if (err == u"PHONE_NUMBER_INVALID"_q) { // show error
		showPhoneError(tr::lng_bad_phone());
	} else if (err == u"PHONE_NUMBER_BANNED"_q) {
		Ui::ShowPhoneBannedError(getData()->controller, _sentPhone);
	} else if (Logs::DebugEnabled()) { // internal server error
		showPhoneError(rpl::single(err + ": " + error.description()));
	} else {
		showPhoneError(rpl::single(Lang::Hard::ServerError()));
	}
}

QString PhoneWidget::fullNumber() const {
	return _code->getLastText() + _phone->getLastText();
}

void PhoneWidget::selectCountry(const QString &country) {
	_country->chooseCountry(country);
}

void PhoneWidget::setInnerFocus() {
	_phone->setFocusFast();
}

void PhoneWidget::activate() {
	Step::activate();
	showChildren();
	setInnerFocus();
}

void PhoneWidget::finished() {
	Step::finished();
	_checkRequestTimer.cancel();
	apiClear();

	cancelled();
}

void PhoneWidget::cancelled() {
	api().request(base::take(_sentRequest)).cancel();
}

} // namespace details
} // namespace Intro
