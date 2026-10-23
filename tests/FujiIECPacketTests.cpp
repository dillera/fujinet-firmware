#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "bus/iec/FujiIECPacket.h"

static constexpr uint8_t DEV_FUJI = 30;

TEST_CASE("data() and dataAsString() are empty for a command sent with no payload")
{
    // fujinet-lib sends WRITE APPKEY for an empty key as OPCODE_NO_PAYLOAD
    FujiIECPacket packet(DEV_FUJI, CMD::FUJI_WRITE_APPKEY);

    CHECK_FALSE(packet.data().has_value());
    CHECK(packet.data().value_or(ByteBuffer{}).empty());
    CHECK_FALSE(packet.dataAsString().has_value());
}

TEST_CASE("data() holds what is left after the params")
{
    ByteBuffer payload{0x05, 0x01, 'k', 'e', 'y'};
    FujiIECPacket packet(DEV_FUJI, CMD::FUJI_OPEN_APPKEY, payload);

    CHECK(packet.param8(0) == 0x05);
    CHECK(packet.param8(1) == 0x01);
    REQUIRE(packet.data().has_value());
    CHECK(packet.data()->size() == 3);
    CHECK(packet.dataAsString().value_or("") == "key");
}

TEST_CASE("params are little-endian")
{
    ByteBuffer payload{0x1C, 0xE4, 0x05, 0x00};
    FujiIECPacket packet(DEV_FUJI, CMD::FUJI_OPEN_APPKEY, payload);

    CHECK(packet.param16(0) == 0xE41C);
    CHECK(packet.param16(1) == 0x0005);
    REQUIRE(packet.data().has_value());
    CHECK(packet.data()->empty());
}

TEST_CASE("hasPayload() tells a command with a payload from one without")
{
    // fujinet-lib sends STATUS bare, and the clock's GET_SOS (also 0x53) with a flag
    ByteBuffer flag{0x00};
    FujiIECPacket status(DEV_FUJI, CMD::FUJI_STATUS);
    FujiIECPacket sos(DEV_FUJI, CMD::APETIME_GET_SOS);

    CHECK_FALSE(status.hasPayload());
    CHECK_FALSE(sos.hasPayload());
    sos.setPayload(flag);
    CHECK(sos.hasPayload());
    // Reading the params leaves the answer, and the params, alone
    CHECK(sos.param8(0) == 0x00);
    CHECK(sos.hasPayload());
}
