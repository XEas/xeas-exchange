// book_bench [--workload steady|insert|deep|all] [--events N] [--warmup N]
//            [--seed S] [--repeat R] [--impl new|baseline|both]
//
// Book-only microbenchmark (spec §2): deterministic TapeGen workloads applied
// in a timed steady_clock batch loop; reports the median and min of Mmsg/s
// over R repeats, ns/event, and peak RSS. Links xeas_core and xeas_baseline
// only — no feed, no I/O. Numbers of record come from a Release build; see
// BENCH.md for methodology (RSS here includes the pre-generated tape, which is
// identical across impls — compare impls with separate --impl runs).

#include "baseline_book.h"
#include "tape_gen.h"
#include "xeas/book.h"

#include <sys/resource.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace xeas;

int usage() {
    std::cerr
        << "usage: book_bench [--workload steady|insert|deep|all] [--events N]\n"
           "                  [--warmup N] [--seed S] [--repeat R] [--impl new|baseline|both]\n"
           "  --workload  synthetic workload(s) to run (default all)\n"
           "  --events    timed events per run (default 10000000)\n"
           "  --warmup    untimed mix events before timing, steady/deep only (default 1000000)\n"
           "  --seed      TapeGen seed, printed with every line (default 1)\n"
           "  --repeat    repeats per (workload, impl); median and min reported (default 5)\n"
           "  --impl      which book implementation(s) to run (default both)\n";
    return 1;
}

bool parse_count(const char* s, std::uint64_t& out) {
    if (s == nullptr || s[0] == '\0') return false;
    if (s[0] == '-' || s[0] == '+') return false;
    char* end;
    errno = 0;
    std::uint64_t val = std::strtoull(s, &end, 10);
    if (errno == ERANGE) return false;
    if (*end != '\0') return false;
    out = val;
    return true;
}

// ru_maxrss is bytes on macOS and KiB on Linux — normalized to bytes here.
std::uint64_t peak_rss_bytes() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}

template <class B>
double run_once_seconds(const WorkloadTape& tape) {
    B book;
    for (const Event& e : tape.warm) apply(book, e);
    const auto t0 = std::chrono::steady_clock::now();
    for (const Event& e : tape.timed) apply(book, e);
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

struct ImplResult {
    double median_mmsgs = 0.0;
    double min_mmsgs = 0.0;
};

template <class B>
ImplResult run_impl(const WorkloadTape& tape, std::uint64_t repeats) {
    std::vector<double> mmsgs;
    for (std::uint64_t r = 0; r < repeats; ++r) {
        const double seconds = run_once_seconds<B>(tape);
        mmsgs.push_back(static_cast<double>(tape.timed.size()) / seconds / 1e6);
    }
    std::sort(mmsgs.begin(), mmsgs.end());
    return ImplResult{mmsgs[mmsgs.size() / 2], mmsgs.front()};
}

void report(std::string_view workload, std::string_view impl, const WorkloadTape& tape,
            std::uint64_t seed, std::uint64_t repeats, const ImplResult& r) {
    std::cout << "workload=" << workload << " impl=" << impl
              << " events=" << tape.timed.size() << " warm=" << tape.warm.size()
              << " seed=" << seed << " repeats=" << repeats << std::fixed
              << std::setprecision(2) << " median=" << r.median_mmsgs
              << " Mmsg/s min=" << r.min_mmsgs << " Mmsg/s ns_per_event="
              << std::setprecision(1) << 1000.0 / r.median_mmsgs
              << " peak_rss_mib=" << peak_rss_bytes() / (1024 * 1024) << "\n"
              << std::defaultfloat;
}

}  // namespace

int main(int argc, char** argv) {
    const std::pair<std::string_view, Workload> all[] = {{"steady", Workload::Steady},
                                                         {"insert", Workload::Insert},
                                                         {"deep", Workload::Deep}};
    std::string_view workload_arg = "all";
    std::string_view impl = "both";
    std::uint64_t events = 10'000'000;
    std::uint64_t warmup = 1'000'000;
    std::uint64_t seed = 1;
    std::uint64_t repeats = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--workload" && i + 1 < argc) {
            workload_arg = argv[++i];
        } else if (arg == "--impl" && i + 1 < argc) {
            impl = argv[++i];
        } else if (arg == "--events" && i + 1 < argc) {
            if (!parse_count(argv[++i], events)) return usage();
        } else if (arg == "--warmup" && i + 1 < argc) {
            if (!parse_count(argv[++i], warmup)) return usage();
        } else if (arg == "--seed" && i + 1 < argc) {
            if (!parse_count(argv[++i], seed)) return usage();
        } else if (arg == "--repeat" && i + 1 < argc) {
            if (!parse_count(argv[++i], repeats)) return usage();
        } else {
            return usage();
        }
    }
    if (impl != "new" && impl != "baseline" && impl != "both") return usage();
    if (events == 0 || repeats == 0) return usage();
    bool matched = false;
    for (const auto& [name, workload] : all) {
        if (workload_arg != "all" && workload_arg != name) continue;
        matched = true;
        const WorkloadTape tape = make_workload_tape(workload, seed, warmup, events);
        ImplResult baseline_result;
        if (impl != "new") {
            baseline_result = run_impl<BaselineBook>(tape, repeats);
            report(name, "baseline", tape, seed, repeats, baseline_result);
        }
        if (impl != "baseline") {
            const ImplResult new_result = run_impl<Book>(tape, repeats);
            report(name, "new", tape, seed, repeats, new_result);
            if (impl == "both") {
                std::cout << "workload=" << name << " ratio new/baseline=" << std::fixed
                          << std::setprecision(2)
                          << new_result.median_mmsgs / baseline_result.median_mmsgs << "x\n"
                          << std::defaultfloat;
            }
        }
    }
    if (!matched) return usage();
    return 0;
}
