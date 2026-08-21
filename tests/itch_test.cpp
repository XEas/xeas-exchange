#include "xeas/itch.h"
#include "xeas/router.h"

#include "itch_fixture.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace xeas;
using xeas::test::ItchBuilder;

// Frames one builder message through the real reader and decodes it.
BookMessage decode_one(const ItchBuilder& b) {
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    EXPECT_TRUE(frame.has_value());
    return decode_book_message(frame->data, frame->size);
}

Event make_add_event(OrderId id, Side side, Price price, Qty qty) {
    Event e;
    e.type = EventType::Add;
    e.order_id = id;
    e.side = side;
    e.price = price;
    e.quantity = qty;
    return e;
}

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

// ---------------------------------------------------------------------------
// Decode: decode_book_message — one test per type, EVERY Event field asserted
// ---------------------------------------------------------------------------

TEST(Decode, AddOrderDecodesEveryField) {
    ItchBuilder b;
    b.add_order(0xBEEF, 0xFFEEDDCCBBAAULL, 0x0102030405060708ULL, 'B', 250,
                "AAPL", 1'234'500);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 0xBEEF);
    EXPECT_EQ(m.event.type, EventType::Add);
    EXPECT_EQ(m.event.timestamp, 0xFFEEDDCCBBAAULL);  // full 48-bit assembly
    EXPECT_EQ(m.event.order_id, 0x0102030405060708ULL);
    EXPECT_EQ(m.event.side, Side::Bid);
    EXPECT_EQ(m.event.price, 1'234'500);
    EXPECT_EQ(m.event.quantity, 250u);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, AddOrderSellSideMapsToAsk) {
    ItchBuilder b;
    b.add_order(1, 100, 42, 'S', 10, "AAPL", 500'000);
    EXPECT_EQ(decode_one(b).event.side, Side::Ask);
}

TEST(Decode, AddOrderMpidDropsAttribution) {
    ItchBuilder b;
    b.add_order_mpid(7, 200, 43, 'B', 20, "MSFT", 3'000'000, "NSDQ");
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 7);
    EXPECT_EQ(m.event.type, EventType::Add);  // same Event as 'A'
    EXPECT_EQ(m.event.timestamp, 200u);
    EXPECT_EQ(m.event.order_id, 43u);
    EXPECT_EQ(m.event.side, Side::Bid);
    EXPECT_EQ(m.event.price, 3'000'000);
    EXPECT_EQ(m.event.quantity, 20u);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, CancelDecodes) {
    ItchBuilder b;
    b.order_cancel(3, 300, 44, 15);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 3);
    EXPECT_EQ(m.event.type, EventType::Cancel);
    EXPECT_EQ(m.event.timestamp, 300u);
    EXPECT_EQ(m.event.order_id, 44u);
    EXPECT_EQ(m.event.quantity, 15u);
    EXPECT_EQ(m.event.price, 0);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, DeleteDecodes) {
    ItchBuilder b;
    b.order_delete(4, 400, 45);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 4);
    EXPECT_EQ(m.event.type, EventType::Delete);
    EXPECT_EQ(m.event.timestamp, 400u);
    EXPECT_EQ(m.event.order_id, 45u);
    EXPECT_EQ(m.event.quantity, 0u);
    EXPECT_EQ(m.event.price, 0);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, ExecutedDecodes) {
    ItchBuilder b;
    b.order_executed(5, 500, 46, 30, 0xDEADBEEFULL);  // match number ignored
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 5);
    EXPECT_EQ(m.event.type, EventType::Execute);
    EXPECT_EQ(m.event.timestamp, 500u);
    EXPECT_EQ(m.event.order_id, 46u);
    EXPECT_EQ(m.event.quantity, 30u);
    EXPECT_EQ(m.event.price, 0);
    EXPECT_EQ(m.event.new_order_id, 0u);
}

TEST(Decode, ExecutedWithPriceIgnoresPrintableAndPrice) {
    ItchBuilder b;
    b.order_executed_price(6, 600, 47, 40, 99, 'N', 9'999'999);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.event.type, EventType::Execute);
    EXPECT_EQ(m.event.order_id, 47u);
    EXPECT_EQ(m.event.quantity, 40u);
    EXPECT_EQ(m.event.price, 0);  // execution price never affects the book
}

TEST(Decode, ReplaceDecodes) {
    ItchBuilder b;
    b.order_replace(8, 800, 48, 49, 60, 2'000'000);
    const BookMessage m = decode_one(b);
    EXPECT_EQ(m.stock_locate, 8);
    EXPECT_EQ(m.event.type, EventType::Replace);
    EXPECT_EQ(m.event.timestamp, 800u);
    EXPECT_EQ(m.event.order_id, 48u);
    EXPECT_EQ(m.event.new_order_id, 49u);
    EXPECT_EQ(m.event.quantity, 60u);
    EXPECT_EQ(m.event.price, 2'000'000);
    // 'U' carries no side byte; Event.side stays at its default and the book
    // resolves side from the old order. Do not assert side here.
}

TEST(Decode, TimestampWidensFull48Bits) {
    ItchBuilder b;
    b.order_delete(1, 0xFFFFFFFFFFFFULL, 42);  // max 6-byte value
    EXPECT_EQ(decode_one(b).event.timestamp, 0xFFFFFFFFFFFFULL);
}

