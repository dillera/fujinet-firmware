#ifndef HOTSYNC_CALENDAR_H
#define HOTSYNC_CALENDAR_H

// Calendar events a HotSync copies into the Palm Date Book.

#include "global_types.h"

#include <cstdint>
#include <string>
#include <vector>

struct HotSyncEvent {
    // Unique per occurrence, so each instance of a repeating event is its own.
    std::string uid;
    std::string summary;  // UTF-8
    std::string location; // UTF-8
    // Seconds since 1970 UTC; end is exclusive. An all-day event runs from
    // midnight UTC of its first day to midnight UTC after its last.
    int64_t start = 0;
    int64_t end = 0;
    bool all_day = false;
};

// Where the events come from: Google Calendar or an iCalendar feed in the
// firmware, a list in tests.
class HotSyncCalendar
{
public:
    virtual ~HotSyncCalendar() = default;

    // Events that overlap [from, to), UTC.
    virtual success_is_true fetch(int64_t from, int64_t to, std::vector<HotSyncEvent> &out) = 0;
};

#endif // HOTSYNC_CALENDAR_H
