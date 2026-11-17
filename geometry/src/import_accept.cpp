#include "chipcae/geometry/import_accept.hpp"

#include <fstream>
#include <unordered_set>

#if CHIPCAE_ACCEPT_JSONCONS
#include <jsoncons/json.hpp>
#endif

namespace chipcae::geometry {
std::filesystem::path default_accept_file(const std::filesystem::path& input) {
    const auto path = input.parent_path() / "import.accept.json";
    std::error_code error;
    return std::filesystem::exists(path, error) && !error ? path : std::filesystem::path{};
}

std::optional<AcceptList> read_accept_file(const std::filesystem::path& path,
                                           std::string* error,
                                           std::vector<std::string>* warnings) {
    if (error)
        error->clear();
    const auto reject = [&](const std::string& reason) -> std::optional<AcceptList> {
        if (error)
            *error = "IMPORT_REJECTED: " + reason;
        return std::nullopt;
    };
    std::error_code inspection_error;
    if (!std::filesystem::exists(path, inspection_error)) {
        if (inspection_error)
            return reject("cannot inspect accept file: " + inspection_error.message());
        // An absent file is normal; inspection failures above carry a fatal error.
        return std::nullopt;
    }
#if CHIPCAE_ACCEPT_JSONCONS
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return reject("cannot open accept file");
        if (input.peek() == std::ifstream::traits_type::eof()) {
            if (input.bad())
                return reject("cannot read accept file");
            return reject("accept file is empty");
        }
        const auto root = jsoncons::json::parse(input);
        if (input.bad())
            return reject("cannot read accept file");
        if (!root.is_object())
            return reject("accept file root must be an object");
        if (!root.contains("accept"))
            return reject("accept field is required");
        if (!root.at("accept").is_array())
            return reject("accept must be an array");
        const auto warn = [&](const std::string& message) {
            if (warnings)
                warnings->push_back(message);
        };
        for (const auto& member : root.object_range())
            if (member.key() != "accept")
                warn("unknown accept file key: " + std::string(member.key()));
        AcceptList list{path, {}};
        std::unordered_set<std::string> seen;
        for (const auto& entry : root.at("accept").array_range()) {
            if (!entry.is_object())
                return reject("accept entry must be an object");
            if (!entry.contains("defect_id") || !entry.at("defect_id").is_string())
                return reject("defect_id must be a nonempty string");
            auto id = entry.at("defect_id").as<std::string>();
            if (id.empty())
                return reject("defect_id must be a nonempty string");
            if (entry.contains("note") && !entry.at("note").is_string())
                return reject("note must be a string");
            for (const auto& member : entry.object_range())
                if (member.key() != "defect_id" && member.key() != "note")
                    warn("unknown accept entry key: " + std::string(member.key()));
            if (!seen.insert(id).second) {
                warn("duplicate defect_id: " + id);
                continue;
            }
            list.entries.push_back(
                {std::move(id), entry.contains("note") ? entry.at("note").as<std::string>() : ""});
        }
        return list;
    } catch (const std::exception& exception) {
        return reject("cannot read accept file: " + std::string(exception.what()));
    }
#else
    (void)warnings;
    return reject("accept file requires a jsoncons build");
#endif
}

std::vector<std::string> accepted_ids(const AcceptList& list) {
    std::vector<std::string> ids;
    ids.reserve(list.entries.size());
    for (const auto& entry : list.entries)
        ids.push_back(entry.defect_id);
    return ids;
}
} // namespace chipcae::geometry
