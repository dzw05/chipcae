#pragma once
#include "LayerModel.hpp"

#include <QWidget>
class QLabel;
namespace chipcae::desktop {
class StackEditor : public QWidget {
    Q_OBJECT
public:
    explicit StackEditor(CaseDocument* document, QWidget* parent = nullptr);
    LayerModel* model() const { return model_; }
    void beginEdit();
    void revertEdit();
    bool save(const QString& path);
    bool previewStale() const { return stale_; }
    DocumentErrors validationErrors() const;
    bool actionsAllowed() const { return validationErrors().empty(); }
signals:
    void previewInvalidated();

private:
    CaseDocument* document_;
    LayerModel* model_;
    QLabel* hint_;
    Json savedCase_, savedStack_;
    Json observedStack_;
    bool stale_ = false;
};
} // namespace chipcae::desktop
