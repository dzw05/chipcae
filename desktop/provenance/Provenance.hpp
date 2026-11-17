#pragma once
#include <QString>
namespace chipcae::desktop {
class ResultView;
struct Provenance {
    QString source, generatedAt, degraded;
    static Provenance read(const QString& directory);
    QString label() const;
    QString watermark() const;
};
bool writeCapturedPng(ResultView& view, const QString& path, const QString& watermark);
} // namespace chipcae::desktop
