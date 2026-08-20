#include "xeas/itch.h"

#include "itch_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace xeas;
using xeas::test::ItchBuilder;

// ---------------------------------------------------------------------------
// Classify: is_book_message()
// ---------------------------------------------------------------------------

TEST(Classify, BookMessageTable) {
    for (char t : {'A', 'F', 'X', 'D', 'E', 'C', 'U'}) {
        EXPECT_TRUE(is_book_message(t)) << "type " << t;
    }
    for (char t : {'R', 'S', 'H', 'P', 'Q', 'I', 'B', 'a', 'u', '\0'}) {
        EXPECT_FALSE(is_book_message(t)) << "type " << t;
    }
}

// ---------------------------------------------------------------------------
// Errors: FeedError context
// ---------------------------------------------------------------------------

TEST(Errors, FeedErrorCarriesContext) {
    const FeedError e("truncated message body", 1234, 56, 'A');
    EXPECT_EQ(e.reason(), "truncated message body");
    EXPECT_EQ(e.byte_offset(), 1234u);
    EXPECT_EQ(e.message_index(), 56u);
    EXPECT_EQ(e.message_type(), 'A');
    const std::string what = e.what();
    EXPECT_NE(what.find("truncated message body"), std::string::npos);
    EXPECT_NE(what.find("1234"), std::string::npos);
    EXPECT_NE(what.find("'A'"), std::string::npos);
}

