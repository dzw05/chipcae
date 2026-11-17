#include "CaseTreeView.hpp"
namespace chipcae::desktop {
CaseTreeView::CaseTreeView(CaseTreeModel* model,
                           SchemaForm* form,
                           const QString& schemaDirectory,
                           QWidget* parent)
    : QTreeView(parent), tree_(model), form_(form), schemas_(schemaDirectory) {
    setModel(model);
    setHeaderHidden(true);
    connect(selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& index) { bind(index); });
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] { source_.clear(); });
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
    if (source_ == "case")
        tree_->document()->applyJson(value);
    else
        tree_->document()->applyStackJson(value);
    return true;
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
