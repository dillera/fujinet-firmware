#ifndef HOTSYNC_PALM_APP_CHANNEL_H
#define HOTSYNC_PALM_APP_CHANNEL_H

// A text-line channel on the cradle for Palm apps that talk to FujiNet,
// served between HotSyncs:
//   Palm    -> FujiNet: "FUJI PING\r\n"
//   FujiNet -> Palm:    "FUJI OK|<line>|<line>...\r\n"
// Each <line> is shown as-is on a 160-pixel screen, so keep them short.

#include <cstdint>
#include <string>

// The FujiNet's usual serial rate; HotSync always opens at 9600 instead.
constexpr uint32_t PALM_APP_BAUD_RATE = 115200;

struct PalmAppStatus {
    std::string version;
    std::string ip;
    std::string ssid;
    std::string time;
};

// The reply to one request line (without its line ending), or an empty
// string when the line is not a request this channel understands.
std::string palm_app_reply(const std::string &request, const PalmAppStatus &status);

#endif // HOTSYNC_PALM_APP_CHANNEL_H
