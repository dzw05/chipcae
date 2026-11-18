#pragma once
#include "CaseDocument.hpp"
#include "PreviewOptions.hpp"
#include "PreviewWorker.hpp"
#include "ResultSession.hpp"

#include <QPointer>
#include <QWidget>
#include <memory>
#include <vtkType.h>

class vtkDataSet;

namespace chipcae::desktop {
struct PreviewBlock {
    QStringList layers, materials;
    QList<vtkIdType> visibleCells, highlightedCells;
    bool hexCutDowngrade = false;
};
class PreviewController : public QObject {
    Q_OBJECT
public:
    explicit PreviewController(QObject* parent = nullptr);
    // program/prefix allow SDK python + mock script; production uses chipcae-preview.
    bool start(CaseDocument* document,
               const QString& stack,
               const QString& output,
               const PreviewOptions& options = {},
               const QString& program = "chipcae-preview",
               const QStringList& prefix = {},
               const QString& manifest = {});
    void cancel();
    bool running() const { return running_; }
    QString error() const { return error_; }
    const ResultSession& session() const { return session_; }
    vtkSmartPointer<vtkDataObject> mesh() const { return mesh_; }
    vtkSmartPointer<vtkDataObject> surface() const { return surfaceMesh_; }
    const QList<PreviewBlock>& blocks() const { return blocks_; }
    const std::optional<MeshDigest>& digest() const { return digest_; }
    PreviewOptions effectiveOptions() const;
signals:
    void progress(const QString& stage, double fraction, const QString& message);
    void diagnostic(const QByteArray& line);
    void finished(bool success);

private:
    void line(const QByteArray& bytes);
    void complete(int code, QProcess::ExitStatus status);
    void inspect(vtkDataObject* object);
    PreviewWorker worker_;
    QPointer<CaseDocument> document_;
    ResultSession session_, surfaceSession_;
    vtkSmartPointer<vtkDataObject> mesh_, surfaceMesh_;
    QList<PreviewBlock> blocks_;
    std::optional<MeshDigest> digest_;
    QString error_, grid_, manifest_, surface_;
    bool running_ = false, success_ = false, cancelled_ = false;
    int nx_ = 0, ny_ = 0, finest_ = 0;
    double ratio_ = 10;
    PreviewOptions effective_;
};

enum class PreviewColorMode { Layer, Material };

class PreviewView : public QWidget {
    Q_OBJECT
public:
    explicit PreviewView(QWidget* parent = nullptr);
    ~PreviewView() override;
    using QWidget::show;
    bool show(const PreviewController& controller);
    bool show(vtkDataObject* mesh,
              const QStringList& layerNames,
              const QStringList& materialNames,
              const QList<PreviewBlock>& blocks,
              const PreviewOptions& effective);
    void setColorMode(PreviewColorMode mode);
    PreviewColorMode colorMode() const;
    QString summaryText() const;
    QString colorBarTitle() const;
    QString scalarArrayName() const;
    vtkDataSet* displayed() const;
    int highlightedCount() const;

private:
    struct State;
    std::unique_ptr<State> d;
};
} // namespace chipcae::desktop
