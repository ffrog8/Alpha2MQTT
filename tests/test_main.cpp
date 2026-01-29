#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest/doctest.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct DurationEntry {
    std::string name;
    double seconds;
};

// SummaryReporter emits stable test totals and a top-10 duration list.
// Lifecycle: constructed per test run by doctest; owned by the framework.
// Thread-safety: not thread-safe; assumes single-threaded reporter callbacks.
// Invariants: durations are recorded for each completed test case before summary output.
class SummaryReporter final : public doctest::IReporter {
public:
    explicit SummaryReporter(const doctest::ContextOptions& options)
        : out(*options.cout) {}

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override { durations.clear(); }
    void test_run_end(const doctest::TestRunStats& stats) override {
        const unsigned total = stats.numTestCasesPassingFilters;
        const unsigned failed = stats.numTestCasesFailed;
        const unsigned passed = total - failed;
        const unsigned skipped = stats.numTestCases - stats.numTestCasesPassingFilters;

        out << "[alpha2mqtt] test cases: total=" << total
            << " passed=" << passed
            << " failed=" << failed
            << " skipped=" << skipped << "\n";
        out << "[alpha2mqtt] assertions: total=" << stats.numAsserts
            << " passed=" << (stats.numAsserts - stats.numAssertsFailed)
            << " failed=" << stats.numAssertsFailed << "\n";

        std::vector<DurationEntry> sorted = durations;
        std::sort(sorted.begin(), sorted.end(), [](const DurationEntry& left, const DurationEntry& right) {
            return left.seconds > right.seconds;
        });

        const size_t top_count = std::min<size_t>(10, sorted.size());
        out << "[alpha2mqtt] top " << top_count << " longest test cases (s):\n";
        for (size_t i = 0; i < top_count; ++i) {
            out << "[alpha2mqtt] " << (i + 1) << ") "
                << std::fixed << std::setprecision(6) << sorted[i].seconds
                << " s: " << sorted[i].name << "\n";
        }
        if (top_count == 0) {
            out << "[alpha2mqtt] no test case durations recorded\n";
        }
    }

    void test_case_start(const doctest::TestCaseData& in) override {
        current_test_case = in.m_name ? in.m_name : "";
    }
    void test_case_reenter(const doctest::TestCaseData& in) override {
        current_test_case = in.m_name ? in.m_name : "";
    }
    void test_case_end(const doctest::CurrentTestCaseStats& st) override {
        durations.push_back({current_test_case, st.seconds});
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}

private:
    std::ostream& out;
    std::string current_test_case;
    std::vector<DurationEntry> durations;
};
} // namespace

DOCTEST_REGISTER_REPORTER("summary", 0, SummaryReporter);

int main(int argc, char** argv) {
    doctest::Context context(argc, argv);
    context.setOption("duration", true);
    context.setOption("reporters", "summary");
    return context.run();
}
