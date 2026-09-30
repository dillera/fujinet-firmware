#ifndef HOTSYNC_BUS_PORT_H
#define HOTSYNC_BUS_PORT_H

#include <cstdint>

class IOChannel;

// The platform bus's serial port, lent to a Palm cradle one HotSync window at
// a time. Between windows the bus serves the device's FujiBus requests on it,
// so a Palm app talks to FujiNet the way any bus host does.
class HotSyncBusPort
{
public:
    virtual ~HotSyncBusPort() = default;

    // Blocks until the bus is between commands, then keeps it off the port
    // until give_back().
    virtual void borrow() = 0;
    virtual void give_back() = 0;

    // True while a host is talking to the bus. A Palm running an app that
    // uses the port cannot HotSync, so the cradle leaves the port alone.
    virtual bool host_active() = 0;

    virtual IOChannel &channel() = 0;
    virtual uint32_t baud_rate() = 0;
    virtual void set_baud_rate(uint32_t baud) = 0;
};

#endif // HOTSYNC_BUS_PORT_H
