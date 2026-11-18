#include "PreviewOptions.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cmath>
#include <stdexcept>

namespace chipcae::desktop {
// Minimal linkable stubs keep the acceptance tests red without implementing panel behavior.
PreviewOptionsPanel::PreviewOptionsPanel(QWidget* parent) : QWidget(parent) {}
void PreviewOptionsPanel::setOptions(const PreviewOptions&) {}
PreviewOptions PreviewOptionsPanel::options() const {
    return {};
}
QString PreviewOptionsPanel::validationMessage() const {
    return {};
}

QString PreviewOptions::validate() const {
    if (nx.has_value() != ny.has_value() || (nx && (*nx <= 0 || *ny <= 0)))
        return "nx and ny must be paired positive integers";
    if (levels < 0 || !std::isfinite(maxRatio) || maxRatio < 1)
        return "invalid levels or max_ratio";
    return {};
}
QStringList PreviewOptions::arguments(const QString& stack,
                                      const QString& output,
                                      const QString& polyset) const {
    if (!validate().isEmpty())
        throw std::runtime_error(validate().toStdString());
    QStringList args{stack};
    if (!polyset.isEmpty())
        args << "--polyset" << polyset;
    if (nx)
        args << "--nx" << QString::number(*nx) << "--ny" << QString::number(*ny);
    args << "--levels" << QString::number(levels) << "--max-ratio"
         << QString::number(maxRatio, 'g', 17) << "-o" << output;
    if (!surface.isEmpty())
        args << "--surface" << surface;
    return args;
}
bool PreviewOptions::save(const QString& path) const {
    if (!validate().isEmpty())
        return false;
    QJsonObject o{{"levels", levels}, {"max_ratio", maxRatio}, {"surface", surface}};
    if (nx) {
        o["nx"] = *nx;
        o["ny"] = *ny;
    }
    const auto data = QJsonDocument(o).toJson();
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}
PreviewOptions PreviewOptions::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot read preview sidecar");
    auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        throw std::runtime_error("invalid preview sidecar");
    auto o = doc.object();
    auto integer = [&](const char* key) {
        auto v = o[key];
        if (!v.isDouble() || v.toDouble() != v.toInt(-1))
            throw std::runtime_error("invalid sidecar integer");
        return v.toInt();
    };
    PreviewOptions p;
    if (o.contains("nx"))
        p.nx = integer("nx");
    if (o.contains("ny"))
        p.ny = integer("ny");
    p.levels = integer("levels");
    p.maxRatio = o["max_ratio"].toDouble(-1);
    p.surface = o["surface"].toString();
    if (!p.validate().isEmpty())
        throw std::runtime_error(p.validate().toStdString());
    return p;
}
} // namespace chipcae::desktop
