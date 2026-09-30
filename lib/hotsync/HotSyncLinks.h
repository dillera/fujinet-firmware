#ifndef HOTSYNC_LINKS_H
#define HOTSYNC_LINKS_H

// HotSyncLink adapters for FujiNet's own socket and serial classes.

#include "HotSyncLink.h"

#include "fnTcpClient.h"

#include <chrono>
#include <functional>

class IOChannel;

class HotSyncTcpLink : public HotSyncLink
{
public:
    explicit HotSyncTcpLink(fnTcpClient client);
    ~HotSyncTcpLink() override;

    int read(uint8_t *buf, size_t len, uint32_t timeout_ms) override;
    int write(const uint8_t *buf, size_t len) override;

private:
    fnTcpClient _client;
};

// A cradle wired to a UART (or a USB serial adapter on FujiNet-PC).
class HotSyncSerialLink : public HotSyncLink
{
public:
    using BaudSetter = std::function<void(uint32_t)>;

    HotSyncSerialLink(IOChannel &channel, BaudSetter set_baud)
        : _channel(channel), _set_baud(std::move(set_baud)) {}

    int read(uint8_t *buf, size_t len, uint32_t timeout_ms) override;
    int write(const uint8_t *buf, size_t len) override;
    void set_baud_rate(uint32_t baud) override;

    // Bytes read since the last call, for diagnosing a silent cradle.
    size_t take_received_count();

    // After this, reads report a timeout even while bytes keep arriving, so
    // line noise cannot hold a listening window open.
    void set_read_deadline(std::chrono::steady_clock::time_point deadline);
    void clear_read_deadline();

private:
    IOChannel &_channel;
    BaudSetter _set_baud;
    size_t _received = 0;
    bool _has_deadline = false;
    std::chrono::steady_clock::time_point _deadline;
};

#endif // HOTSYNC_LINKS_H
