#include "utils.h"
#include "base/platform/win/base_windows_h.h"
#include <shellapi.h>

namespace WinUtils {

    QStringList getApplicationArguments() {
        QStringList args;

        auto count = 0;
        if (const auto list = CommandLineToArgvW(GetCommandLine(), &count)) {
            const auto guard = gsl::finally([&] { LocalFree(list); });
            if (count > 0) {
                args.reserve(count);
                for (auto i = 0; i != count; ++i) {
                    args.push_back(QString::fromWCharArray(list[i]));
                }
            }
        }

        return args;
    }

}