#include "PreviewOptions.hpp"

#include <QFile>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QSaveFile>
#include <QSignalBlocker>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace chipcae::desktop {
namespace {
int parseInteger(const QString& text, int invalid) {
    bool ok = false;
    const int value = text.toInt(&ok);
    return ok ? value : invalid;
}

std::optional<int> parseDimension(const QString& text) {
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return std::nullopt;
    // Keep malformed nonempty dimensions present and invalid for PreviewOptions::validate().
    return parseInteger(trimmed, 0);
}

double parseRatio(const QString& text) {
    const auto trimmed = text.trimmed().toLower();
    if (trimmed == "nan")
        return std::numeric_limits<double>::quiet_NaN();
    if (trimmed == "inf" || trimmed == "+inf")
        return std::numeric_limits<double>::infinity();
    if (trimmed == "-inf")
        return -std::numeric_limits<double>::infinity();
    bool ok = false;
    // QString conversion uses the C locale, independent of the user's locale.
    const double value = trimmed.toDouble(&ok);
    return ok ? value : std::numeric_limits<double>::quiet_NaN();
}

QString formatRatio(double value) {
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return std::signbit(value) ? "-inf" : "inf";
    return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
}
} // namespace

PreviewOptionsPanel::PreviewOptionsPanel(QWidget* parent) : QWidget(parent) {
    auto* form = new QFormLayout(this);
    auto addEditor = [&](const QString& name, const QString& label) {
        auto* editor = new QLineEdit(this);
        editor->setObjectName(name);
        form->addRow(label, editor);
        connect(editor, &QLineEdit::textChanged, this, &PreviewOptionsPanel::optionsEdited);
        return editor;
    };
    nx_ = addEditor("nx", tr("nx (empty = automatic)"));
    ny_ = addEditor("ny", tr("ny (empty = automatic)"));
    levels_ = addEditor("levels", tr("Levels (0 = automatic refinement)"));
    maxRatio_ = addEditor("max_ratio", tr("Max ratio"));
    surface_ = addEditor("surface", tr("Surface"));
    setOptions(PreviewOptions{});
}

void PreviewOptionsPanel::setOptions(const PreviewOptions& options) {
    const QSignalBlocker nxBlocker(nx_);
    const QSignalBlocker nyBlocker(ny_);
    const QSignalBlocker levelsBlocker(levels_);
    const QSignalBlocker ratioBlocker(maxRatio_);
    const QSignalBlocker surfaceBlocker(surface_);
    nx_->setText(options.nx ? QString::number(*options.nx) : QString{});
    ny_->setText(options.ny ? QString::number(*options.ny) : QString{});
    levels_->setText(QString::number(options.levels));
    maxRatio_->setText(formatRatio(options.maxRatio));
    surface_->setText(options.surface);
}

PreviewOptions PreviewOptionsPanel::options() const {
    return {parseDimension(nx_->text()),
            parseDimension(ny_->text()),
            parseInteger(levels_->text(), -1),
            parseRatio(maxRatio_->text()),
            surface_->text()};
}
QString PreviewOptionsPanel::validationMessage() const {
    return options().validate();
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
