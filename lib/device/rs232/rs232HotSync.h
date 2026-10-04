#ifndef RS232HOTSYNC_H
#define RS232HOTSYNC_H

#include "bus.h"
#include "HotSyncSharedCradle.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

// A Palm cradle on the RS232 bus line. The HotSync service thread uses it as
// a HotSyncSharedCradle; systemBus feeds it from service() and does all I/O
// on the port while the line is lent.
class rs232HotSync : public virtualDevice, public HotSyncSharedCradle
{
public:
    // HotSyncSharedCradle, from the HotSync thread.
    bool host_active() override;
    success_is_true claim() override;
    void release() override;
    int read(uint8_t *buf, size_t len, uint32_t timeout_ms) override;
    int write(const uint8_t *buf, size_t len) override;
    void set_baud_rate(uint32_t baud) override;

    // From systemBus::service().
    void bus_packet_handled() { ++_packets; }
    void bus_stray_bytes(size_t count) { _stray += count; }
    // The baud rate to lend the line at, or 0 to keep it on FujiBus.
    uint32_t line_wanted();
    void line_received(const uint8_t *buf, size_t len);
    size_t line_outgoing(uint8_t *buf, size_t len);
    // After a pass with everything outgoing sent; baud 0 once it is FujiBus again.
    void line_served(uint32_t baud);

private:
    void rs232_process(const FujiBusPacket &packet) override {}

    std::atomic<unsigned> _packets{0};
    std::atomic<unsigned> _stray{0};
    unsigned _seen_packets = 0;
    unsigned _seen_stray = 0;
    unsigned long _last_packet_ms = 0;

    std::mutex _lock;
    std::condition_variable _changed;
    uint32_t _wanted_baud = 0;
    uint32_t _served_baud = 0;
    bool _sending = false;
    std::deque<uint8_t> _in;
    std::deque<uint8_t> _out;
};

#endif /* RS232HOTSYNC_H */
