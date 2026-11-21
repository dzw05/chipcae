#pragma once
#include "CaseDocument.hpp"

#include <QAbstractTableModel>
#include <QStringList>

namespace chipcae::desktop {
class LayerModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Thickness, Material, Fraction, Geometry, ColumnCount };
    explicit LayerModel(CaseDocument* document, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    void setResultsStack(const Json& stack);
    void setReadOnly(bool readOnly);
    QStringList materialIds() const;
    QString error() const { return error_; }
signals:
    void editRejected(const QString& message);
    void stackEdited();

private:
    Json displayStack() const;
    CaseDocument* document_;
    Json resultsStack_ = Json::object();
    bool readOnly_ = false;
    QString error_;
};
} // namespace chipcae::desktop
