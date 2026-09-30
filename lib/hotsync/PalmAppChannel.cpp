#include "PalmAppChannel.h"

std::string palm_app_reply(const std::string &request, const PalmAppStatus &status)
{
    if (request != "FUJI PING")
        return std::string();
    return "FUJI OK|Firmware " + status.version + "|IP " + status.ip + "|WiFi " + status.ssid +
           "|" + status.time + "\r\n";
}
