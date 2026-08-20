#include "xeas/itch.h"

#include "itch_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
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

}  // namespace
