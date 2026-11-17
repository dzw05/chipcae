#pragma once
#include "CaseDocument.hpp"
#include "PreviewOptions.hpp"
#include "PreviewWorker.hpp"
#include "ResultSession.hpp"

#include <QPointer>
#include <vtkType.h>

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
};
} // namespace chipcae::desktop