TEST(Decode, WrongBodyLengthThrowsForEveryType) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'B', 10, "AAPL", 1'000'000);
    b.add_order_mpid(1, 100, 2, 'B', 10, "AAPL", 1'000'000, "NSDQ");
    b.order_cancel(1, 100, 1, 5);
    b.order_delete(1, 100, 1);
    b.order_executed(1, 100, 1, 5, 900);
    b.order_executed_price(1, 100, 1, 5, 900, 'Y', 1'000'000);
    b.order_replace(1, 100, 1, 2, 10, 1'000'000);
    auto in = b.stream();
    ItchReader reader(in);
    int checked = 0;
    while (const auto frame = reader.next()) {
        const char type = std::to_integer<char>(frame->data[0]);
        try {
            decode_book_message(frame->data, frame->size - 1);
            FAIL() << "expected FeedError for type " << type;
        } catch (const FeedError& e) {
            EXPECT_EQ(e.message_type(), type);
            EXPECT_NE(e.reason().find("body length"), std::string::npos);
        }
        ++checked;
    }
    EXPECT_EQ(checked, 7);
}

TEST(Decode, BadSideByteThrows) {
    ItchBuilder b;
    b.add_order(1, 100, 1, 'Q', 10, "AAPL", 1'000'000);  // side neither B nor S
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    try {
        decode_book_message(frame->data, frame->size);
        FAIL() << "expected FeedError";
    } catch (const FeedError& e) {
        EXPECT_EQ(e.message_type(), 'A');
        EXPECT_NE(e.reason().find("side"), std::string::npos);
    }
}

TEST(Decode, NonBookTypeThrows) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_THROW(decode_book_message(frame->data, frame->size), FeedError);
}

// ---------------------------------------------------------------------------
// Decode: decode_stock_directory ('R')
// ---------------------------------------------------------------------------

TEST(Decode, StockDirectoryTrimsPadding) {
    ItchBuilder b;
    b.stock_directory(42, 100, "ZVZZT");  // padded to "ZVZZT   " on the wire
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    const StockDirectoryEntry entry =
        decode_stock_directory(frame->data, frame->size);
    EXPECT_EQ(entry.stock_locate, 42);
    EXPECT_EQ(entry.symbol, "ZVZZT");
}

TEST(Decode, StockDirectoryFullWidthSymbolKept) {
    ItchBuilder b;
    b.stock_directory(1, 100, "ABCDEFGH");
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(decode_stock_directory(frame->data, frame->size).symbol, "ABCDEFGH");
}

TEST(Decode, StockDirectoryWrongLengthOrTypeThrows) {
    ItchBuilder b;
    b.stock_directory(1, 100, "AAPL");
    auto in = b.stream();
    ItchReader reader(in);
    const auto frame = reader.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_THROW(decode_stock_directory(frame->data, frame->size - 1), FeedError);

    ItchBuilder b2;
    b2.order_delete(1, 100, 42);  // wrong type byte
    auto in2 = b2.stream();
    ItchReader reader2(in2);
    const auto frame2 = reader2.next();
    ASSERT_TRUE(frame2.has_value());
    EXPECT_THROW(decode_stock_directory(frame2->data, frame2->size), FeedError);
}

// ---------------------------------------------------------------------------
// Router: BookRouter
// ---------------------------------------------------------------------------

TEST(Router, LazyCreation) {
    BookRouter router;
    EXPECT_EQ(router.find(5), nullptr);
    EXPECT_EQ(router.book_count(), 0u);
    Book& book = router.book_for(5);
    EXPECT_EQ(router.find(5), &book);
    EXPECT_EQ(router.book_count(), 1u);
}

TEST(Router, SameLocateSameBook) {
    BookRouter router;
    Book& a = router.book_for(7);
    Book& b = router.book_for(7);
    EXPECT_EQ(&a, &b);
    EXPECT_EQ(router.book_count(), 1u);
}

TEST(Router, DistinctLocatesAreIsolated) {
    BookRouter router;
    apply(router.book_for(1), make_add_event(10, Side::Bid, 1'000'000, 100));
    EXPECT_EQ(router.book_for(1).order_count(), 1u);
    EXPECT_EQ(router.book_for(2).order_count(), 0u);
    EXPECT_EQ(router.book_count(), 2u);
}

TEST(Router, SymbolOrLocateFallsBackThenNames) {
    BookRouter router;
    EXPECT_EQ(router.symbol_or_locate(42), "locate 42");
    router.set_symbol(42, "AAPL");
    EXPECT_EQ(router.symbol_or_locate(42), "AAPL");
    // Symbols are independent of book creation ('R' needs no ordering).
    EXPECT_EQ(router.find(42), nullptr);
}

TEST(Router, ExtremeLocatesWork) {
    BookRouter router;
    router.book_for(0);
    router.book_for(65'535);
    EXPECT_NE(router.find(0), nullptr);
    EXPECT_NE(router.find(65'535), nullptr);
    EXPECT_EQ(router.book_count(), 2u);
}

TEST(Router, ForEachBookVisitsAllInLocateOrder) {
    BookRouter router;
    router.book_for(30);
    router.book_for(10);
    router.book_for(20);
    std::vector<std::uint16_t> visited;
    router.for_each_book(
        [&](std::uint16_t locate, const Book&) { visited.push_back(locate); });
    EXPECT_EQ(visited, (std::vector<std::uint16_t>{10, 20, 30}));
}

}  // namespace
