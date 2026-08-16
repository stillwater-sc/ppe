// test_json -- assertions about the minimal JSON reader.
//
// It exists to read kpu-sim system configurations, which are machine-generated
// and well-formed -- so the risk is not exotic input, it is a quiet misparse
// that turns a 1024 MB memory bank into a 0 and reports a KPU with no memory.
// These assert the shapes that file actually contains, plus the failure modes.

#include <ppe/cli.hpp>
#include <ppe/json.hpp>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(const char* what, double got, double want) {
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  %-46s got %10.3f  want %10.3f  %s\n", what, got, want,
                ok ? "ok" : "FAIL");
}

void expect_str(const char* what, const std::string& got, const std::string& want) {
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  %-46s got %-12s want %-12s %s\n", what, got.c_str(), want.c_str(),
                ok ? "ok" : "FAIL");
}

void expect_true(const char* what, bool ok) {
    if (!ok) ++failures;
    std::printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    if (ppe::wants_help(argc, argv)) {
        std::printf("test_json -- verify the minimal JSON reader\n");
        return 0;
    }

    std::printf("Scalars and nesting:\n");
    {
        const auto r = ppe::json::parse(
            R"({"a":1,"b":[1,2,3],"c":{"d":"x"},"e":true,"f":null,"g":-2.5e2})");
        expect_true("parses", r.ok);
        expect("a", r.root["a"].number(), 1.0);
        expect("b[1]", r.root["b"].items()[1].number(), 2.0);
        expect_str("c.d", r.root["c"]["d"].str(), "x");
        expect_true("e is true", r.root["e"].boolean());
        expect_true("f is null", r.root["f"].is_null());
        expect("negative exponent", r.root["g"].number(), -250.0);
    }

    std::printf("\nMissing keys yield the fallback, never a throw:\n");
    {
        const auto r = ppe::json::parse(R"({"present":1})");
        expect("absent number", r.root["absent"].number(42.0), 42.0);
        expect_str("absent string", r.root["a"]["b"]["c"].str("none"), "none");
        expect_true("absent array is empty", r.root["nope"].items().empty());
    }

    std::printf("\nUnit conversion (the field that matters most):\n");
    {
        const auto r = ppe::json::parse(R"({"capacity_kb":128,"capacity_mb":1024})");
        expect("128 kb -> bytes", static_cast<double>(r.root["capacity_kb"].size_bytes_from_kb()),
               131072.0);
        expect("1024 mb -> bytes", static_cast<double>(r.root["capacity_mb"].size_bytes_from_mb()),
               1073741824.0);
        // A missing size must be 0, not a plausible default: a KPU reported
        // with a made-up bank size is worse than one reported with none.
        expect("absent size is 0", static_cast<double>(r.root["nope"].size_bytes_from_kb()), 0.0);
    }

    std::printf("\nString escapes:\n");
    {
        const auto r = ppe::json::parse(R"({"s":"a\"b\\c\nd\u0041"})");
        expect_true("parses escapes", r.ok);
        expect_str("decoded", r.root["s"].str(), std::string("a\"b\\c\nd") + "A");
    }

    std::printf("\nMalformed input fails with a reason, not a crash:\n");
    {
        const char* bad[] = {"{", "{\"a\":}", "[1,2", "{\"a\" 1}", "", "{} trailing"};
        for (const char* b : bad) {
            const auto r = ppe::json::parse(b);
            if (r.ok) ++failures;
            std::printf("  %-46s %s\n", (std::string("rejects: ") + b).c_str(),
                        r.ok ? "FAIL (accepted)" : "ok");
        }
    }

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAILED");
    return failures == 0 ? 0 : 1;
}
