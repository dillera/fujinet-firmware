#include "DatebookConduit.h"

#include "../../include/debug.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

static constexpr const char *MAP_NAME = "datebook.map";
static constexpr const char *MAP_VERSION = "datebook-map 1";
static constexpr int64_t SECONDS_PER_DAY = 86400;
// How long a record of a past event stays in the map once it leaves the window.
static constexpr int64_t MAP_KEEP_PAST = 60 * SECONDS_PER_DAY;

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

// Uids are written percent-encoded, so spaces and line breaks survive.
static std::string encode_uid(const std::string &uid)
{
    static const char *HEX = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : uid)
    {
        if (c <= ' ' || c == '%' || c >= 0x7F)
        {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0x0F];
        }
        else
            out += static_cast<char>(c);
    }
    return out;
}

static std::string decode_uid(const std::string &text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            out += static_cast<char>(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        }
        else
            out += text[i];
    }
    return out;
}

success_is_true DatebookConduit::load_map(std::vector<Mapping> &map)
{
    map.clear();
    ByteBuffer data;
    if (_storage.read_state(_user, MAP_NAME, data).is_error())
        RETURN_ERROR_AS_FALSE();
    if (data.empty())
        RETURN_SUCCESS_AS_TRUE(); // never synced
    std::istringstream in(std::string(data.begin(), data.end()));
    std::string line;
    if (!std::getline(in, line) || line != MAP_VERSION)
        RETURN_ERROR_AS_FALSE();
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        Mapping m;
        unsigned long id, hash;
        long long start;
        std::string uid;
        if (!(fields >> std::hex >> id >> hash >> std::dec >> start >> uid))
            continue;
        m.record_id = static_cast<uint32_t>(id);
        m.hash = static_cast<uint32_t>(hash);
        m.start = start;
        m.uid = decode_uid(uid);
        map.push_back(m);
    }
    RETURN_SUCCESS_AS_TRUE();
}

success_is_true DatebookConduit::save_map(const std::vector<Mapping> &map)
{
    std::ostringstream out;
    out << MAP_VERSION << "\n";
    for (const Mapping &m : map)
        out << std::hex << m.record_id << " " << m.hash << " " << std::dec << m.start << " "
            << encode_uid(m.uid) << "\n";
    std::string text = out.str();
    return _storage.write_state(_user, MAP_NAME, ByteBuffer(text.begin(), text.end()));
}

// Whether a ReadRecord result means the record is no longer on the Palm.
static bool gone_from_palm(DlpError read, const DlpRecord &record)
{
    return read == DlpError::NOT_FOUND || read == DlpError::RECORD_DELETED ||
           (read == DlpError::NONE && (record.attributes & DLP_RECORD_DELETED));
}

DlpError DatebookConduit::sync(const std::vector<HotSyncEvent> &events,
                               const DatebookSyncOptions &options, DatebookSyncReport &report)
{
    // Without the map, adding events would copy them all again.
    std::vector<Mapping> old_map;
    if (load_map(old_map).is_error())
    {
        Debug_printf("HotSync: cannot read %s, Date Book left alone\r\n", MAP_NAME);
        return DlpError::NOT_FOUND;
    }

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

    std::vector<Mapping> map;
    size_t next = 0;
    for (; next < old_map.size() && err == DlpError::NONE; ++next)
    {
        Mapping m = old_map[next];
        auto found = by_uid.find(m.uid);
        bool listed = found != by_uid.end() && !handled[found->second];
        bool in_window = m.start >= options.from && m.start < options.to;
        if (!listed && !in_window)
        {
            // Not fetched this time, so not known to be cancelled. Long past
            // records are forgotten; they stay on the Palm.
            if (m.start >= options.from - MAP_KEEP_PAST)
                map.push_back(m);
            continue;
        }

        DlpRecord record;
        DlpError read = _dlp.read_record_by_id(db, m.record_id, record);
        if (read == DlpError::TRANSPORT)
        {
            err = read;
            map.push_back(m);
            break;
        }
        bool ours = read == DlpError::NONE && fnv1a(record.data) == m.hash;

        if (!listed)
        {
            // Cancelled: delete the record unless it was changed on the Palm.
            if (ours)
            {
                DlpError del = _dlp.delete_record(db, m.record_id);
                if (del == DlpError::TRANSPORT)
                    err = del;
                if (del != DlpError::NONE)
                {
                    map.push_back(m); // try again next time
                    continue;
                }
                report.deleted++;
            }
            continue;
        }

        size_t i = found->second;
        if (gone_from_palm(read, record))
            continue; // deleted on the Palm; added again below
        handled[i] = true;
        if (read != DlpError::NONE)
        {
            map.push_back(m); // unreadable for now; leave it be
            continue;
        }
        if (hashes[i] != m.hash && ours)
        {
            DlpRecord rewrite;
            rewrite.id = m.record_id;
            rewrite.data = packed[i];
            uint32_t id = 0;
            err = _dlp.write_record(db, rewrite, id);
            if (err == DlpError::NONE)
            {
                m.hash = hashes[i];
                report.updated++;
            }
        }
        else
            report.unchanged++; // the same, or edited on the Palm and kept
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

    if (save_map(map).is_error())
        Debug_printf("HotSync: could not save %s\r\n", MAP_NAME);
    DlpError closed = _dlp.close_db(db);
    return err != DlpError::NONE ? err : closed;
}
