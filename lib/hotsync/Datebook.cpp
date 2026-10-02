#include "Datebook.h"

#include "Dlp.h"

// Flag bits in byte 6 of a record, saying which optional parts follow.
static constexpr uint8_t APPT_ALARM = 0x40;
static constexpr uint8_t APPT_REPEAT = 0x20;
static constexpr uint8_t APPT_NOTE = 0x10;
static constexpr uint8_t APPT_EXCEPTIONS = 0x08;
static constexpr uint8_t APPT_DESCRIPTION = 0x04;

static constexpr uint8_t REPEAT_DAILY = 1;
static constexpr uint8_t NO_TIME = 0xFF;
static constexpr size_t APPT_HEADER = 8;
static constexpr size_t APPT_REPEAT_SIZE = 8;

static uint16_t pack_date(const PalmDate &d)
{
    return static_cast<uint16_t>(((d.year - 1904) << 9) | (d.month << 5) | d.day);
}

static PalmDate unpack_date(uint16_t v)
{
    PalmDate d;
    d.year = static_cast<uint16_t>((v >> 9) + 1904);
    d.month = static_cast<uint8_t>((v >> 5) & 0x0F);
    d.day = static_cast<uint8_t>(v & 0x1F);
    return d;
}

ByteBuffer datebook_pack(const PalmAppointment &appt)
{
    bool repeats = appt.repeat_until.year != 0;
    uint8_t flags = APPT_DESCRIPTION;
    if (repeats)
        flags |= APPT_REPEAT;
    if (!appt.note.empty())
        flags |= APPT_NOTE;

    DlpArgWriter w;
    if (appt.timed)
        w.u8(appt.start_hour).u8(appt.start_minute).u8(appt.end_hour).u8(appt.end_minute);
    else
        w.u8(NO_TIME).u8(NO_TIME).u8(NO_TIME).u8(NO_TIME);
    w.u16(pack_date(appt.date)).u8(flags).u8(0);
    if (repeats)
        w.u8(REPEAT_DAILY).u8(0).u16(pack_date(appt.repeat_until)).u8(1).u8(0).u8(0).u8(0);
    w.cstring(appt.description.substr(0, DATEBOOK_MAX_DESCRIPTION));
    if (!appt.note.empty())
        w.cstring(appt.note.substr(0, DATEBOOK_MAX_NOTE));
    return w.data();
}

success_is_true datebook_unpack(const ByteBuffer &record, PalmAppointment &out)
{
    if (record.size() < APPT_HEADER)
        RETURN_ERROR_AS_FALSE();
    DlpArgReader r(record);
    out = PalmAppointment();
    out.start_hour = r.u8();
    out.start_minute = r.u8();
    out.end_hour = r.u8();
    out.end_minute = r.u8();
    out.timed = !(out.start_hour == NO_TIME && out.start_minute == NO_TIME);
    if (!out.timed)
        out.start_hour = out.start_minute = out.end_hour = out.end_minute = 0;
    out.date = unpack_date(r.u16());
    uint8_t flags = r.u8();
    r.skip(1);

    if (flags & APPT_ALARM)
        r.skip(2);
    if (flags & APPT_REPEAT)
    {
        uint8_t type = r.u8();
        r.skip(1);
        uint16_t until = r.u16();
        uint8_t frequency = r.u8();
        r.skip(3);
        if (type == REPEAT_DAILY && frequency == 1 && until != 0xFFFF)
            out.repeat_until = unpack_date(until);
    }
    if (flags & APPT_EXCEPTIONS)
        r.skip(static_cast<size_t>(r.u16()) * 2);
    if (flags & APPT_DESCRIPTION)
        out.description = r.cstring();
    if (flags & APPT_NOTE)
        out.note = r.cstring();
    RETURN_SUCCESS_IF(!r.failed());
}

// Windows-1252 bytes 0x80-0x9F for the Unicode characters they stand for.
struct Cp1252Extra {
    uint32_t code_point;
    char byte;
};
static const Cp1252Extra CP1252_EXTRAS[] = {
    {0x20AC, '\x80'}, {0x201A, '\x82'}, {0x0192, '\x83'}, {0x201E, '\x84'}, {0x2026, '\x85'},
    {0x2020, '\x86'}, {0x2021, '\x87'}, {0x02C6, '\x88'}, {0x2030, '\x89'}, {0x0160, '\x8A'},
    {0x2039, '\x8B'}, {0x0152, '\x8C'}, {0x017D, '\x8E'}, {0x2018, '\x91'}, {0x2019, '\x92'},
    {0x201C, '\x93'}, {0x201D, '\x94'}, {0x2022, '\x95'}, {0x2013, '\x96'}, {0x2014, '\x97'},
    {0x02DC, '\x98'}, {0x2122, '\x99'}, {0x0161, '\x9A'}, {0x203A, '\x9B'}, {0x0153, '\x9C'},
    {0x017E, '\x9E'}, {0x0178, '\x9F'},
};

std::string palm_text_from_utf8(const std::string &utf8)
{
    std::string out;
    size_t i = 0;
    while (i < utf8.size())
    {
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp;
        size_t extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out += '?'; ++i; continue; }

        bool valid = true;
        for (size_t k = 1; valid && k <= extra; ++k)
        {
            if (i + k >= utf8.size() || (static_cast<uint8_t>(utf8[i + k]) & 0xC0) != 0x80)
                valid = false;
            else
                cp = (cp << 6) | (static_cast<uint8_t>(utf8[i + k]) & 0x3F);
        }
        if (!valid)
        {
            out += '?';
            ++i;
            continue;
        }
        i += extra + 1;

        if (cp == '\r')
            continue;
        if (cp == '\t')
            out += ' ';
        else if (cp == '\n' || (cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF))
            out += static_cast<char>(cp);
        else
        {
            char mapped = '?';
            for (const Cp1252Extra &e : CP1252_EXTRAS)
                if (e.code_point == cp)
                    mapped = e.byte;
            out += mapped;
        }
    }
    return out;
}
