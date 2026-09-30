#include "HotSyncService.h"

#include "HotSyncFsStorage.h"
#include "HotSyncLinks.h"
#include "NetSyncTransport.h"
#include "PadpTransport.h"
#include "PalmAppChannel.h"

#include "fnConfig.h"
#include "fnSystem.h"
#include "fnTcpServer.h"
#include "fnWiFi.h"

#include "UARTChannel.h"
#ifdef ESP_PLATFORM
#include <esp_pthread.h>
#endif

#include "../../include/debug.h"

#include <chrono>
#include <ctime>
#include <random>

static constexpr auto IDLE_POLL = std::chrono::milliseconds(50);
static constexpr uint32_t HOTSYNC_TASK_STACK = 8192;
// Identifies FujiNet as "the PC" a device last synced with ("FJNT").
static constexpr uint32_t HOTSYNC_PC_ID = 0x464A4E54;

// The cradle alternates between listening for a HotSync WAKEUP at 9600 baud
// and for Palm app requests at PALM_APP_BAUD_RATE. A 9600-baud byte read at
// 115200 arrives as a line break the UART drops, so it cannot be detected
// from the faster rate. The device repeats its WAKEUP and the app repeats its
// request for longer than one full cycle.
static constexpr auto CRADLE_HOTSYNC_WINDOW = std::chrono::milliseconds(1500);
static constexpr auto CRADLE_APP_WINDOW = std::chrono::milliseconds(1500);
static constexpr uint32_t CRADLE_APP_READ_MS = 50;
static constexpr size_t PALM_APP_MAX_LINE = 128;

// On FujiNet-PC the cradle is a host serial device; on the ESP32 it is the
// UART the platform bus would otherwise own (serial_port=bus).
static ChannelConfig cradle_channel_config(const std::string &port)
{
    ChannelConfig config;
    config.baud(PALM_APP_BAUD_RATE).readTimeout(10);
#ifdef ESP_PLATFORM
    (void)port;
    // Without an RTS pin the channel skips hardware flow control, which
    // would stall transmit, and still raises CTS so the device may send.
    config.deviceID(FN_UART_BUS).rtsPin(-1);
#else
    config.deviceID(port);
#endif
    return config;
}

// A Palm cradle, time-sliced between HotSync and Palm apps.
struct HotSyncService::SerialCradle {
    UARTChannel channel;
    HotSyncSerialLink link{channel, channel};
    std::unique_ptr<PadpTransport> padp;
    std::string app_line;
    bool listening_for_hotsync = false;
    std::chrono::steady_clock::time_point window_end;

    explicit SerialCradle(const std::string &port)
    {
        channel.begin(cradle_channel_config(port));
        listen_for_hotsync();
    }

    // IOChannel::discardInput() waits for the line to go quiet, which a
    // chattering cradle never does; drop only what has already arrived.
    void drop_pending_input()
    {
        uint8_t scratch[64];
        for (int i = 0; i < 64 && link.read(scratch, sizeof(scratch), 0) > 0; ++i)
            ;
    }
    ~SerialCradle() { channel.end(); }

    void listen_for_hotsync()
    {
        listening_for_hotsync = true;
        window_end = std::chrono::steady_clock::now() + CRADLE_HOTSYNC_WINDOW;
        channel.setBaudrate(CMP_INITIAL_BAUD_RATE);
        drop_pending_input();
        link.take_received_count();
        link.set_read_deadline(window_end);
        padp = std::make_unique<PadpTransport>(link);
    }

    void listen_for_apps()
    {
        listening_for_hotsync = false;
        window_end = std::chrono::steady_clock::now() + CRADLE_APP_WINDOW;
        app_line.clear();
        link.clear_read_deadline();
        channel.setBaudrate(PALM_APP_BAUD_RATE);
        drop_pending_input();
    }

    void next_window()
    {
        if (listening_for_hotsync)
            listen_for_apps();
        else
            listen_for_hotsync();
    }
};


HotSyncServiceConfig hotsync_config_from(fnConfig &config)
{
    HotSyncServiceConfig out;
    out.user_name = config.get_hotsync_user();
    std::string backup = config.get_hotsync_backup();
    out.backup = backup == "none" ? HotSyncBackup::NONE
               : backup == "all"  ? HotSyncBackup::ALL
                                  : HotSyncBackup::FLAGGED;
    out.netsync_port = config.get_hotsync_netsync_port();
    out.emulator_port = config.get_hotsync_emulator_port();
    out.serial_port = config.get_hotsync_serial_port();
    return out;
}

HotSyncService::HotSyncService() = default;

HotSyncService::~HotSyncService()
{
    stop();
}

void HotSyncService::start(const HotSyncServiceConfig &config, FileSystem &fs)
{
    if (running())
        return;
    _config = config;
    _fs = &fs;
    _stopping = false;
#ifdef ESP_PLATFORM
    _owns_bus_uart = _config.serial_port == "bus";
#endif
#ifdef ESP_PLATFORM
    // Core 1 belongs to fnLoop, which never blocks at a higher priority.
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = HOTSYNC_TASK_STACK;
    cfg.thread_name = "hotsync";
    cfg.pin_to_core = 0;
    esp_pthread_set_cfg(&cfg);
#endif
    _thread = std::thread(&HotSyncService::run, this);
#ifdef ESP_PLATFORM
    // Later std::threads must not inherit the HotSync name and core.
    esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);
#endif
    Debug_printf("HotSync: service started, files in %s\r\n", _config.root.c_str());
}

void HotSyncService::stop()
{
    if (!running())
        return;
    _stopping = true;
    _thread.join();
    _owns_bus_uart = false;
}

