#include "HotSyncNetCalendar.h"

#include "../network-protocol/Calendar.h"
#include "../network-protocol/NetworkProtocolFactory.h"
#include "../utils/fn_time.h"
#include "../utils/peoples_url_parser.h"

#include "../../include/debug.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

// Enough for a busy two months; each raw entry is 277 bytes.
static constexpr int MAX_EVENTS = 400;
static constexpr int64_t SECONDS_PER_DAY = 86400;

static std::string url_encode(const std::string &s)
{
    static const char *HEX = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0x0F];
        }
    }
    return out;
}

static std::string field(const char *text, size_t size)
{
    return std::string(text, strnlen(text, size));
}

success_is_true HotSyncNetCalendar::fetch(int64_t from, int64_t to, std::vector<HotSyncEvent> &out)
{
    size_t colon = _source.find("://");
    if (colon == std::string::npos)
        RETURN_ERROR_AS_FALSE();
    std::string scheme = _source.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string base = _source;
    if (base.back() != '/')
        base += '/';

    fn_time::PosixTz tz;
    std::string zone = _timezone.empty() || !tz.parse(_timezone) ? "UTC0" : _timezone;
    int64_t first_day = from / SECONDS_PER_DAY;
    int64_t days = (to - first_day * SECONDS_PER_DAY + SECONDS_PER_DAY - 1) / SECONDS_PER_DAY;
    int y;
    unsigned m, d;
    fn_time::civil_from_days(first_day, y, m, d);
    char view[64];
    std::snprintf(view, sizeof(view), "AGENDA/%04d-%02u-%02u?days=%lld&count=%d&tz=", y, m, d,
                  static_cast<long long>(days), MAX_EVENTS);

    std::string rx, tx, sp, login, password;
    std::unique_ptr<NetworkProtocol> protocol =
        NetworkProtocolFactory::createProtocol(scheme, &rx, &tx, &sp, &login, &password);
    std::unique_ptr<PeoplesUrlParser> url =
        PeoplesUrlParser::parseURL(base + view + url_encode(zone));
    if (!protocol || !url || !url->isValidUrl())
    {
        Debug_printf("HotSync: cannot read calendar \"%s\"\r\n", _source.c_str());
        RETURN_ERROR_AS_FALSE();
    }
    if (protocol->open(url.get(), ACCESS_MODE::DIRECTORY, static_cast<netProtoTranslation_t>(0xFF)) !=
        FUJI_ERROR::NONE)
    {
        Debug_printf("HotSync: calendar fetch failed, status %u\r\n",
                     static_cast<unsigned>(protocol->error));
        protocol->close();
        RETURN_ERROR_AS_FALSE();
    }

    out.clear();
    for (size_t pos = 0; pos + sizeof(CalEventItem) <= rx.size(); pos += sizeof(CalEventItem))
    {
        CalEventItem item;
        std::memcpy(&item, rx.data() + pos, sizeof(item));
        HotSyncEvent e;
        e.uid = field(item.uid, sizeof(item.uid));
        e.summary = field(item.summary, sizeof(item.summary));
        e.location = field(item.location, sizeof(item.location));
        e.start = static_cast<int64_t>(item.start);
        e.end = static_cast<int64_t>(item.end);
        e.all_day = (item.flags & CAL_FLAG_ALLDAY) != 0;
        if (e.all_day)
        {
            // HotSyncEvent dates an all-day event by midnight UTC.
            int64_t last = tz.local_day(e.end > e.start ? e.end - 1 : e.start);
            e.start = tz.local_day(e.start) * SECONDS_PER_DAY;
            e.end = (last + 1) * SECONDS_PER_DAY;
        }
        if (e.uid.empty())
            e.uid = e.summary + "@" + std::to_string(e.start);
        else if (item.flags & CAL_FLAG_RECURRING)
            e.uid += "@" + std::to_string(e.start);
        out.push_back(e);
    }
    protocol->close();
    RETURN_SUCCESS_AS_TRUE();
}
