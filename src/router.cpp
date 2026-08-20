#include "xeas/router.h"

namespace xeas {

BookRouter::BookRouter() : books_(kLocateSlots), symbols_(kLocateSlots) {}

Book& BookRouter::book_for(std::uint16_t stock_locate) {
    auto& slot = books_[stock_locate];
    if (!slot) {
        slot = std::make_unique<Book>();
        ++book_count_;
    }
    return *slot;
}

const Book* BookRouter::find(std::uint16_t stock_locate) const {
    return books_[stock_locate].get();
}

void BookRouter::set_symbol(std::uint16_t locate, std::string symbol) {
    symbols_[locate] = std::move(symbol);
}

std::string BookRouter::symbol_or_locate(std::uint16_t locate) const {
    const std::string& symbol = symbols_[locate];
    return symbol.empty() ? "locate " + std::to_string(locate) : symbol;
}

void BookRouter::for_each_book(
    const std::function<void(std::uint16_t, const Book&)>& fn) const {
    for (std::size_t locate = 0; locate < books_.size(); ++locate) {
        if (books_[locate]) {
            fn(static_cast<std::uint16_t>(locate), *books_[locate]);
        }
    }
}

}  // namespace xeas
