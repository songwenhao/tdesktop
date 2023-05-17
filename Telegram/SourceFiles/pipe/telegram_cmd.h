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
        GetMessage,
        ExportData,
        LogOut
    };

    static std::wstring CmdActionToWString(Action action) {
        std::wstring actionString;

        switch (action) {
        case Action::Unknown:
            actionString = L"Unknown";
            break;
        case Action::CheckIsLogin:
            actionString = L"CheckIsLogin";
            break;
        case Action::SendPhoneCode:
            actionString = L"SendPhoneCode";
            break;
        case Action::GenerateQrCode:
            actionString = L"GenerateQrCode";
            break;
        case Action::LoginByPhone:
            actionString = L"LoginByPhone";
            break;
        case Action::LoginByQrCode:
            actionString = L"LoginByQrCode";
            break;
        case Action::SecondVerify:
            actionString = L"SecondVerify";
            break;
        case Action::GetContactAndChat:
            actionString = L"GetContactAndChat";
            break;
        case Action::GetMessage:
            actionString = L"GetMessage";
            break;
        case Action::ExportData:
            actionString = L"ExportData";
            break;
        case Action::LogOut:
            actionString = L"LogOut";
            break;
        default:
            break;
        }

        return actionString;
    }

    enum class LoginStatus {
        UnknownError = -1,
        Success = 0,
        NeedVerify,
        CodeInvalid,
        CodeExpired
    };

}
