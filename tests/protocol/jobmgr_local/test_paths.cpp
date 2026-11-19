#include "chipcae/jobmgr/local_paths.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <set>

using namespace jobmgr_test;
using namespace chipcae::jobmgr;

int main() {
    Suite suite;
    suite.test("UUID accepts hex, braces and case", [] {
        for (const auto id : {"01234567-89ab-cdef-0123-456789abcdef",
                              "{01234567-89ab-cdef-0123-456789abcdef}",
                              "01234567-89AB-CDEF-0123-456789ABCDEF",
                              "{01234567-89AB-CDEF-0123-456789ABCDEF}",
                              "00000000-0000-0000-0000-000000000001"})
            require(is_uuid(id), std::string("valid UUID rejected: ") + id);
    });
    suite.test("UUID rejects nil and malformed input", [] {
        for (const auto id : {"",
                              "00000000-0000-0000-0000-000000000000",
                              "{00000000-0000-0000-0000-000000000000}",
                              "0123456789abcdef0123456789abcdef",
                              "01234567-89ab-cdef-0123-456789abcde",
                              "01234567-89ab-cdef-0123-456789abcdeff",
                              "g1234567-89ab-cdef-0123-456789abcdef",
                              "{01234567-89ab-cdef-0123-456789abcdef",
                              "01234567-89ab-cdef-0123-456789abcdef}",
                              " 01234567-89ab-cdef-0123-456789abcdef"})
            require(!is_uuid(id), std::string("invalid UUID accepted: ") + id);
    });
    suite.test("1000 generated UUIDs are unique lowercase RFC v4", [] {
        std::set<std::string> ids;
        for (int i = 0; i < 1000; ++i) {
            const auto id = make_run_id();
            require(is_uuid(id) && id.size() == 36, "generated ID must be an unbraced UUID");
            require(id[14] == '4' &&
                        std::string_view("89ab").find(id[19]) != std::string_view::npos,
                    "generated UUID version/variant");
            require(id.find_first_not_of("0123456789abcdef-") == std::string::npos,
                    "generated UUID must be lowercase");
            require(ids.insert(id).second, "duplicate generated UUID");
        }
    });
    suite.test("snapshot references reject each unsafe form", [] {
        for (const auto ref : {"/abs", "a/../b", "a\\b", "c:x", "../a", "a/.."})
            throws_message([&] { check_snapshot_ref(ref); }, "Unsafe snapshot reference");
        check_snapshot_ref("a/b.json");
        check_snapshot_ref("a/..safe/b.json");
    });
    suite.test("SHA256 standard empty and abc vectors", [] {
        require(sha256_hex("") ==
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "SHA256 empty vector");
        require(sha256_hex("abc") ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA256 abc vector");
    });
    suite.test("atomic overwrite replaces complete bytes and cleans temporary files", [] {
        TempDirectory temp;
        const auto target = temp.path / "state.pb";
        write(target, "old bytes that must disappear");
        const std::string replacement = std::string("new\0binary", 10) + std::string(131072, 'x');
        write_file_atomic(target, replacement);
        require(read(target) == replacement, "atomic write did not replace complete file");
        require(std::distance(fs::directory_iterator(temp.path), fs::directory_iterator{}) == 1,
                "atomic write left temporary files");
    });
    return suite.result();
}
