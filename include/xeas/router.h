#pragma once

#include "xeas/book.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xeas {

// One Book per stock locate. Locates are dense, day-scoped u16s, so a flat
// 65,536-slot vector gives O(1) routing with no hashing; heap-allocated books
// give pointer stability for free. Books are created lazily on first use.
// Symbol names (from 'R' messages) are cosmetic, may arrive in any order or
// never; reports fall back to "locate N".
class BookRouter {
public:
    BookRouter();

    Book& book_for(std::uint16_t stock_locate);          // lazily creates
    const Book* find(std::uint16_t stock_locate) const;  // nullptr if never seen
    void set_symbol(std::uint16_t locate, std::string symbol);
    std::string symbol_or_locate(std::uint16_t locate) const;  // "AAPL" or "locate 42"
    std::size_t book_count() const noexcept { return book_count_; }
    // Visits every created book in ascending locate order.
    void for_each_book(const std::function<void(std::uint16_t, const Book&)>&) const;

private:
    static constexpr std::size_t kLocateSlots = 65'536;  // full u16 range

    std::vector<std::unique_ptr<Book>> books_;
    std::vector<std::string> symbols_;
    std::size_t book_count_ = 0;
};

}  // namespace xeas
