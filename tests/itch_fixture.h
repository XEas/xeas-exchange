#pragma once

// Test-only ITCH 5.0 binary fixture builder. Written as an INDEPENDENT
// inverse of the decoder, straight from the ITCH 5.0 field tables — never
// call decoder helpers here, or round-trip tests lose their teeth.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

namespace xeas::test {

class ItchBuilder {
public:
    // 'A' Add Order, body 36.
    ItchBuilder& add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                           char side, std::uint32_t shares, const std::string& stock,
                           std::uint32_t price) {
        begin(36, 'A', locate, ts);
        u64(ref); ch(side); u32(shares); alpha(stock, 8); u32(price);
        return *this;
    }

    // 'F' Add Order with MPID attribution, body 40.
    ItchBuilder& add_order_mpid(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                char side, std::uint32_t shares, const std::string& stock,
                                std::uint32_t price, const std::string& mpid) {
        begin(40, 'F', locate, ts);
        u64(ref); ch(side); u32(shares); alpha(stock, 8); u32(price); alpha(mpid, 4);
        return *this;
    }

    // 'X' Order Cancel (partial), body 23.
    ItchBuilder& order_cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                              std::uint32_t canceled_shares) {
        begin(23, 'X', locate, ts);
        u64(ref); u32(canceled_shares);
        return *this;
    }

    // 'D' Order Delete, body 19.
    ItchBuilder& order_delete(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        begin(19, 'D', locate, ts);
        u64(ref);
        return *this;
    }

    // 'E' Order Executed, body 31.
    ItchBuilder& order_executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                std::uint32_t executed_shares, std::uint64_t match) {
        begin(31, 'E', locate, ts);
        u64(ref); u32(executed_shares); u64(match);
        return *this;
    }

    // 'C' Order Executed with Price, body 36.
    ItchBuilder& order_executed_price(std::uint16_t locate, std::uint64_t ts,
                                      std::uint64_t ref, std::uint32_t executed_shares,
                                      std::uint64_t match, char printable,
                                      std::uint32_t exec_price) {
        begin(36, 'C', locate, ts);
        u64(ref); u32(executed_shares); u64(match); ch(printable); u32(exec_price);
        return *this;
    }

    // 'U' Order Replace, body 35.
    ItchBuilder& order_replace(std::uint16_t locate, std::uint64_t ts,
                               std::uint64_t old_ref, std::uint64_t new_ref,
                               std::uint32_t shares, std::uint32_t price) {
        begin(35, 'U', locate, ts);
        u64(old_ref); u64(new_ref); u32(shares); u32(price);
        return *this;
    }

    // 'R' Stock Directory, body 39. Only locate and symbol matter downstream;
    // the remaining directory attributes get plausible constants.
    ItchBuilder& stock_directory(std::uint16_t locate, std::uint64_t ts,
                                 const std::string& symbol) {
        begin(39, 'R', locate, ts);
        alpha(symbol, 8);
        ch(' ');       // market category
        ch(' ');       // financial status indicator
        u32(100);      // round lot size
        ch('N');       // round lots only
        ch(' ');       // issue classification
        alpha("", 2);  // issue sub-type
        ch('P');       // authenticity
        ch(' ');       // short sale threshold indicator
        ch(' ');       // IPO flag
        ch(' ');       // LULD reference price tier
        ch('N');       // ETP flag
        u32(0);        // ETP leverage factor
        ch('N');       // inverse indicator
        return *this;
    }

    // Any other well-framed type: common header + the given tail bytes.
    ItchBuilder& other(char type, std::uint16_t locate, std::uint64_t ts,
                       const std::string& tail) {
        begin(static_cast<std::uint16_t>(11 + tail.size()), type, locate, ts);
        out_.append(tail);
        return *this;
    }

    // Malformed-input helpers.
    ItchBuilder& raw(const std::string& bytes) { out_.append(bytes); return *this; }
    ItchBuilder& sentinel() { ch('\0'); ch('\0'); return *this; }  // 0x0000 length
    ItchBuilder& truncate_last(std::size_t n) {
        out_.resize(out_.size() >= n ? out_.size() - n : 0);
        return *this;
    }

    const std::string& bytes() const { return out_; }
    std::istringstream stream() const {
        return std::istringstream(out_, std::ios::binary);
    }

private:
    void begin(std::uint16_t body_len, char type, std::uint16_t locate,
               std::uint64_t ts) {
        u16(body_len);  // frame length prefix
        ch(type);
        u16(locate);
        u16(0);         // tracking number (ignored by the decoder)
        u48(ts);
    }

    void ch(char c) { out_.push_back(c); }
    void u16(std::uint16_t v) { be(v, 2); }
    void u32(std::uint32_t v) { be(v, 4); }
    void u48(std::uint64_t v) { be(v, 6); }
    void u64(std::uint64_t v) { be(v, 8); }
    void be(std::uint64_t v, int bytes) {  // big-endian, most significant first
        for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
            out_.push_back(static_cast<char>((v >> shift) & 0xFF));
        }
    }
    void alpha(const std::string& s, std::size_t width) {  // left-just., space-padded
        out_.append(s, 0, std::min(s.size(), width));
        for (std::size_t i = s.size(); i < width; ++i) out_.push_back(' ');
    }

    std::string out_;
};

}  // namespace xeas::test