TEST(Errors, FeedErrorOmitsTypeWhenUnknown) {
    const FeedError e("truncated length prefix", 10, 2);
    EXPECT_EQ(e.message_type(), '\0');
    EXPECT_EQ(std::string(e.what()).find("type"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Fixture: the builder is the decoder's independently written inverse.
// Golden-bytes test: every byte of one 'A' frame checked against the ITCH
// 5.0 field table by hand.
// ---------------------------------------------------------------------------

TEST(Fixture, AddOrderGoldenBytes) {
    ItchBuilder b;
    b.add_order(0x0102, 0x030405060708ULL, 0x1122334455667788ULL, 'B', 200,
                "SPY", 1'234'500);
    const unsigned char expected[] = {
        0x00, 0x24,                                      // length 36
        0x41,                                            // 'A'
        0x01, 0x02,                                      // stock locate
        0x00, 0x00,                                      // tracking number
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08,              // timestamp (48-bit)
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,  // order reference
        0x42,                                            // side 'B'
        0x00, 0x00, 0x00, 0xC8,                          // shares 200
        0x53, 0x50, 0x59, 0x20, 0x20, 0x20, 0x20, 0x20,  // "SPY     "
        0x00, 0x12, 0xD6, 0x44,                          // price 1234500
    };
    ASSERT_EQ(b.bytes().size(), sizeof(expected));
    for (std::size_t i = 0; i < sizeof(expected); ++i) {
        EXPECT_EQ(static_cast<unsigned char>(b.bytes()[i]), expected[i])
            << "byte " << i;
    }
}

TEST(Fixture, FrameSizesMatchSpecTable) {
    // 2-byte prefix + fixed body length per type (spec section 2 table).
    EXPECT_EQ(ItchBuilder().add_order(1, 0, 1, 'B', 1, "X", 1).bytes().size(), 38u);
    EXPECT_EQ(ItchBuilder().add_order_mpid(1, 0, 1, 'B', 1, "X", 1, "MPID").bytes().size(), 42u);
    EXPECT_EQ(ItchBuilder().order_cancel(1, 0, 1, 1).bytes().size(), 25u);
    EXPECT_EQ(ItchBuilder().order_delete(1, 0, 1).bytes().size(), 21u);
    EXPECT_EQ(ItchBuilder().order_executed(1, 0, 1, 1, 1).bytes().size(), 33u);
    EXPECT_EQ(ItchBuilder().order_executed_price(1, 0, 1, 1, 1, 'Y', 1).bytes().size(), 38u);
    EXPECT_EQ(ItchBuilder().order_replace(1, 0, 1, 2, 1, 1).bytes().size(), 37u);
    EXPECT_EQ(ItchBuilder().stock_directory(1, 0, "AAPL").bytes().size(), 41u);
}

TEST(Fixture, RawSentinelAndTruncate) {
    ItchBuilder b;
    b.order_delete(1, 0, 42).sentinel().raw(std::string("\xAB\xCD", 2));
    EXPECT_EQ(b.bytes().size(), 21u + 2u + 2u);
    b.truncate_last(4);
    EXPECT_EQ(b.bytes().size(), 21u);
}

// ---------------------------------------------------------------------------
// Framing: ItchReader
// ---------------------------------------------------------------------------

TEST(Framing, EmptyStreamIsCleanEof) {
    ItchBuilder b;
    auto in = b.stream();
    ItchReader reader(in);
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.bytes_consumed(), 0u);
    EXPECT_EQ(reader.messages_framed(), 0u);
}

TEST(Framing, SingleFrameRoundTrip) {
    ItchBuilder b;
    b.add_order(1, 1000, 42, 'B', 100, "AAPL", 500'000);
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->size, 36u);
    EXPECT_EQ(std::to_integer<char>(frame->data[0]), 'A');
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.bytes_consumed(), 38u);
    EXPECT_EQ(reader.messages_framed(), 1u);
}

TEST(Framing, MultipleFramesInOrder) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    b.add_order(1, 200, 42, 'B', 100, "AAPL", 500'000);
    b.order_delete(1, 300, 42);
    auto in = b.stream();
    ItchReader reader(in);
    const char expected_types[] = {'R', 'A', 'D'};
    const std::size_t expected_sizes[] = {39, 36, 19};
    for (int i = 0; i < 3; ++i) {
        const auto frame = reader.next();
        ASSERT_TRUE(frame.has_value()) << "frame " << i;
        EXPECT_EQ(std::to_integer<char>(frame->data[0]), expected_types[i]);
        EXPECT_EQ(frame->size, expected_sizes[i]);
    }
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_EQ(reader.bytes_consumed(), 41u + 38u + 21u);
    EXPECT_EQ(reader.messages_framed(), 3u);
}

TEST(Framing, SentinelIsCleanEofAndTerminal) {
    ItchBuilder b;
    b.order_delete(1, 1000, 42).sentinel();
    b.add_order(1, 2000, 43, 'B', 100, "AAPL", 500'000);  // after sentinel: ignored
    auto in = b.stream();
    ItchReader reader(in);
    ASSERT_TRUE(reader.next().has_value());
    EXPECT_FALSE(reader.next().has_value());
    EXPECT_FALSE(reader.next().has_value());  // stays EOF
    EXPECT_EQ(reader.messages_framed(), 1u);
    EXPECT_EQ(reader.bytes_consumed(), 21u + 2u);  // frame + sentinel bytes
}

TEST(Framing, TinyBufferForcesStraddle) {
    ItchBuilder b;
    for (std::uint64_t i = 0; i < 40; ++i) {
        b.add_order(1, 1000 + i, 100 + i, 'B', 10, "AAPL", 500'000);
    }
    auto in = b.stream();
    // buffer_size is clamped up to one max frame (2 + 512 = 514 bytes);
    // 40 x 38-byte frames = 1520 bytes forces refills with mid-frame
    // compaction (38 does not divide 514).
    ItchReader reader(in, 1);
    std::uint64_t n = 0;
    while (const auto frame = reader.next()) {
        EXPECT_EQ(frame->size, 36u);
        EXPECT_EQ(std::to_integer<char>(frame->data[0]), 'A');
        ++n;
    }
    EXPECT_EQ(n, 40u);
    EXPECT_EQ(reader.messages_framed(), 40u);
    EXPECT_EQ(reader.bytes_consumed(), 40u * 38u);
}

TEST(Framing, TruncatedPrefixThrowsWithPosition) {
    ItchBuilder b;
    b.order_delete(1, 1000, 42).raw(std::string("\x00", 1));  // lone prefix byte
    auto in = b.stream();
    ItchReader reader(in);
    ASSERT_TRUE(reader.next().has_value());
    try {
        reader.next();
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.reason(), "truncated length prefix");
        EXPECT_EQ(e.byte_offset(), 21u);  // one delete frame consumed
        EXPECT_EQ(e.message_index(), 1u);
        EXPECT_EQ(e.message_type(), '\0');
    }
}

TEST(Framing, TruncatedBodyThrowsWithPosition) {
    ItchBuilder b;
    b.add_order(1, 1000, 42, 'B', 100, "AAPL", 500'000).truncate_last(5);
    auto in = b.stream();
    ItchReader reader(in);
    try {
        reader.next();
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.reason(), "truncated message body");
        EXPECT_EQ(e.byte_offset(), 0u);
        EXPECT_EQ(e.message_index(), 0u);
        EXPECT_EQ(e.message_type(), 'A');  // the type byte was readable
    }
}

TEST(Framing, ImplausibleLengthThrows) {
    ItchBuilder b;
    b.raw(std::string("\x02\x01", 2));  // length 513 > 512
    auto in = b.stream();
    ItchReader reader(in);
    try {
        reader.next();
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.reason(), "implausible frame length 513");
        EXPECT_EQ(e.byte_offset(), 0u);
        EXPECT_EQ(e.message_index(), 0u);
    }
}

TEST(Framing, FileConstructorReadsAndMissingFileThrows) {
    ItchBuilder b;
    b.order_delete(7, 1000, 42);
    const auto path = std::filesystem::temp_directory_path() / "xeas_itch_framing_test.bin";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(b.bytes().data(), static_cast<std::streamsize>(b.bytes().size()));
    }
    {
        ItchReader reader(path);
        const auto frame = reader.next();
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(std::to_integer<char>(frame->data[0]), 'D');
        EXPECT_FALSE(reader.next().has_value());
    }
    std::filesystem::remove(path);
    EXPECT_THROW(ItchReader(std::filesystem::path("/nonexistent/xeas.itch")), FeedError);
}

}  // namespace
