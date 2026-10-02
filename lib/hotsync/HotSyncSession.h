#ifndef HOTSYNC_SESSION_H
#define HOTSYNC_SESSION_H

// One HotSync, from the first DLP request to EndOfSync: install queued files,
// copy calendar events into the Date Book, back up databases, then stamp the
// device's user info.
// Reference: palm-sync src/sync-utils/{sync-device,write-db,read-db}.ts

#include "DatebookConduit.h"
#include "DlpClient.h"
#include "HotSyncCalendar.h"
#include "HotSyncStorage.h"
#include "PalmDatabase.h"

#include <string>

enum class HotSyncBackup : uint8_t {
    NONE,
    // Databases whose backup bit is set, which is what Palm Desktop saves.
    FLAGGED,
    ALL,
};

struct HotSyncOptions {
    // Given to a device that has never been synced.
    std::string default_user_name = "FujiNet";
    uint32_t new_user_id = 0;
    // Identifies this FujiNet as the last machine the device synced with.
    uint32_t pc_id = 0;
    HotSyncBackup backup = HotSyncBackup::FLAGGED;
    // Wall-clock time of the sync; a zero year leaves the device's date alone.
    DlpDateTime now;
    // The same moment in seconds since 1970 UTC, or 0 when the clock is unset.
    int64_t utc_now = 0;

    // Events for the Date Book, fetched for [calendar_from, calendar_to);
    // null leaves the Date Book alone.
    const std::vector<HotSyncEvent> *calendar = nullptr;
    int64_t calendar_from = 0;
    int64_t calendar_to = 0;
    // POSIX zone of the Palm's clock. Empty reads it off the Palm's clock.
    std::string timezone;
};

struct HotSyncReport {
    std::string user_name;
    int installed = 0;
    int install_failures = 0;
    int backed_up = 0;
    int backup_failures = 0;
    bool calendar_synced = false;
    DatebookSyncReport datebook;
    DlpError error = DlpError::NONE;
};

class HotSyncSession
{
public:
    HotSyncSession(DlpTransport &transport, HotSyncStorage &storage, const HotSyncOptions &options);

    // The transport must already have completed accept().
    HotSyncReport run();

private:
    DlpError identify_user();
    DlpError install_pending();
    DlpError install_file(const std::string &file_name);
    DlpError write_database(const PalmDatabase &db);
    DlpError write_contents(uint8_t handle, const PalmDatabase &db);
    DlpError sync_datebook();
    DlpError backup_databases();
    DlpError backup_database(const DlpDbInfo &info);
    DlpError read_contents(uint8_t handle, PalmDatabase &db);
    DlpError finish();
    void log(const std::string &line);
    bool is_fatal(DlpError err) const;

    DlpClient _dlp;
    HotSyncStorage &_storage;
    HotSyncOptions _options;
    DlpUserInfo _user;
    bool _user_is_new = false;
    bool _reset_after_sync = false;
    HotSyncReport _report;
};

// Device names can hold characters a FAT file name cannot.
std::string hotsync_safe_name(const std::string &name);
std::string hotsync_backup_file_name(const DlpDbInfo &info);

#endif // HOTSYNC_SESSION_H
