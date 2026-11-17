#pragma once
#include <QStringList>
class QWidget;
namespace chipcae::desktop {
class ImportSourceDialog {
public:
    static QStringList nameFilters();
    // D09b test-first compile stub.
    static QString getMaterialMapPath(QWidget*, const QString& = {}) { return {}; }
    static QString
    getImportPath(QWidget* parent, const QString& caption = {}, const QString& startDir = {});
    static bool isSupportedImportPath(const QString& path);
};
} // namespace chipcae::desktop
