#pragma once

#include "xeas/event.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace xeas {

// Thrown on any feed-level failure: unopenable file, truncated stream,
// implausible frame length, malformed body (spec section 5). Feed errors
// happen before an Event exists, so the context is positional: byte offset of
// the frame start, 0-based message index, and the type byte ('\0' if not
// known). The decode_* functions have no stream position; they throw with
// offset and index 0, and replay_stream() re-throws enriched with the
// reader's position.
class FeedError : public std::runtime_error {
public:
    FeedError(std::string reason, std::uint64_t byte_offset,
              std::uint64_t message_index, char message_type = '\0')
        : std::runtime_error(reason + " (byte offset " + std::to_string(byte_offset) +
                             ", message index " + std::to_string(message_index) +
                             (message_type != '\0'
                                  ? std::string(", type '") + message_type + "'"
                                  : std::string()) +
                             ")"),
          reason_(std::move(reason)),
          byte_offset_(byte_offset),
          message_index_(message_index),
          message_type_(message_type) {}

    const std::string& reason() const noexcept { return reason_; }  // no position
    std::uint64_t byte_offset() const noexcept { return byte_offset_; }
    std::uint64_t message_index() const noexcept { return message_index_; }
    char message_type() const noexcept { return message_type_; }

private:
    std::string reason_;
    std::uint64_t byte_offset_;
    std::uint64_t message_index_;
    char message_type_;
};

// View into ItchReader's internal buffer; valid only until the next next().
struct FramedMessage {
    const std::byte* data;  // data[0] is the message type byte
    std::size_t size;       // body size in bytes (the u16 length prefix's value)
};

// Framing reader for the ITCH 5.0 historical dump ("BinaryFILE"): a flat
// sequence of [u16 big-endian length][body] frames (spec section 2). Plain
// buffered sequential reads; next() returns zero-copy views into the buffer;
// a frame straddling the buffer end is compacted to the front before
// refilling. buffer_size is clamped up so one maximum frame always fits.
class ItchReader {
public:
    // Throws FeedError if the file cannot be opened.
    explicit ItchReader(const std::filesystem::path& path,
                        std::size_t buffer_size = 1 << 20);
    // For tests: reads from a caller-owned stream (e.g. std::istringstream).
    explicit ItchReader(std::istream& in, std::size_t buffer_size = 1 << 20);

    // Next frame, or nullopt at clean EOF (end of data at a frame boundary,
    // or a 0x0000 sentinel length — terminal, bytes after it are ignored).
    // Throws FeedError on truncation or an implausible (> 512) length.
    std::optional<FramedMessage> next();

    std::uint64_t bytes_consumed() const noexcept { return bytes_consumed_; }
    std::uint64_t messages_framed() const noexcept { return messages_framed_; }

private:
    void refill();  // compact the unread tail to the front, then read more
    std::size_t available() const noexcept { return fill_ - pos_; }

    std::ifstream owned_file_;  // used by the path constructor only
    std::istream* in_;          // points at owned_file_ or the caller's stream
    std::vector<std::byte> buffer_;
    std::size_t pos_ = 0;       // next unread byte in buffer_
    std::size_t fill_ = 0;      // one past the last valid byte in buffer_
    bool eof_ = false;          // underlying stream is exhausted
    bool done_ = false;         // 0x0000 sentinel seen
    std::uint64_t bytes_consumed_ = 0;
    std::uint64_t messages_framed_ = 0;
};

// The seven message types that mutate a book (spec section 2 table).
constexpr bool is_book_message(char type) noexcept {
    return type == 'A' || type == 'F' || type == 'X' || type == 'D' ||
           type == 'E' || type == 'C' || type == 'U';
}

struct BookMessage {
    std::uint16_t stock_locate;
    Event event;
};

// Fused decoder+normalizer: one framed body (data[0] = type byte, size = the
// body length) to a canonical Event. A non-book type throws. Throws FeedError
// on wrong body length for the type or a side byte that is not 'B'/'S'. Does
// NOT validate semantics (unknown ids, over-execute, ...): form belongs here,
// meaning belongs to the book.
BookMessage decode_book_message(const std::byte* data, std::size_t size);

struct StockDirectoryEntry {
    std::uint16_t stock_locate;
    std::string symbol;  // trailing spaces trimmed
};

// Decodes an 'R' (Stock Directory) body; only locate and symbol are kept.
StockDirectoryEntry decode_stock_directory(const std::byte* data, std::size_t size);

}  // namespace xeas
