#ifdef BUILD_RS232

#include "rs232HotSync.h"

#include "PadpTransport.h"
#include "fnSystem.h"

#include <algorithm>
#include <chrono>

// Palm apps pause between requests, some refreshing every few tens of
// seconds; a HotSync start cuts this short (see host_active).
static constexpr unsigned long HOST_QUIET_MS = 60000;
// The bus serves the line once per main-loop pass; a write waits for the whole
// buffer to leave, and a 1 KB PADP fragment takes over a second at 9600 baud.
static constexpr auto BUS_TURN = std::chrono::milliseconds(5000);

bool rs232HotSync::host_active()
{
    unsigned packets = _packets;
    unsigned stray = _stray;
    unsigned long now = fnSystem.millis();
    if (packets != _seen_packets)
    {
        _seen_packets = packets;
        _last_packet_ms = now;
    }
    else if (stray != _seen_stray)
    {
        // Noise and no packets: the device has left its app to HotSync.
        _last_packet_ms = now - HOST_QUIET_MS;
    }
    _seen_stray = stray;
    return now - _last_packet_ms < HOST_QUIET_MS;
}

success_is_true rs232HotSync::claim()
{
    std::unique_lock<std::mutex> lock(_lock);
    _in.clear();
    _out.clear();
    _wanted_baud = CMP_INITIAL_BAUD_RATE;
    if (!_changed.wait_for(lock, BUS_TURN, [&] { return _served_baud == _wanted_baud; }))
        RETURN_ERROR_AS_FALSE();
    RETURN_SUCCESS_AS_TRUE();
}

void rs232HotSync::release()
{
    std::unique_lock<std::mutex> lock(_lock);
    _wanted_baud = 0;
    _out.clear();
    _changed.wait_for(lock, BUS_TURN, [&] { return _served_baud == 0; });
    _in.clear();
}

int rs232HotSync::read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    std::unique_lock<std::mutex> lock(_lock);
    if (!_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return !_in.empty(); }))
        return 0;
    size_t count = std::min(len, _in.size());
    std::copy_n(_in.begin(), count, buf);
    _in.erase(_in.begin(), _in.begin() + count);
    return static_cast<int>(count);
}

int rs232HotSync::write(const uint8_t *buf, size_t len)
{
    std::unique_lock<std::mutex> lock(_lock);
    _out.insert(_out.end(), buf, buf + len);
    // Returns once the bytes are on the wire, as a UART write and flush would.
    if (!_changed.wait_for(lock, BUS_TURN, [&] { return _out.empty() && !_sending; }))
        return 0;
    return static_cast<int>(len);
}

void rs232HotSync::set_baud_rate(uint32_t baud)
{
    std::unique_lock<std::mutex> lock(_lock);
    _wanted_baud = baud;
    _changed.wait_for(lock, BUS_TURN, [&] { return _served_baud == baud; });
}

uint32_t rs232HotSync::line_wanted()
{
    std::lock_guard<std::mutex> lock(_lock);
    return _wanted_baud;
}

void rs232HotSync::line_received(const uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lock(_lock);
    _in.insert(_in.end(), buf, buf + len);
    _changed.notify_all();
}

size_t rs232HotSync::line_outgoing(uint8_t *buf, size_t len)
{
    std::lock_guard<std::mutex> lock(_lock);
    size_t count = std::min(len, _out.size());
    std::copy_n(_out.begin(), count, buf);
    _out.erase(_out.begin(), _out.begin() + count);
    if (count)
        _sending = true;
    return count;
}

void rs232HotSync::line_served(uint32_t baud)
{
    std::lock_guard<std::mutex> lock(_lock);
    _served_baud = baud;
    _sending = false;
    _changed.notify_all();
}

#endif /* BUILD_RS232 */
