#include "AssemblyImport.hpp"
namespace chipcae::desktop {
// TEST-FIRST stubs: no assembly behavior implemented.
bool isAssemblyPath(const QString&) {
    return false;
}
QStringList assemblyNameFilters() {
    return {};
}
QStringList
assemblyArguments(const QString&, const QString&, const QString&, const QString&, bool) {
    return {};
}
} // namespace chipcae::desktop
