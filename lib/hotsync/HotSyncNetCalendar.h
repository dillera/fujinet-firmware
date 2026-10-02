#ifndef HOTSYNC_NET_CALENDAR_H
#define HOTSYNC_NET_CALENDAR_H

// HotSyncCalendar over FujiNet's own calendar protocols, so a HotSync reads
// the same calendars the N: device's GCAL: and ICAL: do.

#include "HotSyncCalendar.h"

#include <string>

class HotSyncNetCalendar : public HotSyncCalendar
{
public:
    // A devicespec without its view, e.g. "GCAL:///", "GCAL://Work/" or
    // "ICAL://example.com/feed.ics", and the POSIX zone that a feed's local
    // times and all-day dates are read in.
    HotSyncNetCalendar(std::string source, std::string timezone)
        : _source(std::move(source)), _timezone(std::move(timezone))
    {
    }

    success_is_true fetch(int64_t from, int64_t to, std::vector<HotSyncEvent> &out) override;

private:
    std::string _source;
    std::string _timezone;
};

#endif // HOTSYNC_NET_CALENDAR_H
