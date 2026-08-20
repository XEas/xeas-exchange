#include "xeas/itch.h"

#include <algorithm>
#include <cstring>

namespace xeas {

namespace {

// Largest real ITCH 5.0 body is 50 bytes; anything past 512 means desynced,
// MoldUDP, or garbage input — fail fast instead of silently skipping
// gigabytes (spec section 5).
constexpr std::size_t kMaxBody = 512;

std::uint16_t read_u16(const std::byte* p) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(p[0]) << 8) |
                                      std::to_integer<std::uint16_t>(p[1]));
}

std::uint32_t read_u32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | std::to_integer<std::uint32_t>(p[i]);
    return v;
}

std::uint64_t read_u48(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

std::uint64_t read_u64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(p[i]);
    return v;
}

// Fixed body lengths per the ITCH 5.0 spec; 0 = not a book message.
std::size_t expected_body_length(char type) {
    switch (type) {
    case 'A': return 36;
    case 'F': return 40;
    case 'X': return 23;
    case 'D': return 19;
    case 'E': return 31;
    case 'C': return 36;
    case 'U': return 35;
    default: return 0;
    }
}

Side decode_side(std::byte b, char type) {
    const char c = std::to_integer<char>(b);
    if (c == 'B') return Side::Bid;
    if (c == 'S') return Side::Ask;
    throw FeedError(std::string("bad side byte '") + c + "'", 0, 0, type);
}

}  // namespace

ItchReader::ItchReader(const std::filesystem::path& path, std::size_t buffer_size)
    : owned_file_(path, std::ios::binary),
      in_(&owned_file_),
      // The buffer must hold one whole frame: next() returns views into it.
      buffer_(std::max(buffer_size, 2 + kMaxBody)) {
    if (!owned_file_.is_open()) {
        throw FeedError("cannot open file: " + path.string(), 0, 0);
    }
}

ItchReader::ItchReader(std::istream& in, std::size_t buffer_size)
    : in_(&in), buffer_(std::max(buffer_size, 2 + kMaxBody)) {}

void ItchReader::refill() {
    if (eof_) return;
    // Compact: move the unread tail to the front to make room. Bodies are
    // <= 512 bytes, so this is trivial and rare.
    if (pos_ > 0) {
        const std::size_t tail = fill_ - pos_;
        std::memmove(buffer_.data(), buffer_.data() + pos_, tail);
        pos_ = 0;
        fill_ = tail;
    }
    while (fill_ < buffer_.size() && !eof_) {
        in_->read(reinterpret_cast<char*>(buffer_.data() + fill_),
                  static_cast<std::streamsize>(buffer_.size() - fill_));
        const std::size_t got = static_cast<std::size_t>(in_->gcount());
        fill_ += got;
        if (in_->eof() || got == 0) eof_ = true;
    }
    // eofbit/failbit at end-of-data is expected; badbit is a hard I/O error.
    if (in_->bad()) {
        throw FeedError("stream read error", bytes_consumed_, messages_framed_);
    }
}

std::optional<FramedMessage> ItchReader::next() {
    if (done_) return std::nullopt;
    if (available() < 2) refill();
    if (available() == 0) return std::nullopt;  // clean EOF at a frame boundary
    if (available() < 2) {
        throw FeedError("truncated length prefix", bytes_consumed_, messages_framed_);
    }
    const std::uint16_t length = read_u16(buffer_.data() + pos_);
    if (length == 0) {
        // 0x0000 sentinel: clean end-of-stream marker at the tail of some
        // NASDAQ dumps. Consume it and stop; anything after it is ignored.
        pos_ += 2;
        bytes_consumed_ += 2;
        done_ = true;
        return std::nullopt;
    }
    if (length > kMaxBody) {
        throw FeedError("implausible frame length " + std::to_string(length),
                        bytes_consumed_, messages_framed_);
    }
    if (available() < 2u + length) refill();
    if (available() < 2u + length) {
        const char type =
            available() >= 3 ? std::to_integer<char>(buffer_[pos_ + 2]) : '\0';
        throw FeedError("truncated message body", bytes_consumed_,
                        messages_framed_, type);
    }
    const FramedMessage frame{buffer_.data() + pos_ + 2, length};
    pos_ += 2u + length;
    bytes_consumed_ += 2u + length;
    ++messages_framed_;
    return frame;
}

BookMessage decode_book_message(const std::byte* data, std::size_t size) {
    const char type = std::to_integer<char>(data[0]);
    const std::size_t expected = expected_body_length(type);
    if (expected == 0) {
        throw FeedError("not a book message", 0, 0, type);
    }
    if (size != expected) {
        throw FeedError("wrong body length " + std::to_string(size) +
                            " (expected " + std::to_string(expected) + ")",
                        0, 0, type);
    }
    BookMessage out;
    out.stock_locate = read_u16(data + 1);
    // data + 3: tracking number — MoldUDP diagnostics, ignored.
    Event& e = out.event;
    e.timestamp = read_u48(data + 5);
    switch (type) {
    case 'A':
    case 'F':  // MPID attribution at data + 36 is dropped
        e.type = EventType::Add;
        e.order_id = read_u64(data + 11);
        e.side = decode_side(data[19], type);
        e.quantity = read_u32(data + 20);
        e.price = static_cast<Price>(read_u32(data + 32));
        break;
    case 'X':
        e.type = EventType::Cancel;
        e.order_id = read_u64(data + 11);
        e.quantity = read_u32(data + 19);
        break;
    case 'D':
        e.type = EventType::Delete;
        e.order_id = read_u64(data + 11);
        break;
    case 'E':
    case 'C':  // 'C' printable flag and execution price never affect the book
        e.type = EventType::Execute;
        e.order_id = read_u64(data + 11);
        e.quantity = read_u32(data + 19);
        break;
    case 'U':
        // No side byte: the book resolves side from the old order (spec §2).
        e.type = EventType::Replace;
        e.order_id = read_u64(data + 11);
        e.new_order_id = read_u64(data + 19);
        e.quantity = read_u32(data + 27);
        e.price = static_cast<Price>(read_u32(data + 31));
        break;
    }
    return out;
}

}  // namespace xeas
