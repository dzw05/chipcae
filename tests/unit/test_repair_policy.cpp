#include "chipcae/geometry/import_accept.hpp"
#include "chipcae/geometry/import_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifndef CHIPCAE_ACCEPT_JSONCONS
#error "geometry target must expose jsoncons availability"
#endif

using namespace chipcae::geometry;
static void check(bool ok, const char* text) {
    if (!ok) {
        std::cerr << text << '\n';
        std::exit(1);
    }
}

namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct AcceptFixture {
    std::filesystem::path dir;
    AcceptFixture() {
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i) {
            dir = std::filesystem::temp_directory_path() /
                  ("chipcae-accept-" + std::to_string(seed) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(dir))
                return;
        }
        throw std::runtime_error("cannot create isolated accept fixture");
    }
    ~AcceptFixture() {
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }
    std::filesystem::path write(const std::string& contents) const {
        const auto path = dir / "import.accept.json";
        std::ofstream out(path);
        out << contents;
        out.close();
        require(bool(out), "fixture write failed");
        return path;
    }
};
void accept_file_default_path() {
    AcceptFixture f;
    const auto path = f.write(R"({"accept":[]})");
    require(default_accept_file(f.dir / "board.step") == path, "default_path: sibling path");
    std::filesystem::remove(path);
    require(default_accept_file(f.dir / "board.step").empty(), "default_path: absent is empty");
}
#if CHIPCAE_ACCEPT_JSONCONS
void accept_file_roundtrip() {
    AcceptFixture f;
    const auto path = f.write(
        R"({"accept":[{"defect_id":"first","note":"one"},{"defect_id":"second","note":"two"},{"defect_id":"third","note":"three"}]})");
    std::string error;
    const auto list = read_accept_file(path, &error);
    require(list.has_value(), "roundtrip: valid file must load");
    require(list->source == path, "roundtrip: source path");
    require(list->entries.size() == 3, "roundtrip: three entries");
    const std::vector<std::string> ids{"first", "second", "third"};
    const std::vector<std::string> notes{"one", "two", "three"};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(list->entries[i].defect_id == ids[i], "roundtrip: ordered exact id");
        require(list->entries[i].note == notes[i], "roundtrip: ordered exact note");
    }
    require(accepted_ids(*list) == ids, "roundtrip: ordered extracted ids");
}
void accept_file_omitted_note() {
    AcceptFixture f;
    std::string error;
    const auto list = read_accept_file(
        f.write(
            R"({"accept":[{"defect_id":"first","note":"one"},{"defect_id":"second"},{"defect_id":"third","note":"three"}]})"),
        &error);
    require(list.has_value(), "omitted_note: valid file must load");
    require(list->entries.size() == 3, "omitted_note: three entries");
    const std::vector<std::string> ids{"first", "second", "third"};
    const std::vector<std::string> notes{"one", "", "three"};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        require(list->entries[i].defect_id == ids[i], "omitted_note: ordered exact id");
        require(list->entries[i].note == notes[i], "omitted_note: ordered exact note");
    }
    require(accepted_ids(*list) == ids, "omitted_note: ordered extracted ids");
}
void accept_file_covers_b() {
    AcceptFixture f;
    std::vector<Defect> defects;
    std::string entries;
    for (int i = 0; i < 10; ++i) {
        Defect d;
        d.tier = Tier::B;
        d.defect_id = "b-" + std::to_string(i);
        defects.push_back(d);
        if (i)
            entries += ',';
        entries += "{\"defect_id\":\"" + defect_id(d) + "\"}";
    }
    std::string error;
    auto list = read_accept_file(f.write("{\"accept\":[" + entries + "]}"), &error);
    require(list.has_value(), "covers_b: valid file must load");
    const auto ids = accepted_ids(*list);
    require(ids.size() == 10, "covers_b: ten ids");
    require(apply_repair_policy(defects, ids).result == ImportResult::Success,
            "covers_b: all ten accepted");
    entries.erase(entries.rfind(','));
    list = read_accept_file(f.write("{\"accept\":[" + entries + "]}"), &error);
    require(list.has_value(), "covers_b: nine-id file must load");
    const auto partial_ids = accepted_ids(*list);
    const auto result = apply_repair_policy(defects, partial_ids);
    require(result.result == ImportResult::ConfirmationRequired, "covers_b: uncovered B");
    const auto rejected = [](const Defect& d) { return !d.rejection_reason.empty(); };
    require(std::count_if(result.defects.begin(), result.defects.end(), rejected) == 1,
            "covers_b: exactly one rejected defect");
    const auto uncovered = std::find_if(result.defects.begin(), result.defects.end(), rejected);
    require(std::find(partial_ids.begin(), partial_ids.end(), defect_id(*uncovered)) ==
                partial_ids.end(),
            "covers_b: rejected canonical id is absent from accepted ids");
}
void accept_file_unknown_id() {
    AcceptFixture f;
    std::string error;
    std::vector<std::string> warnings;
    const auto list = read_accept_file(
        f.write(R"({"accept":[{"defect_id":"nonexistent-id"}]})"), &error, &warnings);
    require(list.has_value(), "unknown_id: file must load");
    require(warnings.empty(), "unknown_id: reader cannot classify unknown ids");
    const auto result = apply_repair_policy({}, accepted_ids(*list));
    require(result.result == ImportResult::Success, "unknown_id: success");
    require(result.warnings.size() == 1, "unknown_id: exactly one policy warning");
    require(result.warnings[0].find("nonexistent-id") != std::string::npos,
            "unknown_id: warning contains input id");
}
void malformed(const std::string& contents) {
    AcceptFixture f;
    std::string error;
    const auto list = read_accept_file(f.write(contents), &error);
    require(!list, "malformed: must reject");
    require(error.starts_with("IMPORT_REJECTED: "), "malformed: required error prefix");
    if (contents.empty())
        require(error == "IMPORT_REJECTED: accept file is empty",
                "empty file: explicit rejection required");
}
void accept_file_duplicate() {
    AcceptFixture f;
    std::string error;
    std::vector<std::string> warnings;
    const auto list = read_accept_file(
        f.write(
            R"({"accept":[{"defect_id":"duplicate-example-42","note":"first"},{"defect_id":"duplicate-example-42","note":"later"}]})"),
        &error,
        &warnings);
    require(list.has_value(), "duplicate: file must load");
    require(list->entries.size() == 1, "duplicate: retain one entry");
    require(list->entries[0].note == "first", "duplicate: retain first occurrence");
    require(warnings.size() == 1, "duplicate: exactly one warning");
    require(warnings[0].find("duplicate-example-42") != std::string::npos,
            "duplicate: warning contains input id");
}
void accept_file_distinct_notes() {
    AcceptFixture f;
    std::string error;
    std::vector<std::string> warnings;
    const auto list = read_accept_file(
        f.write(
            R"({"accept":[{"defect_id":"id","note":"first"},{"defect_id":"id","note":"second"},{"defect_id":"id","note":"First"},{"defect_id":"id","note":"first "}]})"),
        &error,
        &warnings);
    require(list.has_value(), "distinct_notes: file must load");
    require(list->entries.size() == 1, "distinct_notes: deduplicate by id only");
    require(list->entries[0].defect_id == "id", "distinct_notes: preserve id");
    require(list->entries[0].note == "first", "distinct_notes: retain first note");
    require(warnings.size() == 3, "distinct_notes: one warning per skipped duplicate");
    for (const auto& warning : warnings)
        require(warning.find("id") != std::string::npos,
                "distinct_notes: warning contains input id");
}
void accept_file_boundaries() {
    AcceptFixture f;
    std::string error;
    auto list = read_accept_file(f.write(R"({"accept":[]})"), &error);
    require(list.has_value() && list->entries.empty(), "boundaries: empty array valid");
    require(accepted_ids(*list).empty(), "boundaries: no extracted ids");
    std::vector<std::string> warnings;
    list = read_accept_file(
        f.write(
            R"({"accept":[{"defect_id":"ID"},{"defect_id":"id","note":""},{"defect_id":" id "},{"defect_id":"ID","note":"later"}]})"),
        &error,
        &warnings);
    require(list.has_value() && list->entries.size() == 3,
            "boundaries: case and whitespace distinct");
    require(accepted_ids(*list) == std::vector<std::string>{"ID", "id", " id "},
            "boundaries: exact bytes preserved");
    require(list->entries[0].note.empty(), "boundaries: retain first ID entry with omitted note");
    require(list->entries[1].note.empty(),
            "boundaries: preserve id entry with explicit empty note");
    require(list->entries[2].note.empty(), "boundaries: preserve spaced id with omitted note");
    require(warnings.size() == 1, "boundaries: exact id duplicate warns once");
    require(warnings[0].find("ID") != std::string::npos,
            "boundaries: duplicate warning contains exact id");
    warnings.clear();
    list = read_accept_file(
        f.write(R"({"extra":true,"accept":[{"defect_id":"id","extra":1}]})"), &error, &warnings);
    require(list.has_value(), "boundaries: unknown keys allowed");
    require(warnings.size() == 2, "boundaries: warning per unknown key occurrence");
}
bool run_accept_tests() {
    unsigned failures = 0;
    const auto run = [&](const std::string& name, const auto& test) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    run("accept_file_roundtrip", accept_file_roundtrip);
    run("accept_file_omitted_note", accept_file_omitted_note);
    run("accept_file_covers_b", accept_file_covers_b);
    run("accept_file_unknown_id", accept_file_unknown_id);
    run("accept_file_default_path", accept_file_default_path);
    run("accept_file_duplicate", accept_file_duplicate);
    run("accept_file_distinct_notes", accept_file_distinct_notes);
    run("accept_file_boundaries", accept_file_boundaries);
    run("accept_file_malformed_missing_accept", [] { malformed(R"({"foo":1})"); });
    run("accept_file_malformed_missing_defect_id", [] { malformed(R"({"accept":[{}]})"); });
    const std::vector<std::string> invalid{"not JSON",
                                           R"({"accept":{}})",
                                           R"({"accept":[{"defect_id":""}]})",
                                           R"({"accept":[{"defect_id":123}]})",
                                           "",
                                           "[]",
                                           "{}",
                                           R"({"accept":[1]})",
                                           R"({"accept":[{"defect_id":"id","note":1}]})"};
    for (std::size_t i = 0; i < invalid.size(); ++i)
        run("accept_file_malformed[" + std::to_string(i) + "]", [&] { malformed(invalid[i]); });
    run("accept_file_missing", [] {
        AcceptFixture f;
        std::string error = "previous error";
        require(!read_accept_file(f.dir / "missing.json", &error), "missing: absent result");
        require(error.empty(), "missing: no error");
    });
    return failures == 0;
}
#endif
} // namespace

