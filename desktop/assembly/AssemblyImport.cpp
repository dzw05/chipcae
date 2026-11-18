#include "AssemblyImport.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace chipcae::desktop {
bool isAssemblyPath(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() &&
           document.object().value("schema").toString() == "chipcae.assembly/0.1";
}
QStringList assemblyNameFilters() {
    return {"Assembly (chipcae.assembly) (*.json)"};
}
QStringList assemblyArguments(const QString& path,
                              const QString& output,
                              const QString& report,
                              const QString& polyset,
                              bool acceptBTier) {
    QStringList arguments{"--assembly", path, "-o", output};
    if (!report.isEmpty())
        arguments << "--report" << report;
    if (!polyset.isEmpty())
        arguments << "--polyset" << polyset;
    if (acceptBTier)
        arguments << "--accept-b-tier";
    return arguments;
}
} // namespace chipcae::desktop
