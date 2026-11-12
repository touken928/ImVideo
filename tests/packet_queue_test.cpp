#include "packet_queue.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

imvideo::AvPacketPtr packet_with_value(int value) {
    auto packet = imvideo::make_av_packet();
    REQUIRE(packet);
    REQUIRE(av_new_packet(packet.get(), 1) == 0);
    packet->pts = value;
    packet->data[0] = static_cast<unsigned char>(value);
    return packet;
}

} // namespace

TEST_CASE("Packet read-ahead preserves FIFO order across wraparound and rejects overflow", "[packet][queue]") {
    imvideo::PacketQueue queue;
    auto output = imvideo::make_av_packet();
    REQUIRE(output);
    REQUIRE_FALSE(queue.pop(output.get()));
    for (int value = 0; value < static_cast<int>(queue.capacity); ++value) {
        auto packet = packet_with_value(value);
        REQUIRE(queue.push(packet.get()));
        REQUIRE(packet->data == nullptr);
    }
    REQUIRE(queue.full());
    auto overflow = packet_with_value(123);
    REQUIRE_FALSE(queue.push(overflow.get()));
    REQUIRE(overflow->data[0] == 123);

    for (int value = 0; value < 32; ++value) {
        REQUIRE(queue.pop(output.get()));
        REQUIRE(output->pts == value);
        REQUIRE(output->data[0] == value);
    }
    for (int value = 64; value < 96; ++value) {
        auto packet = packet_with_value(value);
        REQUIRE(queue.push(packet.get()));
    }
    for (int value = 32; value < 96; ++value) {
        REQUIRE(queue.pop(output.get()));
        REQUIRE(output->pts == value);
        REQUIRE(output->data[0] == value);
    }
    REQUIRE(queue.empty());
}

TEST_CASE("Clearing read-ahead releases payloads and permits a new seek generation", "[packet][queue]") {
    imvideo::PacketQueue queue;
    auto packet = packet_with_value(7);
    AVBufferRef* observer = av_buffer_ref(packet->buf);
    REQUIRE(observer);
    REQUIRE(queue.push(packet.get()));
    const int retained = av_buffer_get_ref_count(observer);
    queue.clear();
    const int cleared = av_buffer_get_ref_count(observer);
    av_buffer_unref(&observer);
    REQUIRE(retained == 2);
    REQUIRE(cleared == 1);
    REQUIRE(queue.empty());

    auto next = packet_with_value(8);
    REQUIRE(queue.push(next.get()));
    queue.release();
    REQUIRE(queue.empty());
    auto reopened = packet_with_value(9);
    REQUIRE(queue.push(reopened.get()));
    REQUIRE(queue.pop(packet.get()));
    REQUIRE(packet->pts == 9);
}
