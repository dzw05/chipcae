#include "ResultView.hpp"

#include <QDebug>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vtkActor.h>
#include <vtkAppendFilter.h>
#include <vtkBoundingBox.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCell.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCellPicker.h>
#include <vtkClipDataSet.h>
#include <vtkCompositeDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkDataSetTriangleFilter.h>
#include <vtkDoubleArray.h>
#include <vtkExtractCells.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInformation.h>
#include <vtkLookupTable.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPNGWriter.h>
#include <vtkPartitionedDataSet.h>
#include <vtkPartitionedDataSetCollection.h>
#include <vtkPlane.h>
#include <vtkPointData.h>
#include <vtkPointLocator.h>
#include <vtkPointSet.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkScalarBarActor.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkUnstructuredGrid.h>
#include <vtkVariant.h>
#include <vtkWindowToImageFilter.h>

namespace chipcae::desktop {
namespace {
QString canonical(QString n) {
    if (n == "cut_poly")
        return "cut";
    if (n == "embedded_boundary")
        return "eb_surface";
    return n;
}
QString gid(vtkDataSet* data, vtkIdType cell) {
    if (!data || cell < 0 || cell >= data->GetNumberOfCells())
        return QStringLiteral("未知");
    auto a = data->GetCellData()->GetAbstractArray("elem_gid");
    return a ? QString::number(a->GetVariantValue(cell).ToLongLong()) : QStringLiteral("未知");
}
vtkSmartPointer<vtkUnstructuredGrid> extract(vtkDataSet* data, vtkIdList* ids) {
    vtkNew<vtkExtractCells> filter;
    filter->SetInputData(data);
    filter->SetCellList(ids);
    filter->Update();
    auto out = vtkSmartPointer<vtkUnstructuredGrid>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}
vtkSmartPointer<vtkUnstructuredGrid>
clipped(vtkUnstructuredGrid* grid, std::array<double, 3> origin, std::array<double, 3> normal) {
    vtkNew<vtkPlane> plane;
    plane->SetOrigin(origin.data());
    plane->SetNormal(normal.data());
    vtkNew<vtkClipDataSet> filter;
    filter->SetInputData(grid);
    filter->SetClipFunction(plane);
    filter->SetOutputPointsPrecision(vtkAlgorithm::DOUBLE_PRECISION);
    filter->Update();
    auto out = vtkSmartPointer<vtkUnstructuredGrid>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}
double determinant(const double* a, const double* b, const double* c, const double* o) {
    double x[3], y[3], z[3], cross[3];
    vtkMath::Subtract(a, o, x);
    vtkMath::Subtract(b, o, y);
    vtkMath::Subtract(c, o, z);
    vtkMath::Cross(y, z, cross);
    return vtkMath::Dot(x, cross);
}
struct BoundaryGrid {
    vtkUnstructuredGrid* shown;
    vtkSmartPointer<vtkPoints> reference;
};
// Use undeformed face connectivity to find neighbors. Canonicalize duplicate vertices only
// in this temporary topology table; each owner keeps its own displayed solution points.
vtkSmartPointer<vtkActor> layerBoundaries(const std::vector<BoundaryGrid>& grids) {
    using Edge = std::pair<vtkIdType, vtkIdType>;
    struct Owner {
        std::size_t cell;
        double layer;
        std::vector<Edge> edges;
    };
    vtkBoundingBox bounds;
    vtkIdType pointCount = 0;
    auto finite = [](const double* p) {
        return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
    };
    for (const auto& grid : grids)
        for (vtkIdType i = 0; i < grid.reference->GetNumberOfPoints(); ++i) {
            double p[3];
            grid.reference->GetPoint(i, p);
            if (finite(p)) {
                bounds.AddPoint(p);
                ++pointCount;
            }
        }
    if (!bounds.IsValid())
        return nullptr;
    double box[6], lengths[3];
    bounds.GetBounds(box);
    bounds.GetLengths(lengths);
    double magnitude = 0;
    for (double value : box)
        magnitude = std::max(magnitude, std::abs(value));
    // Absolute distance: 1e-10 of the undeformed extent, with a coordinate-roundoff floor.
    // This joins numerical duplicates without merging displayed points or averaging U.
    const double tolerance = std::max(1e-10 * std::max({lengths[0], lengths[1], lengths[2]}),
                                      64 * std::numeric_limits<double>::epsilon() * magnitude);
    vtkNew<vtkPoints> vertices;
    vertices->SetDataTypeToDouble();
    vtkNew<vtkPointLocator> locator;
    locator->SetTolerance(tolerance);
    locator->InitPointInsertion(vertices, box, pointCount);
    std::map<std::vector<vtkIdType>, std::vector<Owner>> faces;
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    std::size_t cellId = 0;
    for (const auto& entry : grids) {
        auto grid = entry.shown;
        auto layers = grid->GetCellData()->GetArray("layer_id");
        if (!layers)
            continue;
        std::vector<vtkIdType> topology(grid->GetNumberOfPoints(), -1);
        std::vector<vtkIdType> displayed(grid->GetNumberOfPoints(), -1);
        for (vtkIdType i = 0; i < grid->GetNumberOfPoints(); ++i) {
            double reference[3], shown[3];
            entry.reference->GetPoint(i, reference);
            grid->GetPoint(i, shown);
            if (finite(reference) && finite(shown)) {
                locator->InsertUniquePoint(reference, topology[i]);
                displayed[i] = points->InsertNextPoint(shown);
            }
        }
        for (vtkIdType i = 0; i < grid->GetNumberOfCells(); ++i, ++cellId) {
            auto cell = grid->GetCell(i);
            const double layer = layers->GetComponent(i, 0);
            if (cell->GetCellDimension() != 3 || !std::isfinite(layer))
                continue;
            for (int f = 0; f < cell->GetNumberOfFaces(); ++f) {
                auto face = cell->GetFace(f);
                std::vector<vtkIdType> key;
                for (vtkIdType j = 0; j < face->GetNumberOfPoints(); ++j)
                    key.push_back(topology[face->GetPointId(j)]);
                std::sort(key.begin(), key.end());
                if (key.size() < 3 || key.front() < 0 ||
                    std::adjacent_find(key.begin(), key.end()) != key.end())
                    continue;
                Owner owner{cellId, layer, {}};
                // Preserve the face's actual edges; sorting the face key loses its perimeter.
                for (int e = 0; e < face->GetNumberOfEdges(); ++e) {
                    auto edge = face->GetEdge(e);
                    const auto a = displayed[edge->GetPointId(0)];
                    const auto b = displayed[edge->GetPointId(1)];
                    if (a >= 0 && b >= 0 && a != b)
                        owner.edges.emplace_back(std::min(a, b), std::max(a, b));
                }
                faces[key].push_back(std::move(owner));
            }
        }
    }
    std::set<Edge> edges;
    for (const auto& [key, owners] : faces) {
        for (const auto& owner : owners)
            if (std::any_of(owners.begin(), owners.end(), [&](const Owner& other) {
                    return owner.cell != other.cell && owner.layer != other.layer;
                }))
                edges.insert(owner.edges.begin(), owner.edges.end());
    }
    if (edges.empty())
        return nullptr;
    vtkNew<vtkCellArray> lines;
    for (const auto& [a, b] : edges) {
        const vtkIdType ids[] = {a, b};
        lines->InsertNextCell(2, ids);
    }
    vtkNew<vtkPolyData> data;
    data->SetPoints(points);
    data->SetLines(lines);
    vtkNew<vtkDataSetMapper> mapper;
    mapper->SetInputData(data);
    mapper->ScalarVisibilityOff();
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetObjectName("layerBoundaries");
    actor->SetMapper(mapper);
    actor->PickableOff();
    actor->GetProperty()->SetColor(0.12, 0.12, 0.12);
    actor->GetProperty()->SetLineWidth(2);
    return actor;
}
} // namespace
struct ResultView::State {
    struct Block {
        QString name;
        vtkSmartPointer<vtkDataSet> raw;
        vtkSmartPointer<vtkUnstructuredGrid> shown;
        vtkSmartPointer<vtkActor> actor;
    };
    std::vector<Block> blocks;
    ResultSession* session = nullptr;
    qint64 step = 0;
    ResultColorField field = ResultColorField::Temperature;
    ColorRange range;
    QString watermark, banner;
    QLabel* bannerLabel = nullptr;
    QLabel* fieldStatusLabel = nullptr;
    bool loaded = false;
    bool cameraInitialized = false;
    double boundarySetupMs = 0;
    vtkSmartPointer<vtkActor> boundaries;
    vtkSmartPointer<vtkTextActor> overlay = vtkSmartPointer<vtkTextActor>::New();
    double scale = 1;
    int layer = 0;
    bool showVoid = false, clip = false;
    std::array<double, 3> origin{}, normal{1, 0, 0};
    std::map<QString, bool> visibility;
    QString error;
    vtkSmartPointer<vtkRenderer> renderer = vtkSmartPointer<vtkRenderer>::New();
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window =
        vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
};
ResultView::ResultView(QWidget* parent) : QWidget(parent), d(std::make_unique<State>()) {
    auto widget = new QVTKOpenGLNativeWidget(this);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    d->bannerLabel = new QLabel(this);
    d->bannerLabel->setWordWrap(true);
    d->bannerLabel->hide();
    d->fieldStatusLabel = new QLabel(this);
    d->fieldStatusLabel->setObjectName("fieldStatusLabel");
    d->fieldStatusLabel->setWordWrap(true);
    d->fieldStatusLabel->hide();
    auto status = new QHBoxLayout;
    status->addWidget(d->bannerLabel);
    status->addWidget(d->fieldStatusLabel);
    layout->addLayout(status);
    layout->addWidget(widget);
    d->overlay->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    d->overlay->SetPosition(0.25, 0.3);
    d->overlay->GetTextProperty()->SetFontSize(64);
    d->overlay->GetTextProperty()->SetOrientation(30);
    d->overlay->GetTextProperty()->SetOpacity(0.3);
    // ASCII viewport text uses VTK's built-in font on every supported system.
    d->overlay->PickableOff();
    widget->setRenderWindow(d->window);
    d->window->AddRenderer(d->renderer);
    d->renderer->SetBackground(0.12, 0.14, 0.18);
    vtkNew<vtkCallbackCommand> click;
    click->SetClientData(this);
    click->SetCallback([](vtkObject* object, unsigned long, void* context, void*) {
        auto view = static_cast<ResultView*>(context);
        auto interactor = vtkRenderWindowInteractor::SafeDownCast(object);
        auto pos = interactor->GetEventPosition();
        emit view->elementPicked(view->pick(pos[0], pos[1]));
    });
    d->window->GetInteractor()->AddObserver(vtkCommand::LeftButtonPressEvent, click);
}
ResultView::~ResultView() = default;
bool ResultView::loadStep(ResultSession& session, qint64 step) {
    auto object = session.loadStep(step);
    if (!object) {
        d->error = session.error();
        d->loaded = false;
        d->renderer->RemoveActor(d->boundaries);
        d->boundaries = nullptr;
        d->fieldStatusLabel->clear();
        d->fieldStatusLabel->hide();
        if (isVisible())
            d->window->Render();
        return false;
    }
    d->blocks.clear();
    d->session = &session;
    d->step = step;
    auto add = [&](QString name, vtkDataSet* ds) {
        if (!ds)
            return;
        vtkSmartPointer<vtkDataSet> copy;
        copy.TakeReference(ds->NewInstance());
        copy->DeepCopy(ds);
        d->blocks.push_back({canonical(name), copy, nullptr, nullptr});
    };
    if (auto pdc = vtkPartitionedDataSetCollection::SafeDownCast(object)) {
        for (unsigned i = 0; i < pdc->GetNumberOfPartitionedDataSets(); ++i) {
            const char* label = pdc->GetMetaData(i)->Get(vtkCompositeDataSet::NAME());
            QString name = label ? QString::fromUtf8(label) : QString{};
            if (name.isEmpty() && i < unsigned(session.blocks().size()))
                name = session.blocks()[i].toObject()["name"].toString();
            auto pd = pdc->GetPartitionedDataSet(i);
            for (unsigned j = 0; j < pd->GetNumberOfPartitions(); ++j)
                add(name, pd->GetPartition(j));
        }
    } else if (auto pd = vtkPartitionedDataSet::SafeDownCast(object)) {
        for (unsigned j = 0; j < pd->GetNumberOfPartitions(); ++j)
            add("hex", pd->GetPartition(j));
    } else
        add("hex", vtkDataSet::SafeDownCast(object));
    std::map<QString, vtkSmartPointer<vtkAppendFilter>> joined;
    for (auto& b : d->blocks) {
        if (!joined.contains(b.name))
            joined[b.name] = vtkSmartPointer<vtkAppendFilter>::New();
        joined[b.name]->AddInputData(b.raw);
    }
    d->blocks.clear();
    for (auto& [name, filter] : joined) {
        filter->Update();
        add(name, filter->GetOutput());
    }
    d->loaded = true;
    rebuild();
    if (!d->error.isEmpty())
        return false;
    const auto start = std::chrono::steady_clock::now();
    if (!d->cameraInitialized) {
        auto camera = d->renderer->GetActiveCamera();
        camera->SetFocalPoint(0, 0, 0);
        camera->SetPosition(0, 0, 1);
        camera->SetViewUp(0, 1, 0);
        d->renderer->ResetCamera();
        camera->Azimuth(45);
        camera->Elevation(30);
        camera->OrthogonalizeViewUp();
        d->renderer->ResetCameraClippingRange();
        d->cameraInitialized = true;
    }
    const double cameraSetupMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    qInfo() << "result_default_view_setup_ms" << d->boundarySetupMs + cameraSetupMs;
    if (isVisible())
        d->window->Render();
    return true;
}
void ResultView::setColorField(ResultColorField field) {
    d->field = field;
    rebuild();
}
ResultColorField ResultView::colorField() const {
    return d->field;
}
ColorRange ResultView::colorRange() const {
    return d->range;
}
QString ResultView::colorBarTitle() const {
    if (!d->range.valid)
        return {};
    switch (d->field) {
    case ResultColorField::Temperature:
        return "T (K)";
    case ResultColorField::DisplacementMagnitude:
        return "|U| (m)";
    case ResultColorField::WarpageUz:
        return "U_z (m)";
    case ResultColorField::LayerId:
    case ResultColorField::MaterialId: {
        const bool layer = d->field == ResultColorField::LayerId;
        const double first = std::ceil(d->range.min), last = std::floor(d->range.max);
        if (!std::isfinite(first) || !std::isfinite(last) || first > last ||
            first < std::numeric_limits<int>::min() || last > std::numeric_limits<int>::max())
            return {};
        QStringList names;
        const int end = static_cast<int>(last);
        for (int id = static_cast<int>(first);; ++id) {
            auto name = d->session
                            ? (layer ? d->session->layerName(id) : d->session->materialName(id))
                            : QString{};
            if (name.isEmpty())
                name = QString("%1 %2").arg(layer ? "layer" : "material").arg(id);
            names << name;
            if (id == end)
                break;
        }
        return QStringLiteral("%1: %2").arg(layer ? QStringLiteral("叠层") : QStringLiteral("材料"),
                                            names.join(", "));
    }
    }
    return {};
}
QString ResultView::tmaxText() const {
    if (d->session)
        for (bool exactStep : {true, false})
            for (auto value : d->session->summary()) {
                const auto entry = value.toObject();
                if (entry.contains("step") != exactStep || entry["name"] != "T_max" ||
                    (entry.contains("step") && entry["step"].toInteger(-1) != d->step) ||
                    !entry["value"].isDouble())
                    continue;
                QString text = QString("T_max: %1 %2")
                                   .arg(entry["value"].toDouble(), 0, 'g', 16)
                                   .arg(entry["unit"].toString());
                for (auto key : {"region", "region_id", "body"}) {
                    auto region = entry[key].toString();
                    if (!region.isEmpty()) {
                        text += " region=" + region;
                        break;
                    }
                }
                bool selectedLocationArray = false;
                for (auto key : {"position", "xyz", "location"}) {
                    if (!entry[key].isArray())
                        continue;
                    auto xyz = entry[key].toArray();
                    if (xyz.size() == 3 && xyz[0].isDouble() && xyz[1].isDouble() &&
                        xyz[2].isDouble()) {
                        selectedLocationArray = true;
                        text += QString(" at=(%1,%2,%3)")
                                    .arg(xyz[0].toDouble(), 0, 'g', 16)
                                    .arg(xyz[1].toDouble(), 0, 'g', 16)
                                    .arg(xyz[2].toDouble(), 0, 'g', 16);
                        break;
                    }
                }
                if (!selectedLocationArray && !entry["location"].toString().isEmpty())
                    text += " location=" + entry["location"].toString();
                return text;
            }
    return "T_max: unavailable";
}
bool ResultView::setProvenance(const QJsonObject& provenance) {
    auto severity = [](const QJsonValue& value) {
        if (!value.isString())
            return -1;
        const auto text = value.toString();
        return text == "real" ? 0 : text == "precomputed" ? 1 : text == "mock" ? 2 : -1;
    };
    const int sourceRank = severity(provenance["source"]);
    const int effectiveRank = provenance.contains("effective_source")
                                  ? severity(provenance["effective_source"])
                                  : sourceRank;
    // A present degraded flag must be boolean; malformed flags use unknown/MOCK markings.
    const bool validDegraded = !provenance.contains("degraded") || provenance["degraded"].isBool();
    const bool valid = provenance["schema"] == "chipcae.provenance/0.1" && sourceRank >= 0 &&
                       effectiveRank >= sourceRank && validDegraded;
    const int rank = effectiveRank;
    d->watermark.clear();
    d->banner.clear();
    if (!valid) {
        d->watermark = "MOCK";
        d->banner = QStringLiteral("来源未知 — MOCK");
    } else if (rank == 2) {
        d->watermark = "MOCK";
        d->banner = QStringLiteral("MOCK 合成数据");
    } else if (rank == 1) {
        d->watermark = QStringLiteral("预计算");
        d->banner = QStringLiteral("预计算数据");
    } else if (provenance["degraded"].toBool()) {
        d->watermark = QStringLiteral("降级");
        d->banner = QStringLiteral("真实计算 — 降级数据");
    }
    d->bannerLabel->setText(d->banner);
    d->bannerLabel->setVisible(!d->banner.isEmpty());
    const auto viewportText = d->watermark == QStringLiteral("预计算") ? QString("PRECOMPUTED")
                              : d->watermark == QStringLiteral("降级") ? QString("DEGRADED")
                                                                       : d->watermark;
    d->overlay->SetInput(viewportText.toUtf8().constData());
    rebuild();
    return valid;
}
void ResultView::clearProvenance() {
    d->watermark.clear();
    d->banner.clear();
    d->bannerLabel->clear();
    d->bannerLabel->hide();
    d->overlay->SetInput("");
    rebuild();
}
QString ResultView::provenanceWatermark() const {
    return d->watermark;
}
QString ResultView::provenanceBanner() const {
    return d->banner;
}
void ResultView::setWarpageScale(double v) {
    d->scale = v;
    rebuild();
}
void ResultView::setLayer(int v) {
    d->layer = v;
    rebuild();
}
void ResultView::setShowVoid(bool v) {
    d->showVoid = v;
    rebuild();
}
void ResultView::setBlockVisible(const QString& n, bool v) {
    d->visibility[canonical(n)] = v;
    rebuild();
}
void ResultView::setClip(bool v, std::array<double, 3> o, std::array<double, 3> n) {
    d->clip = v;
    d->origin = o;
    d->normal = n;
    rebuild();
}
QString ResultView::summary(const QString& name) const {
    if (d->session)
        for (auto v : d->session->summary()) {
            auto s = v.toObject();
            if (s["name"] == name && s["step"].toInteger(-1) == d->step && s["value"].isDouble())
                return QString("%1: %2 %3")
                    .arg(name)
                    .arg(s["value"].toDouble(), 0, 'g', 16)
                    .arg(s["unit"].toString());
        }
    return name + ": unavailable";
}
void ResultView::rebuild() {
    d->error.clear();
    d->renderer->RemoveAllViewProps();
    d->boundaries = nullptr;
    d->boundarySetupMs = 0;
    d->fieldStatusLabel->clear();
    d->fieldStatusLabel->hide();
    d->range = {};
    if (!d->watermark.isEmpty())
        d->renderer->AddViewProp(d->overlay);
    if (d->clip && vtkMath::Norm(d->normal.data()) == 0) {
        d->error = "Clip normal must be nonzero";
        emit displayChanged();
        return;
    }
    vtkNew<vtkLookupTable> lut;
    lut->SetHueRange(0.667, 0);
    lut->Build();
    std::vector<BoundaryGrid> shown;
    for (auto& b : d->blocks) {
        vtkNew<vtkIdList> ids;
        auto kinds = b.raw->GetCellData()->GetArray("cell_kind");
        auto layers = b.raw->GetCellData()->GetArray("layer_id");
        bool visible = !d->visibility.contains(b.name) || d->visibility[b.name];
        for (vtkIdType i = 0; i < b.raw->GetNumberOfCells(); ++i)
            if (visible &&
                (d->showVoid || !kinds ||
                 kinds->GetComponent(i, 0) != ResultSession::hiddenCellKind) &&
                (!d->layer || (layers && layers->GetComponent(i, 0) == d->layer)))
                ids->InsertNextId(i);
        b.shown = extract(b.raw, ids);
        if (d->clip && b.shown->GetNumberOfCells()) {
            vtkSmartPointer<vtkUnstructuredGrid> t = b.shown;
            QString error;
            if (b.shown->GetCell(0)->GetCellDimension() == 3)
                t = tetrahedralize(b.shown, error);
            if (!t) {
                d->error = error;
                b.shown->Initialize();
                continue;
            }
            b.shown = clipped(t, d->origin, d->normal);
        }
        if (b.shown->GetNumberOfCells() && b.shown->GetCellData()->HasArray("layer_id")) {
            vtkNew<vtkPoints> reference;
            reference->DeepCopy(b.shown->GetPoints());
            shown.push_back({b.shown, reference});
        }
        auto u = b.shown->GetPointData()->GetArray("U");
        if (u && u->GetNumberOfComponents() == 3)
            for (vtkIdType i = 0; i < b.shown->GetNumberOfPoints(); ++i) {
                double p[3];
                b.shown->GetPoint(i, p);
                for (int j = 0; j < 3; ++j)
                    p[j] += d->scale * u->GetComponent(i, j);
                b.shown->GetPoints()->SetPoint(i, p);
            }
        vtkNew<vtkDataSetMapper> mapper;
        mapper->SetInputData(b.shown);
        vtkDataArray* scalar = nullptr;
        const bool cellField =
            d->field == ResultColorField::LayerId || d->field == ResultColorField::MaterialId;
        switch (d->field) {
        case ResultColorField::Temperature:
            scalar = b.shown->GetPointData()->GetArray("T");
            break;
        case ResultColorField::LayerId:
            scalar = b.shown->GetCellData()->GetArray("layer_id");
            break;
        case ResultColorField::MaterialId:
            scalar = b.shown->GetCellData()->GetArray("material_id");
            break;
        case ResultColorField::DisplacementMagnitude:
        case ResultColorField::WarpageUz: {
            if (!u || u->GetNumberOfComponents() != 3)
                break;
            vtkNew<vtkDoubleArray> derived;
            derived->SetName(d->field == ResultColorField::DisplacementMagnitude ? "Result_U_mag"
                                                                                 : "Result_U_z");
            for (vtkIdType i = 0; i < b.shown->GetNumberOfPoints(); ++i) {
                const double x = u->GetComponent(i, 0), y = u->GetComponent(i, 1),
                             z = u->GetComponent(i, 2);
                derived->InsertNextValue(d->field == ResultColorField::DisplacementMagnitude
                                             ? std::sqrt(x * x + y * y + z * z)
                                             : z);
            }
            b.shown->GetPointData()->AddArray(derived);
            scalar = derived;
            break;
        }
        }
        const auto count = cellField ? b.shown->GetNumberOfCells() : b.shown->GetNumberOfPoints();
        if (visible && scalar && b.shown->GetNumberOfCells() && count) {
            for (vtkIdType i = 0; i < count; ++i) {
                const double value = scalar->GetComponent(i, 0);
                if (!d->range.valid) {
                    d->range = {value,
                                value,
                                true,
                                cellField                                   ? ""
                                : d->field == ResultColorField::Temperature ? "K"
                                                                            : "m"};
                } else {
                    d->range.min = std::min(d->range.min, value);
                    d->range.max = std::max(d->range.max, value);
                }
            }
            if (cellField)
                mapper->SetScalarModeToUseCellFieldData();
            else
                mapper->SetScalarModeToUsePointFieldData();
            mapper->SelectColorArray(scalar->GetName());
            mapper->SetLookupTable(lut);
        } else
            mapper->ScalarVisibilityOff();
        b.actor = vtkSmartPointer<vtkActor>::New();
        b.actor->SetMapper(mapper);
        b.actor->GetProperty()->SetColor(0.7, 0.75, 0.8);
        d->renderer->AddActor(b.actor);
        vtkNew<vtkIdList> highlight;
        kinds = b.shown->GetCellData()->GetArray("cell_kind");
        if (kinds)
            for (vtkIdType i = 0; i < b.shown->GetNumberOfCells(); ++i)
                if (kinds->GetComponent(i, 0) == 1)
                    highlight->InsertNextId(i);
        vtkNew<vtkDataSetMapper> hm;
        hm->SetInputData(extract(b.shown, highlight));
        hm->ScalarVisibilityOff();
        vtkNew<vtkActor> ha;
        ha->SetMapper(hm);
        ha->PickableOff();
        ha->GetProperty()->SetRepresentationToWireframe();
        ha->GetProperty()->SetColor(1, 0.7, 0.05);
        ha->GetProperty()->SetLineWidth(2);
        d->renderer->AddActor(ha);
    }
    if (d->loaded && d->error.isEmpty()) {
        const auto start = std::chrono::steady_clock::now();
        d->boundaries = layerBoundaries(shown);
        d->boundarySetupMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (d->boundaries)
            d->renderer->AddActor(d->boundaries);
        if (d->field == ResultColorField::Temperature && !d->range.valid) {
            d->fieldStatusLabel->setText(QStringLiteral("无温度场"));
            d->fieldStatusLabel->show();
        }
    }
    for (auto& b : d->blocks)
        if (b.actor)
            b.actor->GetMapper()->SetScalarRange(d->range.min, d->range.max);
    if (d->range.valid) {
        lut->SetRange(d->range.min, d->range.max);
        vtkNew<vtkScalarBarActor> bar;
        bar->SetLookupTable(lut);
        bar->SetTitle(colorBarTitle().toUtf8().constData());
        d->renderer->AddViewProp(bar);
    }
    emit displayChanged();
    d->renderer->ResetCameraClippingRange();
    if (isVisible())
        d->window->Render();
    update();
}
vtkDataSet* ResultView::displayed(const QString& name) const {
    for (auto& b : d->blocks)
        if (b.name == canonical(name))
            return b.shown;
    return nullptr;
}
QString ResultView::pickedId(const QString& name, vtkIdType cell) const {
    return gid(displayed(name), cell);
}
QString ResultView::pick(int x, int y) {
    vtkNew<vtkCellPicker> picker;
    if (picker->Pick(x, y, 0, d->renderer))
        for (auto& b : d->blocks)
            if (b.actor == picker->GetActor())
                return gid(b.shown, picker->GetCellId());
    return QStringLiteral("未知");
}
vtkRenderWindow* ResultView::renderWindow() const {
    return d->window;
}
const QString& ResultView::error() const {
    return d->error;
}
bool ResultView::capture(const QString& path) {
    // Use a backend selected by the caller; QVTK's generic window requires a live Qt context.
    vtkSmartPointer<vtkRenderWindow> window;
    window.TakeReference(vtkRenderWindow::New());
    window->SetOffScreenRendering(1);
    window->SetSize(800, 600);
    d->window->RemoveRenderer(d->renderer);
    window->AddRenderer(d->renderer);
    window->Render();
    vtkNew<vtkWindowToImageFilter> image;
    image->SetInput(window);
    image->ReadFrontBufferOff();
    image->Update();
    vtkNew<vtkPNGWriter> writer;
    writer->SetFileName(path.toUtf8());
    writer->SetInputConnection(image->GetOutputPort());
    writer->Write();
    window->RemoveRenderer(d->renderer);
    d->window->AddRenderer(d->renderer);
    return !writer->GetErrorCode() && QFileInfo(path).size() > 0;
}
vtkSmartPointer<vtkUnstructuredGrid> ResultView::tetrahedralize(vtkDataSet* input, QString& error) {
    error.clear();
    if (!input) {
        error = "Volume input unavailable";
        return nullptr;
    }
    auto out = vtkSmartPointer<vtkUnstructuredGrid>::New();
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    for (vtkIdType i = 0; i < input->GetNumberOfPoints(); ++i) {
        double p[3];
        input->GetPoint(i, p);
        points->InsertNextPoint(p);
    }
    out->SetPoints(points);
    out->GetPointData()->CopyAllocate(input->GetPointData());
    for (vtkIdType i = 0; i < input->GetNumberOfPoints(); ++i)
        out->GetPointData()->CopyData(input->GetPointData(), i, i);
    out->GetCellData()->CopyAllocate(input->GetCellData());
    auto fail = [&](const QString& why) -> vtkSmartPointer<vtkUnstructuredGrid> {
        error = why;
        return nullptr;
    };
    for (vtkIdType ci = 0; ci < input->GetNumberOfCells(); ++ci) {
        auto cell = input->GetCell(ci);
        if (cell->GetCellDimension() != 3)
            return fail("Quantitative volume requires 3D cells");
        double center[3] = {}, anchor[3];
        input->GetPoint(cell->GetPointId(0), anchor);
        for (vtkIdType i = 0; i < cell->GetNumberOfPoints(); ++i) {
            double p[3];
            input->GetPoint(cell->GetPointId(i), p);
            for (int j = 0; j < 3; ++j)
                center[j] += (p[j] - anchor[j]) / cell->GetNumberOfPoints();
        }
        for (int j = 0; j < 3; ++j)
            center[j] += anchor[j];
        vtkIdType mid = points->InsertNextPoint(center);
        std::vector<double> weights(cell->GetNumberOfPoints(), 1.0 / cell->GetNumberOfPoints());
        out->GetPointData()->InterpolatePoint(
            input->GetPointData(), mid, cell->GetPointIds(), weights.data());
        double bounds[6];
        cell->GetBounds(bounds);
        double length =
            std::max({bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]});
        double tolerance = length * 64 * std::numeric_limits<double>::epsilon();
        double fan = 0, divergence = 0;
        std::map<std::pair<vtkIdType, vtkIdType>, int> edges;
        for (int f = 0; f < cell->GetNumberOfFaces(); ++f) {
            auto face = cell->GetFace(f);
            const auto n = face->GetNumberOfPoints();
            if (n < 3)
                return fail("Degenerate cut face");
            double a[3];
            input->GetPoint(face->GetPointId(0), a);
            // Newell normal accommodates collinear leading edges.
            double normal[3] = {};
            for (vtkIdType k = 1; k + 1 < n; ++k) {
                double b[3], c[3], ab[3], ac[3], cross[3];
                input->GetPoint(face->GetPointId(k), b);
                input->GetPoint(face->GetPointId(k + 1), c);
                vtkMath::Subtract(b, a, ab);
                vtkMath::Subtract(c, a, ac);
                vtkMath::Cross(ab, ac, cross);
                for (int j = 0; j < 3; ++j)
                    normal[j] += cross[j];
            }
            if (vtkMath::Normalize(normal) == 0)
                return fail("Degenerate cut face");
            double delta[3];
            vtkMath::Subtract(a, center, delta);
            bool reverse = vtkMath::Dot(normal, delta) < 0;
            if (reverse)
                for (double& v : normal)
                    v = -v;
            for (vtkIdType k = 0; k < cell->GetNumberOfPoints(); ++k) {
                double p[3];
                input->GetPoint(cell->GetPointId(k), p);
                vtkMath::Subtract(p, a, delta);
                if (vtkMath::Dot(normal, delta) > tolerance)
                    return fail("Non-convex cell: quantitative clip/volume requires constrained "
                                "tetrahedralization");
            }
            for (vtkIdType k = 0; k < n; ++k) {
                double p[3];
                input->GetPoint(face->GetPointId(k), p);
                vtkMath::Subtract(p, a, delta);
                if (std::abs(vtkMath::Dot(normal, delta)) > tolerance)
                    return fail("Non-planar cut face");
                auto u = face->GetPointId(k), v = face->GetPointId((k + 1) % n);
                if (reverse)
                    std::swap(u, v);
                edges[{u, v}]++;
            }
            for (vtkIdType k = 1; k + 1 < n; ++k) {
                vtkIdType ids[4] = {
                    mid, face->GetPointId(0), face->GetPointId(k), face->GetPointId(k + 1)};
                if (reverse)
                    std::swap(ids[2], ids[3]);
                double b[3], c[3];
                input->GetPoint(ids[2], b);
                input->GetPoint(ids[3], c);
                double v = determinant(a, b, c, center) / 6;
                if (v < 0)
                    return fail("Non-convex face triangulation");
                if (v <= 0)
                    continue;
                fan += v;
                divergence += determinant(a, b, c, anchor) / 6;
                auto id = out->InsertNextCell(VTK_TETRA, 4, ids);
                out->GetCellData()->CopyData(input->GetCellData(), ci, id);
            }
        }
        for (auto [edge, count] : edges)
            if (count != 1 || edges[{edge.second, edge.first}] != 1)
                return fail("Cut cell is not a closed manifold");
        if (fan <= 0 || std::abs(fan - divergence) > fan * 1e-8)
            return fail("Cut divergence volume check failed");
    }
    return out;
}
VolumeResult ResultView::quantify(vtkDataSet* input,
                                  const QString& scalar,
                                  bool clip,
                                  std::array<double, 3> origin,
                                  std::array<double, 3> normal) {
    VolumeResult result;
    auto grid = tetrahedralize(input, result.error);
    if (!grid)
        return result;
    if (clip) {
        if (vtkMath::Norm(normal.data()) == 0) {
            result.error = "Clip normal must be nonzero";
            return result;
        }
        grid = clipped(grid, origin, normal);
        // The convex source was already validated and split into a center fan.
        // Clip can emit wedges with collapsed faces when the plane meets a vertex.
        vtkNew<vtkDataSetTriangleFilter> triangles;
        triangles->SetInputData(grid);
        triangles->Update();
        grid = vtkSmartPointer<vtkUnstructuredGrid>::New();
        grid->DeepCopy(triangles->GetOutput());
    }
    auto point = grid->GetPointData()->GetArray(scalar.toUtf8());
    auto cellData = grid->GetCellData()->GetArray(scalar.toUtf8());
    if (!scalar.isEmpty() && !point && !cellData) {
        result.error = "Integral field unavailable";
        return result;
    }
    for (vtkIdType i = 0; i < grid->GetNumberOfCells(); ++i) {
        auto cell = grid->GetCell(i);
        if (cell->GetCellType() != VTK_TETRA) {
            result.error = "Clip produced a non-tetrahedral volume";
            return result;
        }
        double p[4][3];
        for (int j = 0; j < 4; ++j)
            grid->GetPoint(cell->GetPointId(j), p[j]);
        double volume = std::abs(determinant(p[1], p[2], p[3], p[0])) / 6;
        result.volume += volume;
        double value = 0;
        if (point)
            for (int j = 0; j < 4; ++j)
                value += point->GetComponent(cell->GetPointId(j), 0) / 4;
        else if (cellData)
            value = cellData->GetComponent(i, 0);
        result.integral += volume * value;
    }
    return result;
}
} // namespace chipcae::desktop
