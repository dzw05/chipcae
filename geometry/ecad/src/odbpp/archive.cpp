#include "archive.hpp"
namespace chipcae::geometry::ecad::detail {
ArchiveReader::ArchiveReader(const std::filesystem::path&) {}
const std::string& ArchiveReader::open_error() const {
    static const std::string error = "IMPORT_REJECTED: not implemented";
    return error;
}
const std::vector<ArchiveEntry>& ArchiveReader::entries() const {
    static const std::vector<ArchiveEntry> empty;
    return empty;
}
bool ArchiveReader::read_bytes(const std::string&, std::string&, std::string& err) const {
    err = open_error();
    return false;
}
bool ArchiveReader::for_each_line(const std::string&,
                                  const std::function<bool(std::string_view)>&,
                                  std::string& err) const {
    err = open_error();
    return false;
}
} // namespace chipcae::geometry::ecad::detail
