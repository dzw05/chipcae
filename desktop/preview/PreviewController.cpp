#include "PreviewController.hpp"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>
#include <vtkActor.h>
#include <vtkAppendFilter.h>
#include <vtkCellData.h>
#include <vtkCellType.h>
#include <vtkCompositeDataIterator.h>
#include <vtkCompositeDataSet.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkDoubleArray.h>
#include <vtkExtractCells.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkLookupTable.h>
#include <vtkNew.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkUnstructuredGrid.h>
#include <vtkVariant.h>

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
    effective_ = {};
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
        effective_.nx = options.nx;
        effective_.ny = options.ny;
        effective_.levels = options.levels;
        effective_.maxRatio = options.maxRatio;
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
PreviewOptions PreviewController::effectiveOptions() const {
    return !running_ && error_.isEmpty() && mesh_ ? effective_ : PreviewOptions{};
}
namespace {
class PreviewRenderWidget : public QVTKOpenGLNativeWidget {
public:
    explicit PreviewRenderWidget(QWidget* parent) : QVTKOpenGLNativeWidget(parent) {
        setFormat(QVTKOpenGLNativeWidget::defaultFormat());
    }
    void requestRender() {
        dirty_ = true;
        update();
    }

protected:
    void paintGL() override {
        // QVTK normally blits its previous render on paint. Render changed data only
        // here, when Qt has made a real context current, never during show(mesh).
        if (dirty_ && renderWindow()) {
            dirty_ = false;
            renderWindow()->Render();
        }
        QVTKOpenGLNativeWidget::paintGL();
    }

private:
    bool dirty_ = true;
};
void dataSets(vtkDataObject* object, std::vector<vtkDataSet*>& result) {
    // Match inspect(): only dataset leaves consume a PreviewBlock.
    if (auto composite = vtkCompositeDataSet::SafeDownCast(object)) {
        auto it = vtkSmartPointer<vtkCompositeDataIterator>::Take(composite->NewIterator());
        for (it->InitTraversal(); !it->IsDoneWithTraversal(); it->GoToNextItem())
            dataSets(it->GetCurrentDataObject(), result);
    } else if (auto data = vtkDataSet::SafeDownCast(object)) {
        result.push_back(data);
    }
}
bool validCells(const QList<vtkIdType>& ids, vtkIdType count) {
    vtkIdType previous = -1;
    for (auto id : ids) {
        if (id <= previous || id >= count)
            return false;
        previous = id;
    }
    return true;
}
vtkSmartPointer<vtkUnstructuredGrid> extract(vtkDataSet* data, const QList<vtkIdType>& ids) {
    vtkNew<vtkExtractCells> filter;
    filter->SetInputData(data);
    filter->SetCellIds(ids.constData(), static_cast<vtkIdType>(ids.size()));
    filter->AssumeSortedAndUniqueIdsOn();
    filter->PassThroughCellIdsOff();
    filter->Update();
    auto result = vtkSmartPointer<vtkUnstructuredGrid>::New();
    result->DeepCopy(filter->GetOutput());
    return result;
}
vtkSmartPointer<vtkUnstructuredGrid>
append(std::vector<vtkSmartPointer<vtkUnstructuredGrid>>& pieces) {
    // vtkAppendFilter intersects array schemas. Normalize classification to doubles,
    // padding absent ids with NaN so a partial name table never loses known ids or
    // invents a category for cells whose classification is unknown.
    for (const char* name : {"layer_id", "material_id"}) {
        const bool present = std::any_of(pieces.begin(), pieces.end(), [name](const auto& p) {
            return p->GetCellData()->GetArray(name) != nullptr;
        });
        if (!present)
            continue;
        for (auto& piece : pieces) {
            auto source = piece->GetCellData()->GetArray(name);
            vtkNew<vtkDoubleArray> ids;
            ids->SetName(name);
            ids->SetNumberOfTuples(piece->GetNumberOfCells());
            for (vtkIdType i = 0; i < piece->GetNumberOfCells(); ++i)
                ids->SetValue(i,
                              source && i < source->GetNumberOfTuples()
                                  ? source->GetComponent(i, 0)
                                  : std::numeric_limits<double>::quiet_NaN());
            piece->GetCellData()->RemoveArray(name);
            piece->GetCellData()->AddArray(ids);
        }
    }
    auto result = vtkSmartPointer<vtkUnstructuredGrid>::New();
    if (!pieces.empty()) {
        vtkNew<vtkAppendFilter> filter;
        filter->MergePointsOff();
        for (const auto& piece : pieces)
            filter->AddInputData(piece);
        filter->Update();
        result->DeepCopy(filter->GetOutput());
    }
    return result;
}
} // namespace
struct PreviewView::State {
    struct Result {
        vtkSmartPointer<vtkUnstructuredGrid> data, highlights;
        QStringList layers, materials;
        QString summary;
    } result;
    PreviewColorMode mode = PreviewColorMode::Layer;
    QString title = "layer_id";
    QLabel* summaryLabel = nullptr;
    PreviewRenderWidget* widget = nullptr;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window =
        vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    vtkSmartPointer<vtkRenderer> renderer = vtkSmartPointer<vtkRenderer>::New();
    vtkSmartPointer<vtkDataSetMapper> mapper = vtkSmartPointer<vtkDataSetMapper>::New();
    vtkSmartPointer<vtkDataSetMapper> highlightMapper = vtkSmartPointer<vtkDataSetMapper>::New();
    vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
    vtkSmartPointer<vtkActor> highlightActor = vtkSmartPointer<vtkActor>::New();
    vtkSmartPointer<vtkScalarBarActor> bar = vtkSmartPointer<vtkScalarBarActor>::New();

