#pragma once

#include <cstdint>
#include <string>
#include <chrono>

namespace PipeCmd {

    inline std::string GenerateUniqueId() {
        auto duration_since_epoch = std::chrono::system_clock::now().time_since_epoch();
        auto nanoseconds_since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(duration_since_epoch).count();
        return std::to_string(nanoseconds_since_epoch);
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
