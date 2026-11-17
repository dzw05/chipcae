#pragma once
#include <QStringList>
#include <optional>

namespace chipcae::desktop {
// UI sidecar: nx/ny never modify stack polygons. levels=0 selects automatic refinement.
struct PreviewOptions {
    std::optional<int> nx, ny;
    int levels = 0;
    double maxRatio = 10;
    QString surface;
    QString validate() const;
    QStringList
    arguments(const QString& stack, const QString& output, const QString& polyset = {}) const;
    bool save(const QString& path) const;
    static PreviewOptions load(const QString& path);
};
} // namespace chipcae::desktop
