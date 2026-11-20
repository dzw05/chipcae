#pragma once
#include "LayerModel.hpp"

#include <QWidget>
class QLabel;
class QPushButton;
namespace chipcae::desktop {
class StackEditor : public QWidget {
    Q_OBJECT
public:
    explicit StackEditor(CaseDocument* document, QWidget* parent = nullptr);
    LayerModel* model() const { return model_; }
    void setReadOnly(bool readOnly);
    bool isReadOnly() const { return readOnly_ || resultsDisplayed_; }
    // A separate, read-only source; never assigns to the case document.
    void setResultsStack(const Json& stack);
    void beginEdit();
    void revertEdit();
    bool save(const QString& path);
    bool previewStale() const { return stale_; }
    DocumentErrors validationErrors() const;
    bool actionsAllowed() const { return !isReadOnly() && validationErrors().empty(); }
signals:
    void previewInvalidated();

private:
    CaseDocument* document_;
    LayerModel* model_;
    QLabel* hint_;
    QPushButton* revert_;
    Json savedCase_, savedStack_;
    Json observedStack_;
    bool stale_ = false;
    bool readOnly_ = false, resultsDisplayed_ = false;
};
} // namespace chipcae::desktop
