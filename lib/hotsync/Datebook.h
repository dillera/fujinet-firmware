#ifndef HOTSYNC_DATEBOOK_H
#define HOTSYNC_DATEBOOK_H

// Records of the Palm Date Book's database, DatebookDB.
// Reference: Palm OS SDK DateDB.h, pilot-link libpisock/datebook.c

#include "global_types.h"

#include <cstdint>
#include <string>

constexpr const char *DATEBOOK_DB_NAME = "DatebookDB";

// A calendar date as the Palm stores it.
struct PalmDate {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;

    bool operator==(const PalmDate &o) const
    {
        return year == o.year && month == o.month && day == o.day;
    }
};

// One appointment. Repeats other than daily-until are kept as raw bytes so a
// record read from the device can be compared, but not built from scratch.
struct PalmAppointment {
    PalmDate date;
    // An untimed appointment is an all-day event in the Date Book.
    bool timed = true;
    uint8_t start_hour = 0;
    uint8_t start_minute = 0;
    uint8_t end_hour = 0;
    uint8_t end_minute = 0;
    // Repeats daily through repeat_until when its year is set.
    PalmDate repeat_until;
    // Shown in the Date Book; Palm text, not UTF-8.
    std::string description;
    std::string note;
};

// The Date Book's own limits on its text fields.
constexpr size_t DATEBOOK_MAX_DESCRIPTION = 255;
constexpr size_t DATEBOOK_MAX_NOTE = 4095;

ByteBuffer datebook_pack(const PalmAppointment &appt);
// Reads the fields above; an alarm, exceptions or another kind of repeat are
// skipped, so a record holding them does not round-trip.
success_is_true datebook_unpack(const ByteBuffer &record, PalmAppointment &out);

// UTF-8 to the Palm's Windows-1252 text, with '?' for anything it lacks.
std::string palm_text_from_utf8(const std::string &utf8);

#endif // HOTSYNC_DATEBOOK_H
