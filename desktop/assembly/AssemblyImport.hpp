#pragma once
#include <QStringList>
namespace chipcae::desktop {
bool isAssemblyPath(const QString& path);
QStringList assemblyNameFilters();
QStringList assemblyArguments(const QString& assemblyPath,
                              const QString& output,
                              const QString& report = {},
                              const QString& polyset = {},
                              bool acceptBTier = false);
} // namespace chipcae::desktop
