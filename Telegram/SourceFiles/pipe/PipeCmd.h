#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <combaseapi.h>

#pragma comment(lib, "Ole32")

namespace PipeCmd {

    inline std::string GenerateUniqueId() {
        std::string uniqueId;
        char buf[256] = { 0 };
        GUID guid = { 0 };

        HRESULT ret = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (SUCCEEDED(::CoCreateGuid(&guid))) {
            _snprintf_s(buf, _countof(buf), _TRUNCATE,
                "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                guid.Data1,
                guid.Data2,
                guid.Data3,
                guid.Data4[0], guid.Data4[1],
                guid.Data4[2], guid.Data4[3],
                guid.Data4[4], guid.Data4[5],
                guid.Data4[6], guid.Data4[7]);
            uniqueId = buf;
        }

        if (uniqueId.empty()) {
            auto duration_since_epoch = std::chrono::system_clock::now().time_since_epoch();
            auto microseconds_since_epoch = std::chrono::duration_cast<std::chrono::microseconds>(duration_since_epoch).count();
            uniqueId = std::to_string(microseconds_since_epoch);
        }

        if (SUCCEEDED(ret)) {
            ::CoUninitialize();
        }

        return uniqueId;
    }

    struct Cmd {
        Cmd() {
            Clear();
        }

        void Clear() {
            action = -1;
            uniqueId = GenerateUniqueId();
            content.clear();
        }

        std::int32_t action;
        std::string uniqueId;
        std::string content;
    };

    inline char* CmdToBlobData(const PipeCmd::Cmd& cmd, std::uint32_t& dataSize) {
        /*
        * | action | uniqueId size | uniqueId | content size | content |
        * | 4 bytes|    4 bytes    |          |    4 bytes   |         |
        */
        dataSize = (std::uint32_t)(sizeof(std::int32_t) + sizeof(std::uint32_t) + cmd.uniqueId.size() + sizeof(std::uint32_t) + cmd.content.size());
        char* data = new char[dataSize]();
        if (data) {
            std::int32_t pos = 0;

            memcpy(data + pos, &cmd.action, sizeof(std::int32_t));
            pos += sizeof(std::int32_t);

            std::uint32_t uniqueIdSize = (std::uint32_t)cmd.uniqueId.size();
            memcpy(data + pos, &uniqueIdSize, sizeof(std::uint32_t));
            pos += sizeof(std::uint32_t);

            memcpy(data + pos, cmd.uniqueId.c_str(), uniqueIdSize);
            pos += (std::uint32_t)cmd.uniqueId.size();

            std::uint32_t contentSize = (std::uint32_t)cmd.content.size();
            memcpy(data + pos, &contentSize, sizeof(std::uint32_t));
            pos += sizeof(std::uint32_t);

            memcpy(data + pos, cmd.content.c_str(), contentSize);
        }

        return data;
    }

    inline bool BlobDataToCmd(PipeCmd::Cmd& cmd, const char* data, std::uint32_t dataSize) {
        bool ok = false;

        cmd.Clear();

        if (data && (dataSize >= sizeof(std::uint32_t))) {
            ok = true;

            std::int32_t pos = 0;

            memcpy(&cmd.action, data + pos, sizeof(std::int32_t));
            pos += sizeof(std::int32_t);

            std::uint32_t uniqueIdSize = 0;
            memcpy(&uniqueIdSize, data + pos, sizeof(std::uint32_t));
            pos += sizeof(std::uint32_t);

            cmd.uniqueId.assign(data + pos, uniqueIdSize);
            pos += uniqueIdSize;

            std::uint32_t contentSize = 0;
            memcpy(&contentSize, data + pos, sizeof(std::uint32_t));
            pos += sizeof(std::uint32_t);

            cmd.content.assign(data + pos, contentSize);
        }

        return ok;
    }

}
