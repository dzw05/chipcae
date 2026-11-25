#pragma once
#include "CaseDocument.hpp"

#include <QJsonArray>
#include <QStandardItemModel>

namespace chipcae::desktop {
// One active document. Callers own case switching and frozen submit snapshots.
class CaseTreeModel : public QStandardItemModel {
    Q_OBJECT
public:
    enum Role {
        PointerRole = Qt::UserRole + 1,
        SourceRole,
        IdentityRole,
        BodyAnnotationRole = Qt::UserRole + 4
    };
    explicit CaseTreeModel(CaseDocument* document, QObject* parent = nullptr);
    CaseDocument* document() const { return document_; }
    void setCaseSchemaPath(const QString& path);
    QString caseSchemaPath() const;
    QModelIndex locate(const QString& pointer, const QString& source = "case") const;
    QString addLoad(Json load);
    QString addHeatSource(const QString& layerName, double power);
    QString addFixedFace(const QString& target);
    bool remove(const QString& identity);
    bool edit(const QString& identity, Json value);
    void setThermalMode(bool thermal);
    void refresh();
    void applyPartSummary(const QJsonArray& parts);

private:
    CaseDocument* document_;
    QString caseSchemaPath_;
    void append(QStandardItem* parent,
                const Json& value,
                const QString& pointer,
                const QString& source,
                const QString& label);
    bool mutate(const QString& identity, const Json* value);
    QString freshId() const;
    bool apply(const Json& value);
};
} // namespace chipcae::desktop
