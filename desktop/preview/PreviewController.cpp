#include "PreviewController.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vtkCellData.h>
#include <vtkCellType.h>
#include <vtkCompositeDataIterator.h>
#include <vtkCompositeDataSet.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>

namespace chipcae::desktop {
namespace {
QByteArray read(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot read preview input");
    return f.readAll();
}
QJsonObject object(const QByteArray& bytes) {
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(bytes, &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        throw std::runtime_error("invalid JSON object");
    return d.object();
}
} // namespace
PreviewController::PreviewController(QObject* parent) : QObject(parent) {
    connect(&worker_, &PreviewWorker::stdoutLine, this, &PreviewController::line);
    connect(&worker_, &PreviewWorker::stderrLine, this, &PreviewController::diagnostic);
    connect(&worker_, &PreviewWorker::finished, this, &PreviewController::complete);
    connect(&worker_,
            &PreviewWorker::errorOccurred,
            this,
            [this](QProcess::ProcessError e, const QString& message) {
                if (!running_)
                    return;
                error_ = message;
                if (e == QProcess::FailedToStart)
                    complete(-1, QProcess::CrashExit);
            });
}
bool PreviewController::start(CaseDocument* document,
                              const QString& stack,
                              const QString& output,
                              const PreviewOptions& options,
                              const QString& program,
                              const QStringList& prefix,
                              const QString& manifest) {
    if (running_)
        return false;
    error_.clear();
    success_ = false;
    cancelled_ = false;
    mesh_ = nullptr;
    surfaceMesh_ = nullptr;
    surface_ = options.surface;
    blocks_.clear();
    digest_.reset();
    grid_.clear();
    document_ = document;
    manifest_ = manifest;
    try {
        if (!document)
            throw std::runtime_error("missing case document");
        auto input = Json::parse(read(stack).toStdString());
        QString polyset;
        const QDir dir = QFileInfo(stack).absoluteDir();
        if (input.contains("external_refs")) {
            for (const auto& ref : input.at("external_refs").array_range()) {
                const auto relative = QString::fromStdString(ref.at("path").as<std::string>());
                auto path = QFileInfo(dir.filePath(relative)).canonicalFilePath();
                if (QDir::isAbsolutePath(relative) || relative.contains('\\') ||
                    !path.startsWith(dir.canonicalPath() + '/'))
                    throw std::runtime_error("invalid external reference");
                auto hash =
                    QCryptographicHash::hash(read(path), QCryptographicHash::Sha256).toHex();
                if (hash != QByteArray::fromStdString(ref.at("sha256").as<std::string>()).toLower())
                    throw std::runtime_error("INPUT_HASH_MISMATCH");
                if (!polyset.isEmpty())
                    throw std::runtime_error("multiple polyset references");
                polyset = path;
            }
        }
        auto args = prefix + options.arguments(QFileInfo(stack).absoluteFilePath(),
                                               QFileInfo(output).absoluteFilePath(),
                                               polyset);
        running_ = true;
        if (!worker_.start(program, args)) {
            running_ = false;
            throw std::runtime_error("preview worker busy");
        }
        return true;
    } catch (const std::exception& e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}
void PreviewController::cancel() {
    if (running_) {
        cancelled_ = true;
        worker_.cancel();
    }
}
void PreviewController::line(const QByteArray& bytes) {
    if (!running_ || !error_.isEmpty())
        return;
    try {
        if (success_)
            throw std::runtime_error("event after terminating success");
        auto o = object(bytes);
        auto p = o["progress"];
        if (!o["stage"].isString() || o["stage"].toString().isEmpty() || !o["message"].isString() ||
            !p.isDouble() || !std::isfinite(p.toDouble()) || p.toDouble() < 0 || p.toDouble() > 1)
            throw std::runtime_error("invalid JSONL event");
        auto stage = o["stage"].toString(), message = o["message"].toString();
        if (stage == "error")
            throw std::runtime_error(message.toStdString());
        if (stage == "success") {
            auto effective = object(message.toUtf8());
            auto n = QRegularExpression("^([1-9][0-9]*)x([1-9][0-9]*)$")
                         .match(effective["n0"].toString());
            bool xok = false, yok = false;
            nx_ = n.captured(1).toInt(&xok);
            ny_ = n.captured(2).toInt(&yok);
            auto level = effective["L"];
            ratio_ = effective["max_ratio"].toDouble(-1);
            grid_ = effective["grid"].toString();
            if (!n.hasMatch() || !xok || !yok || !level.isDouble() || level.toDouble() < 0 ||
                level.toDouble() >= std::numeric_limits<int>::max() ||
                level.toDouble() != std::floor(level.toDouble()) || !std::isfinite(ratio_) ||
                ratio_ < 1 || grid_.isEmpty())
                throw std::runtime_error("invalid effective preview options");
            finest_ = level.toInt();
            if (effective.contains("mesh_sha256")) {
                digest_ = MeshDigest::fromJson(
                    Json::parse(QJsonDocument(effective).toJson().toStdString()));
                if (digest_->nx0 != nx_ || digest_->ny0 != ny_ || digest_->L != finest_ ||
                    digest_->padded_cells < 0 || digest_->nz < 1)
                    throw std::runtime_error("invalid preview mesh digest");
            }
            success_ = true;
        }
        emit progress(stage, p.toDouble(), message);
    } catch (const std::exception& e) {
        error_ = QString::fromUtf8(e.what());
    }
}
void PreviewController::complete(int code, QProcess::ExitStatus status) {
    if (!running_)
        return;
    try {
        if (cancelled_)
            throw std::runtime_error("preview cancelled");
        if (!error_.isEmpty())
            throw std::runtime_error(error_.toStdString());
        if (code != 0 || status != QProcess::NormalExit || !success_)
            throw std::runtime_error("preview failed or missing success event");
        if (!document_)
            throw std::runtime_error("case document destroyed");
        if (!session_.open(grid_, manifest_))
            throw std::runtime_error(session_.error().toStdString());
        auto d = session_.meshDigest();
        if (!d.isEmpty()) {
            auto manifestDigest =
                MeshDigest::fromJson(Json::parse(QJsonDocument(d).toJson().toStdString()));
            if (digest_ && (digest_->nx0 != manifestDigest.nx0 ||
                            digest_->ny0 != manifestDigest.ny0 || digest_->L != manifestDigest.L ||
                            digest_->padded_cells != manifestDigest.padded_cells ||
                            digest_->nz != manifestDigest.nz ||
                            digest_->mesh_sha256 != manifestDigest.mesh_sha256))
                throw std::runtime_error("manifest mesh digest disagrees with preview");
            digest_ = manifestDigest;
            if (digest_->nx0 != nx_ || digest_->ny0 != ny_ || digest_->L != finest_)
                throw std::runtime_error("preview mesh digest disagrees with success");
        }
        mesh_ = session_.loadStep(0);
        if (!mesh_)
            throw std::runtime_error(session_.error().toStdString());
        inspect(mesh_);
        QString companion = surface_;
        if (companion.isEmpty() && grid_.endsWith(".vtkhdf")) {
            companion = grid_.left(grid_.size() - 7) + ".eb.vtkhdf";
            if (!QFileInfo::exists(companion))
                companion.clear();
        }
        if (!companion.isEmpty()) {
            // Pass a non-existent sidecar so a sibling volume manifest.json is not
            // discovered and loadStep(0) cannot substitute a different file.
            const QString noManifest = companion + ".ignore-manifest.json";
            if (!surfaceSession_.open(companion, noManifest))
                throw std::runtime_error(surfaceSession_.error().toStdString());
            surfaceMesh_ = surfaceSession_.loadStep(0);
            if (!surfaceMesh_)
                throw std::runtime_error(surfaceSession_.error().toStdString());
        }
        auto options = document_->meshOptions();
        options.nx = nx_;
        options.ny = ny_;
        options.maxRatio = ratio_;
        options.levels = finest_ + 1;
        document_->setMeshOptions(options);
    } catch (const std::exception& e) {
        error_ = QString::fromUtf8(e.what());
        mesh_ = nullptr;
        surfaceMesh_ = nullptr;
        blocks_.clear();
        digest_.reset();
    }
    running_ = false;
    emit finished(error_.isEmpty());
}
void PreviewController::inspect(vtkDataObject* object) {
    if (auto composite = vtkCompositeDataSet::SafeDownCast(object)) {
        auto it = vtkSmartPointer<vtkCompositeDataIterator>::Take(composite->NewIterator());
        for (it->InitTraversal(); !it->IsDoneWithTraversal(); it->GoToNextItem())
            inspect(it->GetCurrentDataObject());
    } else if (auto data = vtkDataSet::SafeDownCast(object)) {
        PreviewBlock block;
        block.layers = session_.layerNames();
        block.materials = session_.materialNames();
        auto kinds = data->GetCellData()->GetArray("cell_kind");
        for (vtkIdType i = 0; i < data->GetNumberOfCells(); ++i) {
            int kind = kinds ? static_cast<int>(kinds->GetComponent(i, 0)) : 0;
            if (kind != ResultSession::hiddenCellKind)
                block.visibleCells << i;
            if (kind == 1) {
                block.highlightedCells << i;
                if (data->GetCellType(i) == VTK_HEXAHEDRON)
                    block.hexCutDowngrade = true;
            }
        }
        blocks_ << block;
    }
}
// TEST-FIRST compilation stubs only. The acceptance tests intentionally remain red;
// snapshot capture, data extraction, coloring and rendering are not implemented here.
PreviewOptions PreviewController::effectiveOptions() const {
    return {};
}
PreviewView::PreviewView(QWidget* parent) : QWidget(parent) {}
bool PreviewView::show(const PreviewController&) {
    return false;
}
bool PreviewView::show(vtkDataObject*,
                       const QStringList&,
                       const QStringList&,
                       const QList<PreviewBlock>&,
                       const PreviewOptions&) {
    return false;
}
void PreviewView::setColorMode(PreviewColorMode) {}
PreviewColorMode PreviewView::colorMode() const {
    return PreviewColorMode::Layer;
}
QString PreviewView::summaryText() const {
    return {};
}
QString PreviewView::colorBarTitle() const {
    return {};
}
QString PreviewView::scalarArrayName() const {
    return {};
}
vtkDataSet* PreviewView::displayed() const {
    return nullptr;
}
int PreviewView::highlightedCount() const {
    return 0;
}
} // namespace chipcae::desktop
