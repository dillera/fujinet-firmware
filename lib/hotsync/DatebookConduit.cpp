#include "DatebookConduit.h"

#include "../../include/debug.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

static constexpr const char *MAP_NAME = "datebook.map";
static constexpr const char *MAP_VERSION = "datebook-map 1";
static constexpr int64_t SECONDS_PER_DAY = 86400;

static uint32_t fnv1a(const ByteBuffer &data)
{
    uint32_t h = 2166136261u;
    for (uint8_t b : data)
        h = (h ^ b) * 16777619u;
    return h;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

static PalmDate palm_date(int64_t day_number)
{
    int y;
    unsigned m, d;
    fn_time::civil_from_days(day_number, y, m, d);
    PalmDate out;
    out.year = static_cast<uint16_t>(y);
    out.month = static_cast<uint8_t>(m);
    out.day = static_cast<uint8_t>(d);
    return out;
}

PalmAppointment DatebookConduit::appointment_for(const HotSyncEvent &event,
                                                 const fn_time::PosixTz &tz)
{
    PalmAppointment appt;
    appt.description = palm_text_from_utf8(event.summary);
    if (appt.description.empty())
        appt.description = "(No title)";
    if (!event.location.empty())
        appt.note = "Location: " + palm_text_from_utf8(event.location);

    if (event.all_day)
    {
        int64_t first = floor_div(event.start, SECONDS_PER_DAY);
        int64_t last = floor_div(event.end - 1, SECONDS_PER_DAY);
        appt.timed = false;
        appt.date = palm_date(first);
        if (last > first)
            appt.repeat_until = palm_date(last);
        return appt;
    }

    int y, h, mi, s, wd;
    unsigned mo, d;
    tz.to_local(event.start, y, mo, d, h, mi, s, wd);
    appt.date = palm_date(fn_time::days_from_civil(y, mo, d));
    appt.start_hour = static_cast<uint8_t>(h);
    appt.start_minute = static_cast<uint8_t>(mi);

    // A Date Book appointment cannot run past midnight, so it stops at 23:59.
    int64_t end = event.end > event.start ? event.end : event.start;
    if (tz.local_day(end) != tz.local_day(event.start))
    {
        appt.end_hour = 23;
        appt.end_minute = 59;
    }
    else
    {
        tz.to_local(end, y, mo, d, h, mi, s, wd);
        appt.end_hour = static_cast<uint8_t>(h);
        appt.end_minute = static_cast<uint8_t>(mi);
    }
    return appt;
}

fn_time::PosixTz datebook_zone_from_offset(int offset_seconds)
{
    int rounded = static_cast<int>(floor_div(offset_seconds + 450, 900) * 900);
    int west = -rounded;
    int magnitude = west < 0 ? -west : west;
    char spec[24];
    std::snprintf(spec, sizeof(spec), "PALM%s%d:%02d", west < 0 ? "-" : "", magnitude / 3600,
                  (magnitude % 3600) / 60);
    fn_time::PosixTz tz;
    tz.parse(spec);
    return tz;
}

std::vector<DatebookConduit::Mapping> DatebookConduit::load_map()
{
    std::vector<Mapping> map;
    ByteBuffer data;
    if (_storage.read_state(_user, MAP_NAME, data).is_error())
        return map;
    std::istringstream in(std::string(data.begin(), data.end()));
    std::string line;
    if (!std::getline(in, line) || line != MAP_VERSION)
        return map;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        Mapping m;
        unsigned long id, hash;
        long long start;
        if (!(fields >> std::hex >> id >> hash >> std::dec >> start))
            continue;
        std::getline(fields >> std::ws, m.uid);
        if (m.uid.empty())
            continue;
        m.record_id = static_cast<uint32_t>(id);
        m.hash = static_cast<uint32_t>(hash);
        m.start = start;
        map.push_back(m);
    }
    return map;
}

void DatebookConduit::save_map(const std::vector<Mapping> &map)
{
    std::ostringstream out;
    out << MAP_VERSION << "\n";
    for (const Mapping &m : map)
        out << std::hex << m.record_id << " " << m.hash << " " << std::dec << m.start << " "
            << m.uid << "\n";
    std::string text = out.str();
    if (_storage.write_state(_user, MAP_NAME, ByteBuffer(text.begin(), text.end())).is_error())
        Debug_printf("HotSync: could not save %s\r\n", MAP_NAME);
}

DlpError DatebookConduit::sync(const std::vector<HotSyncEvent> &events,
                               const DatebookSyncOptions &options, DatebookSyncReport &report)
{
    uint8_t db = 0;
    DlpError err = _dlp.open_db(DATEBOOK_DB_NAME, DLP_OPEN_READ | DLP_OPEN_WRITE, db);
    if (err != DlpError::NONE)
        return err;

    std::vector<ByteBuffer> packed;
    std::vector<uint32_t> hashes;
    std::map<std::string, size_t> by_uid;
    for (size_t i = 0; i < events.size(); ++i)
    {
        packed.push_back(datebook_pack(appointment_for(events[i], options.tz)));
        hashes.push_back(fnv1a(packed.back()));
        by_uid.emplace(events[i].uid, i);
    }
    std::vector<bool> handled(events.size(), false);

    std::vector<Mapping> old_map = load_map();
    std::vector<Mapping> map;
    size_t next = 0;
    for (; next < old_map.size() && err == DlpError::NONE; ++next)
    {
        Mapping m = old_map[next];
        if (m.start < options.from || m.start >= options.to)
        {
            map.push_back(m);
            continue;
        }

        DlpRecord record;
        DlpError read = _dlp.read_record_by_id(db, m.record_id, record);
        if (read == DlpError::NOT_FOUND ||
            (read == DlpError::NONE && (record.attributes & DLP_RECORD_DELETED)))
            continue; // deleted on the Palm
        if (read != DlpError::NONE)
        {
            map.push_back(m);
            if (read == DlpError::TRANSPORT)
                err = read;
            continue;
        }

        auto found = by_uid.find(m.uid);
        if (found == by_uid.end())
        {
            if (fnv1a(record.data) == m.hash)
            {
                err = _dlp.delete_record(db, m.record_id);
                if (err == DlpError::NONE)
                    report.deleted++;
                else if (err != DlpError::TRANSPORT)
                    err = DlpError::NONE;
            }
            continue;
        }

        size_t i = found->second;
        handled[i] = true;
        if (hashes[i] != m.hash)
        {
            DlpRecord rewrite;
            rewrite.id = m.record_id;
            rewrite.data = packed[i];
            uint32_t id = 0;
            err = _dlp.write_record(db, rewrite, id);
            if (err != DlpError::NONE)
            {
                map.push_back(m);
                continue;
            }
            m.hash = hashes[i];
            report.updated++;
        }
        else
            report.unchanged++;
        m.start = events[i].start;
        map.push_back(m);
    }
    // A failed sync keeps what it had not reached yet.
    for (; next < old_map.size(); ++next)
        map.push_back(old_map[next]);

    for (size_t i = 0; i < events.size() && err == DlpError::NONE; ++i)
    {
        if (handled[i] || by_uid[events[i].uid] != i)
            continue;
        DlpRecord record;
        record.data = packed[i];
        uint32_t id = 0;
        err = _dlp.write_record(db, record, id);
        if (err != DlpError::NONE)
            break;
        map.push_back(Mapping{id, hashes[i], events[i].start, events[i].uid});
        report.added++;
    }

    save_map(map);
    DlpError closed = _dlp.close_db(db);
    return err != DlpError::NONE ? err : closed;
}
