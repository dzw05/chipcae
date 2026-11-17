#include "StackEditor.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QVBoxLayout>
#include <limits>

namespace chipcae::desktop {
namespace {
class NumberDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent,
                          const QStyleOptionViewItem&,
                          const QModelIndex& index) const override {
        auto* number = new QDoubleSpinBox(parent);
        number->setDecimals(15);
        number->setRange(
            0, index.column() == LayerModel::Fraction ? 1 : std::numeric_limits<double>::max());
        number->setSingleStep(index.column() == LayerModel::Fraction ? 0.01 : 0.000001);
        return number;
    }
};
class MaterialDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent,
                          const QStyleOptionViewItem&,
                          const QModelIndex& index) const override {
        auto* combo = new QComboBox(parent);
        const auto* model = qobject_cast<const LayerModel*>(index.model());
        combo->addItems(model->materialIds());
        return combo;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        static_cast<QComboBox*>(editor)->setCurrentText(index.data().toString());
    }
    void setModelData(QWidget* editor,
                      QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        model->setData(index, static_cast<QComboBox*>(editor)->currentText());
    }
};
} // namespace
StackEditor::StackEditor(CaseDocument* document, QWidget* parent)
    : QWidget(parent), document_(document), model_(new LayerModel(document, this)),
      hint_(new QLabel(this)), observedStack_(document->stackJson()) {
    auto* layout = new QVBoxLayout(this);
    auto* table = new QTableView(this);
    table->setModel(model_);
    table->setItemDelegateForColumn(LayerModel::Thickness, new NumberDelegate(table));
    table->setItemDelegateForColumn(LayerModel::Fraction, new NumberDelegate(table));
    table->setItemDelegateForColumn(LayerModel::Material, new MaterialDelegate(table));
    layout->addWidget(table);
    layout->addWidget(hint_);
    auto* revert = new QPushButton("Revert edit group", this);
    layout->addWidget(revert);
    connect(revert, &QPushButton::clicked, this, &StackEditor::revertEdit);
    connect(model_, &LayerModel::editRejected, hint_, &QLabel::setText);
    connect(document_, &CaseDocument::changed, this, [this] {
        if (!document_->isDirty() && document_->toJson() != savedCase_)
            beginEdit();
        if (observedStack_ != document_->stackJson()) {
            observedStack_ = document_->stackJson();
            stale_ = true;
            hint_->setText("Stack changed: preview must be regenerated.");
            emit previewInvalidated();
        }
    });
    beginEdit();
}
void StackEditor::beginEdit() {
    savedCase_ = document_->toJson();
    savedStack_ = document_->stackJson();
    document_->beginEdit();
}
void StackEditor::revertEdit() {
    document_->applyJson(savedCase_);
    document_->setStackJson(savedStack_);
}
bool StackEditor::save(const QString& path) {
    if (!actionsAllowed() || !document_->save(path))
        return false;
    beginEdit();
    return true;
}
DocumentErrors StackEditor::validationErrors() const {
    auto errors = document_->validate();
    for (const auto& error : document_->errors())
        if (error.code == "INPUT_HASH_MISMATCH")
            errors.push_back(error);
    return errors;
}
} // namespace chipcae::desktop
