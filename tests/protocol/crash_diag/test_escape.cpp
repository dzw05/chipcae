#include "support.hpp"

using namespace crashdiag_test;

int main() {
    return run([] {
        // Guard the independent oracle against malformed output that would otherwise
        // make the round-trip assertions accept a broken serializer.
        const std::string fixture =
            R"({"schema":"chipcae.diag.rank/0.1","rank":0,"category":"FAILURE_CATEGORY_INTERNAL","code":"","error_code":0,"module":"","window":18446744073709551615,"time":0.35,"message":"ok"})";
        check(Parser(fixture).parse().number<std::uint64_t>("window") ==
                  std::numeric_limits<std::uint64_t>::max(),
              "parser narrowed uint64");
        std::vector<std::string> invalid{fixture + "x"};
        auto replace = [&](std::string_view from, std::string_view to) {
            auto changed = fixture;
            changed.replace(changed.find(from), from.size(), to);
            invalid.push_back(changed);
        };
        replace(R"("message":"ok")", R"("message":"\q")");
        replace(R"("message":"ok")", std::string("\"message\":\"a\nb\""));
        replace(R"("message":"ok")", R"("message":"ok","message":"duplicate")");
        replace(R"("message":"ok")", R"("message":"ok","extra":1)");
        replace(R"(,"message":"ok")", "");
        replace("0.35", "NaN");
        replace("0.35", "Infinity");
        replace("0.35", "1e9999");
        replace("0.35", "01");
        replace(R"("message":"ok")", R"("message":"\uD800")");
        for (const auto& text : invalid)
            expect_throw<std::runtime_error>([&] { (void)Parser(text).parse(); },
                                             "parser accepted malformed diagnostic JSON");

        TempDir tmp;
        CrashDiag::install(tmp.path, 0);
        std::string controls;
        for (unsigned value = 0; value < 0x20; ++value)
            controls.push_back(static_cast<char>(value));
        const std::vector<std::string> messages{"quote=\" slash=\\ newline=\n tab=\t carriage=\r",
                                                std::string("nul\0tail", 8),
                                                controls,
                                                "UTF-8: 芯仿 é 😀",
                                                ""};
        for (const auto& message : messages) {
            CrashError error{"FAILURE_CATEGORY_IO", message, 14, message, message};
            CrashDiag::on_error(error);
            expect_json(parse(diagnostic(tmp.path, 0, ".json")), 0, error, 0, 0.0);
        }
        CrashDiag::finalize();
        empty_signal(tmp.path, 0);
    });
}
