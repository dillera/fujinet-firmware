#ifndef HOTSYNC_SHARED_CRADLE_H
#define HOTSYNC_SHARED_CRADLE_H

#include "HotSyncLink.h"
#include "global_types.h"

// A cradle on a serial line that the platform bus owns. The bus lends the line
// to the cradle for a HotSync window and does the I/O; between windows it
// serves FujiBus, so a Palm app reaches FujiNet as any bus host does.
class HotSyncSharedCradle : public HotSyncLink
{
public:
    // True while a host is talking FujiBus. A Palm running an app cannot
    // HotSync, so the cradle leaves the line alone.
    virtual bool host_active() = 0;

    // Waits until the bus has lent the line at CMP_INITIAL_BAUD_RATE.
    virtual success_is_true claim() = 0;
    // Waits until the bus has the line back at its own rate.
    virtual void release() = 0;
};

#endif // HOTSYNC_SHARED_CRADLE_H