std::string HotSyncService::last_result()
{
    std::lock_guard<std::mutex> lock(_result_lock);
    return _last_result;
}

void HotSyncService::run()
{
    if (!_config.serial_port.empty())
    {
        _cradle = std::make_unique<SerialCradle>(_config.serial_port);
        Debug_printf("HotSync: cradle open on %s\r\n", _config.serial_port.c_str());
    }
    while (!_stopping)
    {
        if (fnWiFi.connected())
            open_listeners();
        else
            close_listeners();

        poll_listener(_netsync_server.get(), Transport::NETSYNC);
        poll_listener(_emulator_server.get(), Transport::SERIAL_OVER_TCP);
        poll_serial();
        std::this_thread::sleep_for(IDLE_POLL);
    }
    close_listeners();
    _cradle.reset();
}

void HotSyncService::open_listeners()
{
    auto open = [](std::unique_ptr<fnTcpServer> &server, uint16_t port) {
        if (server || port == 0)
            return;
        server = std::make_unique<fnTcpServer>();
        server->begin(port);
        Debug_printf("HotSync: listening on TCP %u\r\n", port);
    };
    open(_netsync_server, _config.netsync_port);
    open(_emulator_server, _config.emulator_port);
}

void HotSyncService::close_listeners()
{
    _netsync_server.reset();
    _emulator_server.reset();
}

void HotSyncService::poll_listener(fnTcpServer *server, Transport transport)
{
    if (server == nullptr || !server->hasClient())
        return;
    HotSyncTcpLink link(server->client());
    sync(link, transport);
}

void HotSyncService::poll_serial()
{
    if (!_cradle)
        return;
    if (std::chrono::steady_clock::now() >= _cradle->window_end)
        _cradle->next_window();
    if (_cradle->listening_for_hotsync)
        poll_cradle_hotsync();
    else
        poll_cradle_app();
}

void HotSyncService::poll_cradle_hotsync()
{
    // accept() gives up after ~1s without a WAKEUP.
    if (_cradle->padp->accept().is_error())
    {
        if (size_t count = _cradle->link.take_received_count())
            Debug_printf("HotSync: %u bytes from the cradle at 9600 baud, no WAKEUP\r\n",
                         static_cast<unsigned>(count));
        return;
    }
    // A sync takes as long as it takes; only the WAKEUP wait is windowed.
    _cradle->link.clear_read_deadline();
    HotSyncFsStorage storage(*_fs, _config.root);
    record(HotSyncSession(*_cradle->padp, storage, session_options()).run());
    _cradle->listen_for_hotsync();
}

void HotSyncService::poll_cradle_app()
{
    uint8_t buf[64];
    int got = _cradle->link.read(buf, sizeof(buf), CRADLE_APP_READ_MS);
    if (got > 0)
        Debug_printf("HotSync: %d bytes from the cradle at %lu baud\r\n", got,
                     static_cast<unsigned long>(PALM_APP_BAUD_RATE));
    for (int i = 0; i < got; ++i)
    {
        if (buf[i] == '\r')
            continue;
        if (buf[i] != '\n')
        {
            if (_cradle->app_line.size() < PALM_APP_MAX_LINE)
                _cradle->app_line += static_cast<char>(buf[i]);
            continue;
        }
        std::string reply = palm_app_reply(_cradle->app_line, app_status());
        Debug_printf("HotSync: Palm app sent \"%s\"\r\n", _cradle->app_line.c_str());
        _cradle->app_line.clear();
        if (!reply.empty())
            _cradle->link.write(reinterpret_cast<const uint8_t *>(reply.data()), reply.size());
    }
}

PalmAppStatus HotSyncService::app_status() const
{
    PalmAppStatus status;
    status.version = fnSystem.get_fujinet_version(true);
    status.ip = fnSystem.Net.get_ip4_address_str();
    status.ssid = fnWiFi.get_current_ssid();

    char when[32] = "Clock not set";
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2020)
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
    status.time = when;
    return status;
}

void HotSyncService::sync(HotSyncLink &link, Transport transport)
{
    std::unique_ptr<DlpTransport> dlp;
    if (transport == Transport::NETSYNC)
        dlp = std::make_unique<NetSyncTransport>(link);
    else
        dlp = std::make_unique<PadpTransport>(link);

    if (dlp->accept().is_error())
    {
        Debug_printf("HotSync: handshake failed\r\n");
        return;
    }
    HotSyncFsStorage storage(*_fs, _config.root);
    record(HotSyncSession(*dlp, storage, session_options()).run());
}

HotSyncOptions HotSyncService::session_options() const
{
    HotSyncOptions options;
    options.default_user_name = _config.user_name;
    options.backup = _config.backup;
    options.pc_id = HOTSYNC_PC_ID;
    options.new_user_id = std::random_device()();

    // Before SNTP sets the clock the date is meaningless; leave the device's.
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2020)
    {
        options.now.year = local.tm_year + 1900;
        options.now.month = local.tm_mon + 1;
        options.now.day = local.tm_mday;
        options.now.hour = local.tm_hour;
        options.now.minute = local.tm_min;
        options.now.second = local.tm_sec;
    }
    return options;
}

void HotSyncService::record(const HotSyncReport &report)
{
    std::string line = "'" + report.user_name + "': " + std::to_string(report.installed) +
                       " installed, " + std::to_string(report.backed_up) + " backed up";
    if (report.install_failures + report.backup_failures > 0)
        line += ", " + std::to_string(report.install_failures + report.backup_failures) + " failed";
    if (report.error != DlpError::NONE)
        line += std::string(" (") + dlp_error_name(report.error) + ")";

    std::lock_guard<std::mutex> lock(_result_lock);
    _last_result = line;
}
