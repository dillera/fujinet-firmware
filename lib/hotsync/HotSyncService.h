#ifndef HOTSYNC_SERVICE_H
#define HOTSYNC_SERVICE_H

// Makes FujiNet a HotSync server. A Palm device syncs with it over:
//   - network HotSync (NetSync, TCP 14238);
//   - serial-over-TCP, which POSE and CloudpilotEmu use for their serial port;
//   - a serial cradle, on the ESP32 bus UART or a FujiNet-PC serial device.
// Each sync installs the files queued in <root>/install and backs databases
// up to <root>/backup/<user>. Between syncs the cradle also serves Palm apps
// (PalmAppChannel.h).

#include "HotSyncSession.h"
#include "PalmAppChannel.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class FileSystem;
class fnTcpServer;
class HotSyncLink;

struct HotSyncServiceConfig {
    std::string root = "/palm";
    std::string user_name = "FujiNet";
    HotSyncBackup backup = HotSyncBackup::FLAGGED;
    // 0 disables a listener.
    uint16_t netsync_port = 14238;
    uint16_t emulator_port = 6416;
    // Serial device for a cradle: a host device path on FujiNet-PC, or "bus"
    // on the ESP32 to take the platform bus UART. Empty disables it.
    std::string serial_port;
};

class HotSyncService
{
public:
    // Defined in the .cpp, where SerialCradle is complete.
    HotSyncService();
    ~HotSyncService();

    void start(const HotSyncServiceConfig &config, FileSystem &fs);
    void stop();
    bool running() const { return _thread.joinable(); }
    // True while the cradle holds the UART the platform bus would use.
    bool owns_bus_uart() const { return _owns_bus_uart; }
    // One line describing the most recent sync, for the web UI.
    std::string last_result();

private:
    enum class Transport { NETSYNC, SERIAL_OVER_TCP, SERIAL };

    void run();
    void open_listeners();
    void close_listeners();
    void poll_listener(fnTcpServer *server, Transport transport);
    void poll_serial();
    void poll_cradle_hotsync();
    void poll_cradle_app();
    PalmAppStatus app_status() const;
    void sync(HotSyncLink &link, Transport transport);
    HotSyncOptions session_options() const;
    void record(const HotSyncReport &report);

    HotSyncServiceConfig _config;
    FileSystem *_fs = nullptr;
    std::thread _thread;
    std::atomic<bool> _stopping{false};
    bool _owns_bus_uart = false;
    std::unique_ptr<fnTcpServer> _netsync_server;
    std::unique_ptr<fnTcpServer> _emulator_server;
    std::mutex _result_lock;
    std::string _last_result = "No HotSync yet";

    struct SerialCradle;
    std::unique_ptr<SerialCradle> _cradle;
};

class fnConfig;
HotSyncServiceConfig hotsync_config_from(fnConfig &config);

#endif // HOTSYNC_SERVICE_H
