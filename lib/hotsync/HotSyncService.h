#ifndef HOTSYNC_SERVICE_H
#define HOTSYNC_SERVICE_H

// Makes FujiNet a HotSync server. A Palm device syncs with it over:
//   - network HotSync (NetSync, TCP 14238);
//   - serial-over-TCP, which POSE and CloudpilotEmu use for their serial port;
//   - a serial cradle, on a FujiNet-PC serial device or on a bus line it
//     shares (HotSyncSharedCradle.h), where Palm apps then reach FujiNet.
// Each sync installs the files queued in <root>/install and backs databases
// up to <root>/backup/<user>.

#include "HotSyncCalendar.h"
#include "HotSyncSession.h"
#include "HotSyncSharedCradle.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    // Serial device for a cradle: a host device path on FujiNet-PC, or
    // HOTSYNC_BUS_SERIAL_PORT to share the bus line. Empty disables it.
    std::string serial_port;
    // Calendar for the Date Book (see HotSyncNetCalendar); empty disables it.
    std::string calendar;
    int calendar_days_back = 7;
    int calendar_days_ahead = 60;
    // POSIX zone of the Palm's clock; empty or UTC reads it off the Palm.
    std::string timezone;
};

constexpr const char *HOTSYNC_BUS_SERIAL_PORT = "bus";

// What the web UI shows about the calendar fetch.
struct HotSyncCalendarStatus {
    std::string source;      // empty when no calendar is configured
    bool fetching = false;
    int64_t fetched_at = 0;  // UTC of the last good fetch, 0 if none
    int events = 0;          // events from that fetch
    std::string error;       // why the latest fetch failed; empty if it worked
    int next_fetch_in = -1;  // seconds, -1 when none is due (e.g. no Wi-Fi yet)
};

class HotSyncService
{
public:
    // Defined in the .cpp, where SerialCradle is complete.
    HotSyncService();
    ~HotSyncService();

    // shared is the bus line a cradle with serial_port=bus uses; builds
    // whose bus cannot share its line pass nullptr.
    void start(const HotSyncServiceConfig &config, FileSystem &fs,
               HotSyncSharedCradle *shared = nullptr);
    void stop();
    bool running() const { return _thread.joinable(); }
    // One line describing the most recent sync, for the web UI.
    std::string last_result();

    HotSyncCalendarStatus calendar_status();
    // Fetch the calendar as soon as the service thread is free.
    void fetch_calendar_now() { _fetch_now = true; }

private:
    enum class Transport { NETSYNC, SERIAL_OVER_TCP };
    struct SerialCradle;

    void run();
    void open_listeners();
    void close_listeners();
    void poll_listener(fnTcpServer *server, Transport transport);
    void open_cradle();
    void poll_serial();
    void listen_for_hotsync(SerialCradle &cradle);
    void sync(HotSyncLink &link, Transport transport);
    HotSyncOptions session_options() const;
    void refresh_calendar();
    void record(const HotSyncReport &report);

    HotSyncServiceConfig _config;
    FileSystem *_fs = nullptr;
    HotSyncSharedCradle *_shared = nullptr;
    std::thread _thread;
    std::atomic<bool> _stopping{false};
    std::unique_ptr<fnTcpServer> _netsync_server;
    std::unique_ptr<fnTcpServer> _emulator_server;
    std::mutex _result_lock;
    std::string _last_result = "No HotSync yet";

    std::unique_ptr<SerialCradle> _cradle;

    // Fetched ahead of time, so a sync does not keep the Palm waiting.
    std::unique_ptr<HotSyncCalendar> _calendar;
    std::vector<HotSyncEvent> _events;
    int64_t _events_from = 0;
    int64_t _events_to = 0;
    bool _have_events = false;
    std::chrono::steady_clock::time_point _next_calendar_fetch;
    std::atomic<bool> _fetch_now{false};
    HotSyncCalendarStatus _calendar_status; // guarded by _result_lock
};

class fnConfig;
HotSyncServiceConfig hotsync_config_from(fnConfig &config);

#endif // HOTSYNC_SERVICE_H
