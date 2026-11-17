#pragma once
#include <QStringList>
class QWidget;
namespace chipcae::desktop {
class ImportSourceDialog {
public:
    static QStringList nameFilters();
    static QString
    getImportPath(QWidget* parent, const QString& caption = {}, const QString& startDir = {});
    static bool isSupportedImportPath(const QString& path);
};
} // namespace chipcae::desktop