    const char* arrayName() const {
        return mode == PreviewColorMode::Layer ? "layer_id" : "material_id";
    }
    void color() {
        mapper->SetScalarModeToUseCellFieldData();
        mapper->SelectColorArray(arrayName());
        mapper->SetColorModeToMapScalars();
        mapper->UseLookupTableScalarRangeOn();
        auto values = result.data ? result.data->GetCellData()->GetArray(arrayName()) : nullptr;
        std::set<int> ids;
        if (values)
            for (vtkIdType i = 0; i < values->GetNumberOfTuples(); ++i) {
                const double value = values->GetComponent(i, 0);
                if (std::isfinite(value) && value >= std::numeric_limits<int>::min() &&
                    value <= std::numeric_limits<int>::max() && std::trunc(value) == value)
                    ids.insert(static_cast<int>(value));
            }
        vtkNew<vtkLookupTable> colors;
        colors->IndexedLookupOn();
        colors->SetNanColor(0.65, 0.65, 0.65, 1);
        colors->SetNumberOfTableValues(static_cast<vtkIdType>(ids.size()));
        colors->Build();
        const auto& names = mode == PreviewColorMode::Layer ? result.layers : result.materials;
        QStringList labels;
        vtkIdType index = 0;
        for (int id : ids) {
            const QString name =
                id > 0 && id <= names.size() && !names[id - 1].isEmpty()
                    ? names[id - 1]
                    : QString(mode == PreviewColorMode::Layer ? "layer %1" : "material %1").arg(id);
            labels << name;
            colors->SetAnnotation(vtkVariant(static_cast<double>(id)), name.toStdString());
            // ID-based hue keeps a category's color stable when the visible set changes.
            const double hue = id * 0.618033988749895;
            const auto color = QColor::fromHsvF(hue - std::floor(hue), 0.6, 0.9);
            colors->SetTableValue(index++, color.redF(), color.greenF(), color.blueF(), 1);
        }
        title = arrayName();
        if (!labels.isEmpty())
            title += ": " + labels.join(", ");
        mapper->SetLookupTable(colors);
        mapper->SetScalarVisibility(values != nullptr);
        bar->SetLookupTable(colors);
        bar->SetTitle(title.toUtf8().constData());
        bar->SetVisibility(!ids.empty());
    }
};
PreviewView::PreviewView(QWidget* parent) : QWidget(parent), d(std::make_unique<State>()) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    d->summaryLabel = new QLabel(this);
    d->summaryLabel->setWordWrap(true);
    d->widget = new PreviewRenderWidget(this);
    layout->addWidget(d->summaryLabel);
    layout->addWidget(d->widget);
    d->widget->setRenderWindow(d->window);
    d->window->AddRenderer(d->renderer);
    d->renderer->SetBackground(0.12, 0.14, 0.18);
    d->actor->SetMapper(d->mapper);
    d->actor->GetProperty()->SetColor(0.65, 0.65, 0.65);
    d->highlightMapper->ScalarVisibilityOff();
    d->highlightActor->SetMapper(d->highlightMapper);
    d->highlightActor->GetProperty()->SetRepresentationToWireframe();
    d->highlightActor->GetProperty()->SetColor(1, 0.9, 0.1);
    d->highlightActor->GetProperty()->SetLineWidth(3);
    d->highlightActor->GetProperty()->LightingOff();
    d->renderer->AddActor(d->actor);
    d->renderer->AddActor(d->highlightActor);
    d->renderer->AddViewProp(d->bar);
    d->actor->VisibilityOff();
    d->highlightActor->VisibilityOff();
    d->bar->VisibilityOff();
}
PreviewView::~PreviewView() = default;
bool PreviewView::show(const PreviewController& controller) {
    if (controller.running() || !controller.error().isEmpty() || !controller.mesh() ||
        controller.effectiveOptions().levels == 0)
        return false;
    return show(controller.mesh(),
                controller.session().layerNames(),
                controller.session().materialNames(),
                controller.blocks(),
                controller.effectiveOptions());
}
bool PreviewView::show(vtkDataObject* mesh,
                       const QStringList& layerNames,
                       const QStringList& materialNames,
                       const QList<PreviewBlock>& blocks,
                       const PreviewOptions& effective) {
    if (!mesh)
        return false;
    std::vector<vtkDataSet*> inputs;
    dataSets(mesh, inputs);
    // An explicitly empty block list requests a valid empty view.
    if (!blocks.empty() && inputs.size() != static_cast<std::size_t>(blocks.size()))
        return false;
    std::vector<vtkSmartPointer<vtkUnstructuredGrid>> visible, highlighted;
    qsizetype highlightCount = 0;
    bool downgrade = false;
    for (qsizetype i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        auto input = inputs[static_cast<std::size_t>(i)];
        const auto count = input->GetNumberOfCells();
        if (!validCells(block.visibleCells, count) || !validCells(block.highlightedCells, count))
            return false;
        for (auto id : block.highlightedCells)
            if (!std::binary_search(block.visibleCells.begin(), block.visibleCells.end(), id))
                return false;
        if (block.highlightedCells.size() > std::numeric_limits<int>::max() - highlightCount)
            return false;
        highlightCount += block.highlightedCells.size();
        visible.push_back(extract(input, block.visibleCells));
        highlighted.push_back(extract(input, block.highlightedCells));
        downgrade = downgrade || block.hexCutDowngrade;
    }
    State::Result next;
    next.data = append(visible);
    next.highlights = append(highlighted);
    next.layers = layerNames;
    next.materials = materialNames;
    next.summary = QString("nx=%1 ny=%2 levels=%3 max_ratio=%4")
                       .arg(effective.nx ? QString::number(*effective.nx) : "auto")
                       .arg(effective.ny ? QString::number(*effective.ny) : "auto")
                       .arg(effective.levels)
                       .arg(QString::number(effective.maxRatio, 'g', 15));
    if (downgrade)
        next.summary += "\nhex cut downgrade";
    d->result = std::move(next);
    d->mapper->SetInputData(d->result.data);
    d->highlightMapper->SetInputData(d->result.highlights);
    d->actor->SetVisibility(d->result.data->GetNumberOfCells() > 0);
    d->highlightActor->SetVisibility(d->result.highlights->GetNumberOfCells() > 0);
    d->summaryLabel->setText(d->result.summary);
    d->color();
    d->renderer->ResetCamera();
    d->widget->requestRender();
    return true;
}
void PreviewView::setColorMode(PreviewColorMode mode) {
    d->mode = mode;
    d->color();
    d->widget->requestRender();
}
PreviewColorMode PreviewView::colorMode() const {
    return d->mode;
}
QString PreviewView::summaryText() const {
    return d->result.summary;
}
QString PreviewView::colorBarTitle() const {
    return d->title;
}
QString PreviewView::scalarArrayName() const {
    return d->arrayName();
}
// Test-first API stubs: copper availability and coloring are not implemented yet.
bool PreviewView::hasCopperPhi() const {
    return false;
}
std::array<double, 2> PreviewView::colorRange() const {
    return {};
}
vtkDataSet* PreviewView::displayed() const {
    return d->result.data;
}
int PreviewView::highlightedCount() const {
    return d->result.highlights ? static_cast<int>(d->result.highlights->GetNumberOfCells()) : 0;
}
} // namespace chipcae::desktop
