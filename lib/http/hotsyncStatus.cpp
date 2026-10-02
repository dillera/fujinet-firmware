// The HotSync panel's status, shared by the ESP32 and FujiNet-PC web servers.

#include "httpService.h"

#include "HotSyncService.h"

#include <cJSON.h>

#include <cstdlib>
#include <ctime>

std::string fnHttpService::hotsync_status_json()
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "running", hotsync != nullptr && hotsync->running());
    if (hotsync != nullptr)
    {
        HotSyncCalendarStatus cal = hotsync->calendar_status();
        cJSON_AddStringToObject(j, "calendar", cal.source.c_str());
        cJSON_AddBoolToObject(j, "fetching", cal.fetching);
        cJSON_AddNumberToObject(j, "events", cal.events);
        // Seconds since the last good fetch, -1 if there has been none.
        // SNTP can set the clock back after a fetch, so an age is never negative.
        double ago = (double)(time(nullptr) - cal.fetched_at);
        cJSON_AddNumberToObject(j, "fetched_ago", cal.fetched_at ? (ago < 0 ? 0 : ago) : -1);
        cJSON_AddStringToObject(j, "error", cal.error.c_str());
        cJSON_AddNumberToObject(j, "next_fetch_in", cal.next_fetch_in);
        cJSON_AddStringToObject(j, "last_sync", hotsync->last_result().c_str());
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    std::string out = s ? s : "{}";
    free(s);
    return out;
}
