#pragma once
#include "archive.hpp"
#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <istream>

namespace chipcae::geometry::ecad::detail {
bool decode_gzip(std::string_view, std::string&, std::string&);
bool decode_lzw(std::string_view, std::string&, std::string&);
std::string lower(std::string);
using ByteSink = std::function<bool(std::string_view)>;
class GzipCursor {
    struct State;
    std::string error_;
    std::uint64_t consumed_ = 0;
    std::size_t output_limit_ = 32768;
    std::unique_ptr<State> state_;

public:
    explicit GzipCursor(std::istream&, std::uint64_t total_limit = ArchiveLimits{}.total_bytes);
    ~GzipCursor();
    bool next(std::string_view&, std::string&, std::size_t max_output = 32768);
    std::uint64_t consumed_bytes() const { return consumed_; }
};
bool stream_gzip(std::istream&, const ByteSink&, std::string&);
bool index_tar(const std::filesystem::path&,
               const ArchiveLimits&,
               std::vector<ArchiveEntry>&,
               std::vector<std::uint64_t>&,
               std::vector<UnsupportedElement>&,
               std::string&);
bool canonical_path(std::string&, std::string&);

} // namespace chipcae::geometry::ecad::detail
