#include "LayerModel.hpp"

#include <cmath>

namespace chipcae::desktop {
LayerModel::LayerModel(CaseDocument* document, QObject* parent)
    : QAbstractTableModel(parent), document_(document) {
    connect(document_, &CaseDocument::changed, this, [this] {
        beginResetModel();
        endResetModel();
    });
}
int LayerModel::rowCount(const QModelIndex& parent) const {
    auto stack = document_->stackJson();
    return parent.isValid() || !stack.contains("layers") ? 0 : int(stack.at("layers").size());
}
int LayerModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}
QStringList LayerModel::materialIds() const {
    QStringList ids;
    auto library = document_->materialLibrary();
    if (library.contains("materials"))
        for (const auto& material : library.at("materials").array_range())
            ids << QString::fromStdString(material.at("id").as<std::string>());
    return ids;
}
QVariant LayerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount() ||
        (role != Qt::DisplayRole && role != Qt::EditRole))
        return {};
    const auto stack = document_->stackJson();
    const auto& layer = stack.at("layers").at(index.row());
    switch (index.column()) {
    case Name:
        return QString::fromStdString(layer.at("name").as<std::string>());
    case Thickness:
        return layer.at("thickness").as<double>();
    case Material:
        return QString::fromStdString(layer.get_value_or<std::string>("material", ""));
    case Fraction:
        if (layer.contains("copper") && layer.at("copper").contains("fraction"))
            return layer.at("copper").at("fraction").as<double>();
        return {};
    case Geometry: {
        QStringList sources;
        if (layer.contains("copper") && layer.at("copper").contains("polygons")) {
            const auto& p = layer.at("copper").at("polygons");
            sources << QStringLiteral("polygons: %1 / layer %2")
                           .arg(QString::fromStdString(p.at("file").as<std::string>()),
                                QString::fromStdString(p.at("layer").as<std::string>()));
        }
        if (stack.contains("external_refs"))
            for (const auto& ref : stack.at("external_refs").array_range())
                sources << QStringLiteral("source %1 / sha256 %2")
                               .arg(QString::fromStdString(ref.at("path").as<std::string>()),
                                    QString::fromStdString(ref.at("sha256").as<std::string>()));
        return sources.join('\n');
    }
    default:
        return {};
    }
}
QVariant LayerModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    const QStringList labels{
        "Layer", "Thickness (m)", "Material id", "Copper φ", "Geometry (read-only)"};
    return labels.value(section);
}
Qt::ItemFlags LayerModel::flags(const QModelIndex& index) const {
    auto result = QAbstractTableModel::flags(index);
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= Geometry)
        return result;
    if (index.column() == Fraction) {
        const auto stack = document_->stackJson();
        const auto& layer = stack.at("layers").at(index.row());
        if (!layer.contains("copper") || layer.at("copper").contains("polygons"))
            return result;
    }
    return result | Qt::ItemIsEditable;
}
bool LayerModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    auto reject = [&](const QString& message) {
        error_ = message;
        emit editRejected(message);
        return false;
    };
    if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable))
        return reject("This field is read-only");
    auto stack = document_->stackJson();
    auto& layer = stack.at("layers").at(index.row());
    bool ok = true;
    if (index.column() == Name) {
        if (value.toString() == data(index).toString())
            return true;
        ok = document_->renameLayer(layer.at("name").as<std::string>(),
                                    value.toString().toStdString());
    } else {
        if (index.column() == Material) {
            if (!materialIds().contains(value.toString()))
                return reject("Unknown material library id");
            layer["material"] = value.toString().toStdString();
        } else {
            bool numeric = false;
            const double number = value.toDouble(&numeric);
            if (!numeric || !std::isfinite(number) ||
                (index.column() == Thickness ? number <= 0 : number < 0 || number > 1))
                return reject("Thickness must be > 0 m; copper φ must be in [0,1]");
            if (index.column() == Thickness)
                layer["thickness"] = number;
            else
                layer["copper"]["fraction"] = number;
        }
        ok = document_->setStackJson(stack);
    }
    if (!ok)
        return reject("Stack edit rejected");
    error_.clear();
    emit stackEdited();
    return true;
}
} // namespace chipcae::desktop
