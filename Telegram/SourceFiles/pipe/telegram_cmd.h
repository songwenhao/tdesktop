#ifndef TELEGRAM_CMD_H
#define TELEGRAM_CMD_H

#include <cstdint>
#include <string>

namespace TelegramCmd {

    enum class Action : std::int32_t {
        Unknown = -1,
        CheckIsLogin = 0,
        SendPhoneCode,
        GenerateQrCode,
        LoginByPhone,
        LoginByQrCode,
        SecondVerify,
        GetContactAndChat,
        GetChatMessage,
        ExportData,
        LogOut
    };

    enum class LoginStatus : std::int32_t {
        UnknownError = -1,
        Success = 0,
        NeedVerify,
        CodeInvalid,
        CodeExpired
    };

}

#endif // TELEGRAM_CMD_H