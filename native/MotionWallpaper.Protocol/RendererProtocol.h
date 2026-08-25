#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace motion::protocol
{
    enum class AckChannel { Unknown, Target, Control };

    struct Ack
    {
        AckChannel channel{ AckChannel::Unknown };
        uint64_t revision{};
        std::string state;
    };

    struct DecodeStatus
    {
        std::string path;
        std::string reason;
    };

    [[nodiscard]] inline Ack parse_ack(std::string_view value)
    {
        std::istringstream input{ std::string(value) };
        std::string tag;
        std::string channel;
        Ack result;
        if (!(input >> tag >> channel >> result.revision >> result.state) || tag != "ack" || !result.revision) return {};
        if (channel == "target") result.channel = AckChannel::Target;
        else if (channel == "control") result.channel = AckChannel::Control;
        else return {};
        return result;
    }

    [[nodiscard]] inline DecodeStatus parse_decode_status(std::string_view value)
    {
        std::istringstream input{ std::string(value) };
        std::string tag;
        std::string channel;
        DecodeStatus result;
        if (!(input >> tag >> channel >> result.path >> result.reason) ||
            tag != "status" || channel != "decode") return {};
        std::string trailing;
        if (input >> trailing) return {};
        return result;
    }
}
