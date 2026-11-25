#include "CaseTreeView.hpp"

#include <QMenu>
#include <QRegularExpression>

namespace chipcae::desktop {
CaseTreeView::CaseTreeView(CaseTreeModel* model,
                           SchemaForm* form,
                           const QString& schemaDirectory,
                           QWidget* parent)
    : QTreeView(parent), tree_(model), form_(form), schemas_(schemaDirectory) {
    setModel(model);
    setHeaderHidden(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (auto* menu = contextMenuFor(indexAt(pos))) {
            menu->exec(viewport()->mapToGlobal(pos));
            menu->deleteLater();
        }
    });
    connect(selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& index) { bind(index); });
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { source_.clear(); });
}
QMenu* CaseTreeView::contextMenuFor(const QModelIndex& index) {
    if (!index.isValid() || index.model() != tree_ ||
        index.data(CaseTreeModel::SourceRole) != "stack")
        return nullptr;
    static const QRegularExpression layerPath(QStringLiteral("\\A/layers/([0-9]+)\\z"));
    const auto match = layerPath.match(index.data(CaseTreeModel::PointerRole).toString());
    if (!match.hasMatch())
        return nullptr;
    bool valid = false;
    const auto layerIndex = match.captured(1).toULongLong(&valid);
    const auto stack = tree_->document()->stackJson();
    if (!valid || !stack.contains("layers") || !stack.at("layers").is_array() ||
        layerIndex >= stack.at("layers").size())
        return nullptr;
    const auto& layer = stack.at("layers").at(static_cast<std::size_t>(layerIndex));
    if (!layer.is_object() || !layer.contains("name") || !layer.at("name").is_string())
        return nullptr;
    const auto layerName = QString::fromStdString(layer.at("name").as<std::string>());
    bool thermal = false;
    const auto data = tree_->document()->toJson();
    if (data.contains("physics") && data.at("physics").is_array())
        for (const auto& physics : data.at("physics").array_range())
            if (physics.is_object() && physics.contains("id") && physics.at("id") == "thermal")
                thermal = true;
    auto* menu = new QMenu(this);
    auto* action = menu->addAction(QStringLiteral("在此层添加热源"));
    action->setEnabled(thermal);
    action->setToolTip(thermal ? QStringLiteral("") : QStringLiteral("需先切换到 Thermal 模式"));
    connect(action, &QAction::triggered, this, [this, layerName] {
        if (tree_->addHeatSource(layerName, 1.0).isEmpty())
            return;
        const auto loads = tree_->document()->toJson().at("loads");
        setCurrentIndex(tree_->locate("/loads/" + QString::number(loads.size() - 1)));
    });
    return menu;
}
void CaseTreeView::bind(const QModelIndex& index) {
    source_ = index.data(CaseTreeModel::SourceRole).toString();
    if (source_ != "case" && source_ != "stack") {
        source_.clear();
        return;
    }
    base_ = source_ == "case" ? tree_->document()->toJson() : tree_->document()->stackJson();
    if (form_->load(schemas_ +
                        (source_ == "case" ? "/case-1.0.schema.json" : "/stack-0.2.schema.json"),
                    base_))
        form_->setObjectPath(index.data(CaseTreeModel::PointerRole).toString());
}
bool CaseTreeView::commitEdit() {
    if (source_.isEmpty() || !form_->errors().empty())
        return false;
    const auto live =
        source_ == "case" ? tree_->document()->toJson() : tree_->document()->stackJson();
    if (live != base_)
        return false;
    auto value = form_->document();
    auto* document = tree_->document();
    if (source_ == "case") {
        document->applyJson(value);
        return document->toJson() == value;
    }
    const auto oldStack = document->stackJson();
    if (oldStack.contains("layers") && value.contains("layers") &&
        oldStack.at("layers").size() == value.at("layers").size()) {
        for (std::size_t i = 0; i < oldStack.at("layers").size(); ++i) {
            const auto oldName = oldStack.at("layers").at(i).at("name").as<std::string>();
            const auto newName = value.at("layers").at(i).at("name").as<std::string>();
            if (oldName != newName && !document->renameLayer(oldName, newName))
                return false;
        }
    }
    document->applyStackJson(value);
    return document->stackJson() == value;
}
void CaseTreeView::cancelEdit() {
    bind(currentIndex());
}
bool CaseTreeView::highlight(const DocumentError& error) {
    auto index = tree_->locate(error.pointer, error.source);
    if (!index.isValid())
        return false;
    setCurrentIndex(index);
    scrollTo(index);
    return true;
}
} // namespace chipcae::desktop