int main() {
    std::vector<Defect> ds;
    for (int tier = 0; tier < 4; ++tier)
        for (int i = 0; i < 10; ++i) {
            Defect d;
            d.tier = static_cast<Tier>(tier);
            d.reason = DefectReason::TOLERANCE_RANGE_UNSUPPORTED;
            d.dimension = "L";
            d.size = i;
            d.defect_id = std::to_string(tier) + "-" + std::to_string(i);
            d.suggestion = "repair";
            ds.push_back(d);
        }
    auto r = apply_repair_policy(ds, {});
    check(r.result == ImportResult::Rejected && r.defects.size() == 40,
          "tier B confirmation and complete rows");
    for (const auto& d : r.defects)
        if (d.tier != Tier::Weld)
            check(!d.action.empty() || !d.rejection_reason.empty(), "no silent defects");
    std::vector<std::string> ids;
    for (int i = 0; i < 10; ++i)
        ids.push_back("2-" + std::to_string(i));
    std::vector<Defect> without_c(ds.begin(), ds.begin() + 30);
    r = apply_repair_policy(without_c, {});
    check(r.result == ImportResult::ConfirmationRequired && r.defects.size() == 30,
          "unconfirmed tier B requires confirmation and writes all rows");
    r = apply_repair_policy(without_c, ids);
    check(r.result == ImportResult::Success, "confirmed tier B accepted");
    const auto dir = std::filesystem::temp_directory_path() / "chipcae-import-report-test";
    ImportReportHeader h;
    h.L = 1;
    h.t_min = .1;
    check(write_import_report(dir, h, r.defects), "report writing");
    check(std::filesystem::file_size(dir / "import_report.json") > 0 &&
              std::filesystem::file_size(dir / "import_report.csv") > 0,
          "reports present");
    std::filesystem::remove_all(dir);
#if CHIPCAE_ACCEPT_JSONCONS
    return run_accept_tests() ? 0 : 1;
#else
    accept_file_default_path();
    AcceptFixture fixture;
    std::string error = "previous error";
    check(!read_accept_file(fixture.dir / "missing.json", &error), "missing file absent result");
    check(error.empty(), "missing file has no error");
    check(!read_accept_file(fixture.write(R"({"accept":[]})"), &error),
          "existing file requires jsoncons");
    check(error == "IMPORT_REJECTED: accept file requires a jsoncons build",
          "disabled jsoncons error");
    std::cout << "accept-file parser tests skipped because jsoncons is disabled; path and file "
                 "availability tested\n";
#endif
}
