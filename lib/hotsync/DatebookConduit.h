#ifndef HOTSYNC_DATEBOOK_CONDUIT_H
#define HOTSYNC_DATEBOOK_CONDUIT_H

// Copies calendar events into the Palm Date Book, one way. Each event FujiNet
// adds is remembered (record ID and contents) in state/<user>/datebook.map, so
// a later sync updates or removes only those records:
//   - an event that changed is rewritten, even over an edit made on the Palm;
//   - an event that is gone is deleted, unless it was edited on the Palm;
//   - a record deleted on the Palm is added again while the event lasts.
// Appointments made on the Palm are never touched.

#include "Datebook.h"
#include "DlpClient.h"
#include "HotSyncCalendar.h"
#include "HotSyncStorage.h"

#include "../utils/fn_time.h"

#include <string>
#include <vector>

struct DatebookSyncOptions {
    // The window the events were fetched for, UTC. Records of events outside
    // it are left alone, since the calendar did not say whether they remain.
    int64_t from = 0;
    int64_t to = 0;
    // The Palm's wall clock, for timed events.
    fn_time::PosixTz tz;
};

struct DatebookSyncReport {
    int added = 0;
    int updated = 0;
    int deleted = 0;
    int unchanged = 0;
};

class DatebookConduit
{
public:
    DatebookConduit(DlpClient &dlp, HotSyncStorage &storage, const std::string &user)
        : _dlp(dlp), _storage(storage), _user(user)
    {
    }

    DlpError sync(const std::vector<HotSyncEvent> &events, const DatebookSyncOptions &options,
                  DatebookSyncReport &report);

    static PalmAppointment appointment_for(const HotSyncEvent &event, const fn_time::PosixTz &tz);

private:
    struct Mapping {
        uint32_t record_id;
        uint32_t hash;
        int64_t start;
        std::string uid;
    };

    std::vector<Mapping> load_map();
    void save_map(const std::vector<Mapping> &map);

    DlpClient &_dlp;
    HotSyncStorage &_storage;
    std::string _user;
};

// Builds the zone a Palm keeps from the offset between its clock and UTC,
// rounded to 15 minutes; for when FujiNet has no timezone configured.
fn_time::PosixTz datebook_zone_from_offset(int offset_seconds);

#endif // HOTSYNC_DATEBOOK_CONDUIT_H
