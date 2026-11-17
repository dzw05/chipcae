#pragma once
#include "CaseTreeModel.hpp"
#include "SchemaForm.hpp"

#include <QTreeView>
namespace chipcae::desktop {
class CaseTreeView : public QTreeView {
    Q_OBJECT
public:
    CaseTreeView(CaseTreeModel* model,
                 SchemaForm* form,
                 const QString& schemaDirectory,
                 QWidget* parent = nullptr);
    bool commitEdit();
    void cancelEdit();
    bool highlight(const DocumentError& error);

private:
    CaseTreeModel* tree_;
    SchemaForm* form_;
    QString schemas_, source_;
    Json base_;
    void bind(const QModelIndex& index);
};
} // namespace chipcae::desktop
