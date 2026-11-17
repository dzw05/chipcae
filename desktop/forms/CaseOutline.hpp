#pragma once
#include "SchemaForm.hpp"

#include <QTreeView>
class QStandardItem;
class QStandardItemModel;
namespace chipcae::desktop {
class CaseOutline : public QTreeView {
    Q_OBJECT
public:
    explicit CaseOutline(SchemaForm* form, QWidget* parent = nullptr);
    bool setCaseDocument(const QString& schemaFile, const Json& document);
    // Separate stack document: case.stack is a file reference, not inline layers.
    void setStackDocument(const QString& schemaFile, const Json& document);
    void setResultDocument(const QString& schemaFile, const Json& document);

private:
    SchemaForm* form_;
    QStandardItemModel* model_;
    QString caseSchema_;
    Json case_;
    QString stackSchema_;
    Json stack_;
    QString resultSchema_;
    Json result_;
    int active_ = 0;
    bool selecting_ = false;
    void refresh();
    void append(QStandardItem* parent, const Json& value, const QString& path, int source);
};
} // namespace chipcae::desktop
