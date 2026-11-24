#include "ResultControls.hpp"
#include "ResultView.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QProcess>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCamera.h>
#include <vtkCell.h>
#include <vtkCellData.h>
#include <vtkCompositeDataIterator.h>
#include <vtkCompositeDataSet.h>
#include <vtkCoordinate.h>
#include <vtkDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkDoubleArray.h>
#include <vtkExtractCells.h>
#include <vtkHDFWriter.h>
#include <vtkIdList.h>
#include <vtkIntArray.h>
#include <vtkNew.h>
#include <vtkPartitionedDataSet.h>
#include <vtkPartitionedDataSetCollection.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPropCollection.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkScalarBarActor.h>
#include <vtkScalarsToColors.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkUnstructuredGrid.h>
using namespace chipcae::desktop;
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
void near(double a, double b, double tolerance = 1e-10) {
    require(std::abs(a - b) <= tolerance, "numeric mismatch");
}
vtkSmartPointer<vtkUnstructuredGrid> sheet() {
    auto g = vtkSmartPointer<vtkUnstructuredGrid>::New();
    vtkNew<vtkPoints> p;
    p->SetDataTypeToDouble();
    for (auto xyz : {std::array<double, 3>{0, 0, 0},
                     {2, 0, 0},
                     {2, 3, 0},
                     {0, 3, 0},
                     {0, 0, 1e-7},
                     {2, 0, 1e-7},
                     {2, 3, 1e-7},
                     {0, 3, 1e-7}})
        p->InsertNextPoint(xyz.data());
    g->SetPoints(p);
    // Explicit outward faces, exercising the polyhedron path, rather than only hex triangulation.
    vtkIdType ids[] = {0, 1, 2, 3, 4, 5, 6, 7};
    vtkIdType faces[] = {4, 0, 3, 2, 1, 4, 4, 5, 6, 7, 4, 0, 1, 5, 4,
                         4, 1, 2, 6, 5, 4, 2, 3, 7, 6, 4, 3, 0, 4, 7};
    g->InsertNextCell(VTK_POLYHEDRON, 8, ids, 6, faces);
    vtkNew<vtkDoubleArray> t;
    t->SetName("T");
    vtkNew<vtkDoubleArray> u;
    u->SetName("U");
    u->SetNumberOfComponents(3);
    for (int i = 0; i < 8; ++i) {
        double x[3];
        p->GetPoint(i, x);
        t->InsertNextValue(300 + x[0]);
        u->InsertNextTuple3(x[0], 2, 3);
    }
    g->GetPointData()->AddArray(t);
    g->GetPointData()->AddArray(u);
    vtkNew<vtkDoubleArray> kind;
    kind->SetName("cell_kind");
    kind->InsertNextValue(1);
    g->GetCellData()->AddArray(kind);
    return g;
}
using Coordinates = std::vector<std::vector<std::array<double, 3>>>;
const QStringList displayedBlocks = {"hex", "cut", "eb_surface", "particles"};
Coordinates coordinates(ResultView& view) {
    Coordinates out;
    for (const auto& name : displayedBlocks) {
        out.emplace_back();
        if (auto block = view.displayed(name))
            for (vtkIdType i = 0; i < block->GetNumberOfPoints(); ++i) {
                std::array<double, 3> point;
                block->GetPoint(i, point.data());
                out.back().push_back(point);
            }
    }
    return out;
}
vtkCamera* camera(ResultView& view) {
    return view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActiveCamera();
}
std::array<double, 3> cameraDirection(ResultView& view) {
    const auto position = camera(view)->GetPosition();
    const auto focal = camera(view)->GetFocalPoint();
    std::array<double, 3> direction;
    double norm = 0;
    for (int axis = 0; axis < 3; ++axis) {
        direction[axis] = focal[axis] - position[axis];
        norm += direction[axis] * direction[axis];
    }
    norm = std::sqrt(norm);
    require(std::isfinite(norm) && norm > 0, "camera direction must be finite and nonzero");
    for (auto& value : direction) {
        value /= norm;
        require(std::isfinite(value), "normalized camera direction must be finite");
    }
    near(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2],
         1,
         1e-12);
    return direction;
}
void checkCameraDirection(ResultView& view, const std::array<double, 3>& expected) {
    const auto actual = cameraDirection(view);
    for (int axis = 0; axis < 3; ++axis)
        require(std::abs(actual[axis] - expected[axis]) <= 1e-12,
                "rebuild/reload changed camera direction");
}
void checkOblique(ResultView& view) {
    const auto direction = cameraDirection(view);
    constexpr double cos20 = 0.9396926207859084;
    for (auto value : direction)
        require(std::abs(value) <= cos20, "default camera is almost aligned with a world axis");
    double bounds[6] = {std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity()};
    auto hex = view.displayed("hex");
    bool found = false;
    for (const auto& name : displayedBlocks) {
        auto block = view.displayed(name);
        if (!block || !block->GetNumberOfCells() ||
            (hex && hex->GetNumberOfCells() && name != "hex"))
            continue;
        double blockBounds[6];
        block->GetBounds(blockBounds);
        for (int axis = 0; axis < 3; ++axis) {
            bounds[2 * axis] = std::min(bounds[2 * axis], blockBounds[2 * axis]);
            bounds[2 * axis + 1] = std::max(bounds[2 * axis + 1], blockBounds[2 * axis + 1]);
        }
        found = true;
    }
    require(found, "oblique assertion needs visible geometry");
    const std::array lengths = {
        bounds[1] - bounds[0], bounds[3] - bounds[2], bounds[5] - bounds[4]};
    for (int axis = 0; axis < 3; ++axis) {
        const double a = lengths[axis], b = lengths[(axis + 1) % 3], c = lengths[(axis + 2) % 3];
        if (a < b && a < c && b - a >= 1e-6 * b && c - a >= 1e-6 * c)
            require(std::abs(direction[axis]) <= cos20,
                    "default camera is almost aligned with the strict shortest bounding axis");
    }
}
vtkActor* boundaryActor(ResultView& view) {
    vtkActor* found = nullptr;
    auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
    actors->InitTraversal();
    while (auto actor = actors->GetNextActor())
        if (actor->GetObjectName() == "layerBoundaries") {
            require(!found, "multiple layerBoundaries actors");
            found = actor;
        }
    return found;
}
vtkDataSet* boundaryInput(ResultView& view) {
    auto actor = boundaryActor(view);
    if (!actor)
        return nullptr;
    require(actor->GetMapper(), "layer boundary mapper missing");
    actor->GetMapper()->Update();
    return vtkDataSet::SafeDownCast(actor->GetMapper()->GetInputDataObject(0, 0));
}
void checkNoBoundaries(ResultView& view) {
    auto input = boundaryInput(view);
    require(!input || input->GetNumberOfCells() == 0, "unexpected layer interface edges");
}
// QLabel::isVisible() needs a visible ancestor. Hide QVTK first so showing the Qt
// status row does not initialize an embedded OpenGL context on offscreen runners.
class VisibleStatusView {
public:
    explicit VisibleStatusView(ResultView& view) : view(view) {
        auto vtk = view.findChild<QVTKOpenGLNativeWidget*>();
        require(vtk, "QVTK child missing");
        vtk->hide();
        view.resize(1000, 200);
        view.show();
        view.layout()->activate();
    }
    ~VisibleStatusView() { view.hide(); }

private:
    ResultView& view;
};
void checkFieldStatus(ResultView& view, bool missingTemperature) {
    VisibleStatusView visible(view);
    auto label = view.findChild<QLabel*>("fieldStatusLabel");
    if (missingTemperature) {
        require(label, "missing fieldStatusLabel for absent PointData T");
        require(label->text() == QStringLiteral("无温度场"), "missing-temperature status text");
        require(label->isVisible(), "missing-temperature status is not visible");
    } else {
        require(!label || (label->text().isEmpty() && !label->isVisible()),
                "unexpected temperature status on empty, valid, or non-temperature view");
    }
}
void checkTemperatureControls(ResultControls& controls) {
    bool found = false;
    for (auto label : controls.findChildren<QLabel*>()) {
        found |= label->text().contains(QStringLiteral("无温度场"));
        require(!label->text().contains("Field: unavailable"),
                "temperature controls retain English unavailable message");
    }
    require(found, "temperature controls do not explain absent PointData T");
}
void checkScalarBar(ResultView& view) {
    auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
    int count = 0;
    props->InitTraversal();
    while (auto prop = props->GetNextProp()) {
        auto bar = vtkScalarBarActor::SafeDownCast(prop);
        if (!bar)
            continue;
        ++count;
        require(view.colorRange().valid, "unavailable field has scalar bar");
        require(QString::fromUtf8(bar->GetTitle()) == view.colorBarTitle(), "scalar bar title");
        auto lut = bar->GetLookupTable();
        require(lut && lut->GetRange()[0] == view.colorRange().min &&
                    lut->GetRange()[1] == view.colorRange().max,
                "scalar bar LUT range");
    }
    require(count == (view.colorRange().valid ? 1 : 0), "scalar bar presence");
}
void checkLoadedResultScene(ResultView& view) {
    auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
    int blockActors = 0;
    for (const auto& name : displayedBlocks) {
        auto block = view.displayed(name);
        if (!block || !block->GetNumberOfCells())
            continue;
        actors->InitTraversal();
        while (auto actor = actors->GetNextActor()) {
            auto mapper = vtkDataSetMapper::SafeDownCast(actor->GetMapper());
            if (mapper && mapper->GetInput() == block)
                ++blockActors;
        }
    }
    require(blockActors > 0, "failed reload fixture needs displayed cells and result block actors");
    require(boundaryActor(view), "failed reload fixture needs a layer boundary actor");
}
void checkClearedResultScene(ResultView& view) {
    auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
    require(actors->GetNumberOfItems() == 0,
            qPrintable(QString("failed reload retained %1 stale result/highlight actors")
                           .arg(actors->GetNumberOfItems())));
    require(!boundaryActor(view), "failed reload retained layer boundary actor");
    for (const auto& name : displayedBlocks)
        require(!view.displayed(name), "failed reload retained displayed block data");
    const auto range = view.colorRange();
    require(!range.valid, "failed reload retained a valid color range");
    near(range.min, 0);
    near(range.max, 0);
    require(range.unit.isEmpty(), "failed reload retained color range unit");
    require(view.colorBarTitle().isEmpty(), "failed reload retained color bar title");
    checkScalarBar(view);
    checkFieldStatus(view, false);
}
void checkRange(ResultView& view, ResultColorField field, const QStringList& hidden = {}) {
    checkScalarBar(view);
    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    for (const auto& name : displayedBlocks) {
        auto block = view.displayed(name);
        if (hidden.contains(name)) {
            require(!block || !block->GetNumberOfCells(), "hidden block still displayed");
            continue;
        }
        if (!block || !block->GetNumberOfCells())
            continue;
        auto array =
            block->GetPointData()->GetArray(field == ResultColorField::Temperature ? "T" : "U");
        require(array, "fixture missing selected point array");
        vtkDataSetMapper* mapper = nullptr;
        auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
        actors->InitTraversal();
        while (auto actor = actors->GetNextActor()) {
            auto candidate = vtkDataSetMapper::SafeDownCast(actor->GetMapper());
            if (candidate && candidate->GetInput() == block)
                mapper = candidate;
        }
        require(mapper && mapper->GetScalarVisibility(), "selected scalar mapper missing");
        require(mapper->GetScalarRange()[0] == view.colorRange().min &&
                    mapper->GetScalarRange()[1] == view.colorRange().max,
                "mapper range differs from color range");
        require(mapper->GetScalarMode() == VTK_SCALAR_MODE_USE_POINT_FIELD_DATA &&
                    mapper->GetArrayAccessMode() == VTK_GET_ARRAY_BY_NAME,
                "mapper does not select point field by name");
        const char* expected = field == ResultColorField::Temperature             ? "T"
                               : field == ResultColorField::DisplacementMagnitude ? "Result_U_mag"
                                                                                  : "Result_U_z";
        require(mapper->GetArrayName() && QString(mapper->GetArrayName()) == expected,
                "mapper selected wrong field array");
        auto selected = block->GetPointData()->GetArray(mapper->GetArrayName());
        require(selected && selected->GetNumberOfComponents() == 1 &&
                    (mapper->GetArrayComponent() == 0 || mapper->GetArrayComponent() == -1) &&
                    selected->GetNumberOfTuples() == block->GetNumberOfPoints(),
                "mapper selected wrong scalar component or tuple count");
        for (vtkIdType i = 0; i < block->GetNumberOfPoints(); ++i) {
            double value = array->GetComponent(i, 0);
            if (field == ResultColorField::WarpageUz)
                value = array->GetComponent(i, 2);
            if (field == ResultColorField::DisplacementMagnitude) {
                double u[3];
                array->GetTuple(i, u);
                value = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            }
            near(selected->GetComponent(i, 0), value);
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    require(std::isfinite(low), "fixture has no displayed points");
    const auto range = view.colorRange();
    require(range.valid, "selected field range unavailable");
    require(range.min == low && range.max == high, "displayed point range differs");
    require(range.unit == (field == ResultColorField::Temperature ? "K" : "m"), "range unit");
}
void checkUnavailable(ResultView& view, bool loadedMissingTemperature = false) {
    checkScalarBar(view);
    const auto range = view.colorRange();
    require(!range.valid && range.min == 0 && range.max == 0 && range.unit.isEmpty(),
            "invalid range defaults");
    require(view.colorBarTitle().isEmpty(), "unavailable field title");
    require(view.tmaxText() == "T_max: unavailable", "unavailable official T_max");
    require(view.error().isEmpty(), "unavailable field set error");
    checkFieldStatus(view, loadedMissingTemperature);
}
const char* idArrayName(ResultColorField field) {
    require(field == ResultColorField::LayerId || field == ResultColorField::MaterialId,
            "expected ID color field");
    return field == ResultColorField::LayerId ? "layer_id" : "material_id";
}
void checkIdRange(ResultView& view, ResultSession& session, ResultColorField field) {
    require(view.colorField() == field, "ID field selection ignored");
    const auto name = idArrayName(field);
    int low = std::numeric_limits<int>::max();
    int high = std::numeric_limits<int>::min();
    bool nonempty = false;
    const auto range = view.colorRange();
    for (const auto& blockName : displayedBlocks) {
        auto block = view.displayed(blockName);
        if (!block || !block->GetNumberOfCells())
            continue;
        nonempty = true;
        auto ids = block->GetCellData()->GetArray(name);
        require(ids && QString::fromUtf8(ids->GetName()) == name,
                "displayed ID cell array missing or incorrectly named");
        require(ids->GetNumberOfComponents() == 1 &&
                    ids->GetNumberOfTuples() == block->GetNumberOfCells(),
                "ID array component or tuple count");
        vtkDataSetMapper* mapper = nullptr;
        auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
        actors->InitTraversal();
        while (auto actor = actors->GetNextActor()) {
            auto candidate = vtkDataSetMapper::SafeDownCast(actor->GetMapper());
            if (candidate && candidate->GetInput() == block)
                mapper = candidate;
        }
        require(mapper && mapper->GetScalarVisibility(), "ID scalar mapper missing");
        require(mapper->GetScalarMode() == VTK_SCALAR_MODE_USE_CELL_FIELD_DATA &&
                    mapper->GetArrayAccessMode() == VTK_GET_ARRAY_BY_NAME,
                "ID mapper must select cell field by name");
        require(mapper->GetArrayName() && QString::fromUtf8(mapper->GetArrayName()) == name,
                "ID mapper selected wrong array");
        require(mapper->GetScalarRange()[0] == range.min &&
                    mapper->GetScalarRange()[1] == range.max,
                "ID mapper range differs from color range");
        auto lut = mapper->GetLookupTable();
        require(lut && lut->GetRange()[0] == range.min && lut->GetRange()[1] == range.max,
                "ID mapper LUT range differs from color range");
        for (vtkIdType cell = 0; cell < block->GetNumberOfCells(); ++cell) {
            const double value = ids->GetComponent(cell, 0);
            const int id = static_cast<int>(value);
            require(value == static_cast<double>(id), "fixture ID is not integral");
            low = std::min(low, id);
            high = std::max(high, id);
        }
    }
    require(nonempty, "ID fixture has no displayed cells");
    require(range.valid, "ID color range unavailable");
    require(range.unit.isEmpty(), "ID color range must have no unit");
    // IDs are integers; the acceptance contract requires exact double equality.
    require(range.min == static_cast<double>(low) && range.max == static_cast<double>(high),
            "ID range differs from visible cell IDs");
    QStringList labels;
    for (int id = low;; ++id) {
        auto label =
            field == ResultColorField::LayerId ? session.layerName(id) : session.materialName(id);
        if (label.isEmpty())
            label = QString("%1 %2")
                        .arg(field == ResultColorField::LayerId ? "layer" : "material")
                        .arg(id);
        labels << label;
        if (id == high)
            break;
    }
    const auto expected =
        (field == ResultColorField::LayerId ? QStringLiteral("叠层: ") : QStringLiteral("材料: ")) +
        labels.join(", ");
    require(view.colorBarTitle() == expected,
            qPrintable("ID title must list every ID in ascending order: expected [" + expected +
                       "] got [" + view.colorBarTitle() + "]"));
    checkScalarBar(view);
}
void checkIdBounds(ResultView& view, double low, double high) {
    const auto range = view.colorRange();
    require(range.valid && range.unit.isEmpty() && range.min == low && range.max == high,
            "ID range differs from independent fixture bounds");
}
void checkIdUnavailable(ResultView& view, ResultColorField field) {
    require(view.colorField() == field, "unavailable ID field selection ignored");
    require(!view.colorRange().valid, "unavailable ID retained a color range");
    require(view.colorBarTitle().isEmpty(), "unavailable ID retained a title");
    require(view.error().isEmpty(), "unavailable ID set an error");
    checkScalarBar(view);
    auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
    actors->InitTraversal();
    while (auto actor = actors->GetNextActor())
        require(!actor->GetMapper()->GetScalarVisibility(), "unavailable ID scalar coloring");
}
void addMaterialIds(vtkUnstructuredGrid* grid) {
    auto layers = grid->GetCellData()->GetArray("layer_id");
    require(layers, "material fixture requires layer IDs");
    vtkNew<vtkIntArray> materials;
    materials->SetName("material_id");
    for (vtkIdType cell = 0; cell < grid->GetNumberOfCells(); ++cell) {
        const int layer = static_cast<int>(layers->GetComponent(cell, 0));
        require(layer >= 1 && layer <= 3, "unexpected synthetic layer ID");
        materials->InsertNextValue(10 * layer);
    }
    grid->GetCellData()->AddArray(materials);
}
void checkSummaryLabel(ResultControls& controls, ResultView& view) {
    auto range = view.colorRange();
    bool found = false;
    for (auto label : controls.findChildren<QLabel*>()) {
        const auto text = label->text();
        found |= text.contains(view.tmaxText()) && text.contains("min=") && text.contains("max=") &&
                 text.contains(QString::number(range.min, 'g', 16)) &&
                 text.contains(QString::number(range.max, 'g', 16)) &&
                 text.contains(view.summary("warpage_pv")) &&
                 text.contains(view.summary("U_mag_max"));
    }
    require(found, "controls summary missing official T_max, range or existing QoIs");
}
// Read through the production session and write a temporary VTKHDF fixture. Mutations
// use VTK arrays and cell connectivity rather than HDF paths or point ordering.
template <typename Mutate>
void writeFixture(const QDir& root,
                  const QString& path,
                  Mutate mutate,
                  bool partitionCells = false) {
    ResultSession source;
    require(source.open(root.filePath("viewer_mixed")), qPrintable(source.error()));
    auto data = source.loadStep(0);
    require(data != nullptr, qPrintable(source.error()));
    auto composite = vtkCompositeDataSet::SafeDownCast(data);
    require(composite != nullptr, "expected composite synthetic fixture");
    auto iterator = vtkSmartPointer<vtkCompositeDataIterator>::Take(composite->NewIterator());
    int grids = 0;
    for (iterator->InitTraversal(); !iterator->IsDoneWithTraversal(); iterator->GoToNextItem()) {
        if (auto grid = vtkUnstructuredGrid::SafeDownCast(iterator->GetCurrentDataObject())) {
            mutate(grid);
            ++grids;
        }
    }
    require(grids > 0, "synthetic fixture has no grids");
    if (partitionCells) {
        auto pdc = vtkPartitionedDataSetCollection::SafeDownCast(data);
        require(pdc && pdc->GetNumberOfPartitionedDataSets() == 1,
                "partition fixture requires one volume block");
        auto pd = pdc->GetPartitionedDataSet(0);
        auto grid = vtkUnstructuredGrid::SafeDownCast(pd->GetPartition(0));
        require(grid && grid->GetNumberOfCells() == 2, "partition fixture requires two cells");
        std::array<vtkSmartPointer<vtkUnstructuredGrid>, 2> partitions;
        for (vtkIdType cell = 0; cell < 2; ++cell) {
            vtkNew<vtkIdList> ids;
            ids->InsertNextId(cell);
            vtkNew<vtkExtractCells> extract;
            extract->SetInputData(grid);
            extract->SetCellList(ids);
            extract->Update();
            partitions[cell] = vtkSmartPointer<vtkUnstructuredGrid>::New();
            partitions[cell]->DeepCopy(extract->GetOutput());
        }
        pd->SetNumberOfPartitions(2);
        for (unsigned cell = 0; cell < 2; ++cell)
            pd->SetPartition(cell, partitions[cell]);
    }
    vtkNew<vtkHDFWriter> writer;
    writer->SetFileName(path.toUtf8().constData());
    writer->SetInputData(data);
    require(writer->Write() == 1, "write temporary VTKHDF fixture");
}
// Synthetic summary packages keep method tests independent of solver output and frozen fixtures.
void writeSummaryFixture(
    const QDir& root,
    const QTemporaryDir& temp,
    const QJsonArray& summary,
    const std::function<void(vtkUnstructuredGrid*)>& mutate = [](vtkUnstructuredGrid*) {}) {
    require(temp.isValid(), "summary fixture directory");
    writeFixture(root, temp.filePath("step.vtkhdf"), mutate);
    const QJsonObject manifest{
        {"schema_version", "chipcae.results/1.0"},
        {"fields", QJsonArray{}},
        {"steps", QJsonArray{QJsonObject{{"n", 0}, {"time", 0}, {"file", "step.vtkhdf"}}}},
        {"summary", summary}};
    QFile file(temp.filePath("manifest.json"));
    require(file.open(QIODevice::WriteOnly), "summary fixture manifest open");
    const auto bytes = QJsonDocument(manifest).toJson();
    require(file.write(bytes) == bytes.size(), "summary fixture manifest write");
}
// D14g-1 compares metadata-only variants against identical geometry without methods.
// Equality is deliberately exact, as required by this viewer presentation contract.
bool exactRange(const ColorRange& a, const ColorRange& b) {
    return a.valid == b.valid && a.min == b.min && a.max == b.max && a.unit == b.unit;
}
bool exactCoordinates(const Coordinates& a, const Coordinates& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t block = 0; block < a.size(); ++block) {
        if (a[block].size() != b[block].size())
            return false;
        for (std::size_t point = 0; point < a[block].size(); ++point)
            for (int axis = 0; axis < 3; ++axis)
                if (a[block][point][axis] != b[block][point][axis])
                    return false;
    }
    return true;
}
std::vector<vtkTextActor*>
matchingTextActors(ResultView& view, const QString& input, bool visibleOnly = true) {
    std::vector<vtkTextActor*> matches;
    auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
    props->InitTraversal();
    while (auto prop = props->GetNextProp())
        if (auto text = vtkTextActor::SafeDownCast(prop))
            if (QString::fromUtf8(text->GetInput()) == input &&
                (!visibleOnly || text->GetVisibility()))
                matches.push_back(text);
    return matches;
}
int scalarBarCount(ResultView& view) {
    int count = 0;
    auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
    props->InitTraversal();
    while (auto prop = props->GetNextProp())
        if (vtkScalarBarActor::SafeDownCast(prop))
            ++count;
    return count;
}
bool geometryScalarsOff(ResultView& view) {
    int count = 0;
    auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
    actors->InitTraversal();
    while (auto actor = actors->GetNextActor()) {
        auto mapper = vtkDataSetMapper::SafeDownCast(actor->GetMapper());
        if (!mapper || actor->GetObjectName() == "layerBoundaries")
            continue;
        mapper->Update();
        auto input = mapper->GetInput();
        if (input && input->GetNumberOfCells()) {
            ++count;
            if (mapper->GetScalarVisibility())
                return false;
        }
    }
    return count > 0;
}
bool exactStatus(ResultView& view, const QString& expected) {
    auto label = view.findChild<QLabel*>("fieldStatusLabel");
    return label && label->text() == expected && label->isHidden() == expected.isEmpty();
}
int fvmNoticeAcceptance(const QDir& root) {
    int failures = 0;
    QString context;
    auto run = [&](const QString& name, auto test) {
        const auto before = failures;
        context.clear();
        auto check = [&](bool ok, const char* message) {
            if (!ok) {
                ++failures;
                std::cerr << "FAIL D14g-1 " << name.toStdString() << " [" << context.toStdString()
                          << "]: " << message << '\n';
            }
        };
        try {
            test(check);
        } catch (const std::exception& e) {
            check(false, e.what());
        }
        if (failures == before)
            std::cout << "PASS D14g-1 " << name.toStdString() << '\n';
    };
    const QString notice = "FVM elastic: accuracy unverified - demo only";
    const QString fvmStatus = QStringLiteral("FVM 弹性结果：精度未验证，仅供演示");
    const QString unsolvedStatus = QStringLiteral("力学未求解：不显示位移／翘曲");
    const std::array fields = {ResultColorField::Temperature,
                               ResultColorField::DisplacementMagnitude,
                               ResultColorField::WarpageUz,
                               ResultColorField::LayerId,
                               ResultColorField::MaterialId};
    // The three entries are copied literally from the frozen manifest_fvm sample.
    const QJsonArray enums{QJsonObject{{"step", 0},
                                       {"name", "warpage_pv"},
                                       {"value", 1.25e-06},
                                       {"unit", "m"},
                                       {"method", "fvm_cell_centers_lsq"}},
                           QJsonObject{{"step", 0},
                                       {"name", "T_max"},
                                       {"value", 370.0},
                                       {"unit", "K"},
                                       {"method", "fvm_cell_centers"}},
                           QJsonObject{{"step", 0},
                                       {"name", "U_mag_max"},
                                       {"value", 2.5e-06},
                                       {"unit", "m"},
                                       {"method", "fvm_cell_centers"}}};
    auto withoutMethods = enums;
    auto sentences = enums;
    for (qsizetype i = 0; i < enums.size(); ++i) {
        auto entry = enums[i].toObject();
        entry.remove("method");
        withoutMethods[i] = entry;
        entry["method"] =
            i == 0 ? "FVM top-face centroids; least-squares plane removal" : "FVM cell centers";
        sentences[i] = entry;
    }
    auto mechanical = [&](const QString& name, const QJsonValue& method) {
        auto entry = enums[name == "warpage_pv" ? 0 : 2].toObject();
        entry["method"] = method;
        return entry;
    };
    QTemporaryDir baselineTemp;
    ResultSession baselineSession;
    ResultView baseline;
    std::array<ColorRange, 5> baselineRanges;
    std::array<Coordinates, 5> baselineDeformed;
    Coordinates undeformed;
    run("fixture has nonzero Uz and scale moves baseline", [&](auto check) {
        writeSummaryFixture(root, baselineTemp, withoutMethods);
        require(baselineSession.open(baselineTemp.path()), qPrintable(baselineSession.error()));
        require(baseline.loadStep(baselineSession, 0), qPrintable(baseline.error()));
        baseline.setWarpageScale(0);
        undeformed = coordinates(baseline);
        bool nonzeroZ = false;
        for (const auto& name : displayedBlocks)
            if (auto block = baseline.displayed(name))
                if (auto u = block->GetPointData()->GetArray("U"))
                    for (vtkIdType i = 0; i < u->GetNumberOfTuples(); ++i)
                        nonzeroZ |= u->GetComponent(i, 2) != 0;
        check(nonzeroZ, "fixture must have nonzero U_z to detect suppressed deformation");
        baseline.setWarpageScale(1000);
        check(!exactCoordinates(undeformed, coordinates(baseline)),
              "baseline scale 0 and 1000 must produce different coordinates");
        for (std::size_t i = 0; i < fields.size(); ++i) {
            baseline.setColorField(fields[i]);
            baselineRanges[i] = baseline.colorRange();
            baselineDeformed[i] = coordinates(baseline);
        }
        check(baselineRanges[0].valid, "baseline temperature must be available");
    });
    if (failures)
        return failures; // Fixture errors must not masquerade as missing-feature failures.
    auto withPackage = [&](const QJsonArray& summary,
                           auto test,
                           bool missingT = false,
                           bool absentSummary = false) {
        QTemporaryDir temp;
        writeSummaryFixture(root, temp, summary, [&](vtkUnstructuredGrid* grid) {
            if (missingT)
                grid->GetPointData()->RemoveArray("T");
        });
        if (absentSummary) {
            QFile file(temp.filePath("manifest.json"));
            require(file.open(QIODevice::ReadOnly), "read absent-summary manifest");
            auto manifest = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            manifest.remove("summary");
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                    "rewrite absent-summary manifest");
            const auto bytes = QJsonDocument(manifest).toJson();
            require(file.write(bytes) == bytes.size(), "write absent-summary manifest");
        }
        ResultSession session;
        require(session.open(temp.path()), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        test(view, session);
    };
    auto noNotice = [&](ResultView& view, auto check) {
        check(matchingTextActors(view, notice, false).empty(), "notice actor must be absent");
        auto label = view.findChild<QLabel*>("fieldStatusLabel");
        check(label && !label->text().contains(fvmStatus) &&
                  !label->text().contains(unsolvedStatus),
              "unexpected new status text");
    };
    auto checkFvm = [&](ResultView& view, auto check) {
        const auto variant = context;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            context = variant + " field=" + QString::number(static_cast<int>(fields[i]));
            view.setColorField(fields[i]);
            view.setWarpageScale(0);
            check(exactCoordinates(coordinates(view), undeformed), "scale-zero geometry changed");
            view.setWarpageScale(1000);
            check(exactRange(view.colorRange(), baselineRanges[i]), "FVM range or unit changed");
            check(exactCoordinates(coordinates(view), baselineDeformed[i]),
                  "FVM deformed geometry changed");
            check(exactStatus(view, fvmStatus), "missing exact visible FVM status");
            check(matchingTextActors(view, notice).size() == 1,
                  "expected exactly one visible FVM notice actor");
        }
    };
    auto checkOrdinary = [&](ResultView& view, auto check) {
        const auto variant = context;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            context = variant + " field=" + QString::number(static_cast<int>(fields[i]));
            view.setColorField(fields[i]);
            view.setWarpageScale(1000);
            check(exactRange(view.colorRange(), baselineRanges[i]), "ordinary range changed");
            check(exactCoordinates(coordinates(view), baselineDeformed[i]),
                  "ordinary deformed geometry changed");
            check(exactStatus(view, {}), "ordinary status changed");
            noNotice(view, check);
        }
    };
    auto checkUnsolved = [&](ResultView& view, auto check) {
        const auto variant = context;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            context = variant + " field=" + QString::number(static_cast<int>(fields[i]));
            view.setColorField(fields[i]);
            for (double scale : {0.0, 1000.0}) {
                view.setWarpageScale(scale);
                check(exactCoordinates(coordinates(view), undeformed),
                      "unsolved mechanics must leave every block undeformed");
            }
            check(matchingTextActors(view, notice, false).empty(),
                  "unsolved mechanics must not show FVM notice");
            if (fields[i] == ResultColorField::DisplacementMagnitude ||
                fields[i] == ResultColorField::WarpageUz) {
                check(!view.colorRange().valid, "unsolved mechanical range must be invalid");
                check(view.colorBarTitle().isEmpty(), "unsolved mechanical title must be empty");
                check(scalarBarCount(view) == 0, "unsolved mechanics must omit scalar bar");
                check(geometryScalarsOff(view), "unsolved geometry mapper scalars must be off");
                check(exactStatus(view, unsolvedStatus), "missing exact visible unsolved status");
            } else {
                check(exactRange(view.colorRange(), baselineRanges[i]),
                      "unsolved mechanics changed temperature or ID coloring");
                check(exactStatus(view, {}), "unexpected non-mechanical unsolved status");
                if (fields[i] == ResultColorField::Temperature)
                    check(view.colorRange().valid, "unsolved mechanics lost temperature");
            }
        }
    };
    run("fvm elastic enum shows unverified notice", [&](auto check) {
        withPackage(enums, [&](auto& view, auto&) {
            checkFvm(view, check);
            check(view.summary("warpage_pv") == "warpage_pv: 1.25e-06 m [fvm_cell_centers_lsq]",
                  "enum summary changed");
            check(view.tmaxText() == "T_max: 370 K [fvm_cell_centers]", "enum Tmax changed");
        });
    });
    run("fvm elastic long sentences show unverified notice", [&](auto check) {
        withPackage(sentences, [&](auto& view, auto&) {
            checkFvm(view, check);
            check(
                view.summary("warpage_pv") ==
                    "warpage_pv: 1.25e-06 m [FVM top-face centroids; least-squares plane removal]",
                "long warpage summary changed");
            check(view.summary("U_mag_max") == "U_mag_max: 2.5e-06 m [FVM cell centers]",
                  "long magnitude summary changed");
            check(view.tmaxText() == "T_max: 370 K [FVM cell centers]", "long Tmax changed");
        });
    });
    run("single fvm mechanical entry is enough", [&](auto check) {
        for (const auto& entry : {enums.at(0), sentences.at(2)})
            withPackage({entry}, [&](auto& view, auto&) { checkFvm(view, check); });
    });
    run("not solved hides displacement and warpage", [&](auto check) {
        for (const auto& method : {"not_solved", "not solved: fvm thermal-only stage"})
            for (int names : {1, 2, 3}) {
                context = QString(method) + " names=" + QString::number(names);
                QJsonArray summary;
                if (names & 1)
                    summary.append(mechanical("warpage_pv", method));
                if (names & 2)
                    summary.append(mechanical("U_mag_max", method));
                withPackage(summary, [&](auto& view, auto&) { checkUnsolved(view, check); });
            }
    });
    run("not solved wins over fvm method", [&](auto check) {
        for (const auto& method : {"not_solved", "not solved: fvm thermal-only stage"})
            for (bool reversed : {false, true}) {
                context = QString(method) + (reversed ? " U unsolved" : " warpage unsolved");
                withPackage({mechanical("warpage_pv", reversed ? "fvm_cell_centers_lsq" : method),
                             mechanical("U_mag_max", reversed ? method : "fvm_cell_centers")},
                            [&](auto& view, auto&) { checkUnsolved(view, check); });
            }
    });
    run("non fvm methods unchanged", [&](auto check) {
        const std::vector<QJsonValue> methods{
            QJsonValue(QJsonValue::Undefined),
            "",
            QJsonValue(QJsonValue::Null),
            1,
            true,
            QJsonArray{"not_solved"},
            QJsonObject{{"method", "fvm_cell_centers"}},
            "fem_q2_nodes",
            "mechanical_disabled",
            "mechanical physics disabled; zero displacement",
            "FVM_CELL_CENTERS_LSQ",
            "FVM_CELL_CENTERS",
            "Not_Solved",
            "not_SOLVED",
            " fvm_cell_centers_lsq",
            "fvm_cell_centers_lsq ",
            " fvm_cell_centers",
            "fvm_cell_centers ",
            " not_solved",
            "not_solved ",
            "prefix_fvm_cell_centers_lsq",
            "fvm_cell_centers_lsq_suffix",
            "prefix_fvm_cell_centers",
            "fvm_cell_centers_suffix",
            "prefix_not_solved",
            "not_solved_suffix",
            " FVM cell centers ",
            "FVM top-face centroids; least-squares plane removal suffix",
            "not solved: fvm thermal-only stage suffix"};
        for (std::size_t i = 0; i < methods.size(); ++i) {
            context = "method variant=" + QString::number(i);
            withPackage({mechanical("warpage_pv", methods[i]), mechanical("U_mag_max", methods[i])},
                        [&](auto& view, auto&) { checkOrdinary(view, check); });
        }
        for (const auto& entry : {enums.at(1), sentences.at(1)}) {
            context = "only thermal FVM";
            withPackage({entry}, [&](auto& view, auto&) { checkOrdinary(view, check); });
        }
        auto wrongStep = enums;
        for (qsizetype i = 0; i < wrongStep.size(); ++i) {
            auto entry = wrongStep[i].toObject();
            entry["step"] = 1;
            wrongStep[i] = entry;
        }
        context = "wrong step";
        withPackage(wrongStep, [&](auto& view, auto&) { checkOrdinary(view, check); });
        context = "empty summary";
        withPackage({}, [&](auto& view, auto&) { checkOrdinary(view, check); });
        context = "absent summary";
        withPackage({}, [&](auto& view, auto&) { checkOrdinary(view, check); }, false, true);
        // Methods are specific to each mechanical quantity.
        context = "swapped FVM enums";
        withPackage({mechanical("warpage_pv", "fvm_cell_centers"),
                     mechanical("U_mag_max", "fvm_cell_centers_lsq")},
                    [&](auto& view, auto&) { checkOrdinary(view, check); });
    });
    run("first qualifying mechanical summary entry wins", [&](auto check) {
        for (const auto& name : {"warpage_pv", "U_mag_max"}) {
            const auto fvm = mechanical(
                name, QString(name) == "warpage_pv" ? "fvm_cell_centers_lsq" : "fvm_cell_centers");
            const auto unsolved = mechanical(name, "not_solved");
            for (bool fvmFirst : {true, false}) {
                context = QString(name) + (fvmFirst ? " first FVM" : " first unsolved");
                withPackage(fvmFirst ? QJsonArray{fvm, unsolved} : QJsonArray{unsolved, fvm},
                            [&](auto& view, auto&) {
                                if (fvmFirst)
                                    checkFvm(view, check);
                                else
                                    checkUnsolved(view, check);
                            });
            }
            for (const auto& method :
                 std::vector<QJsonValue>{QJsonValue(QJsonValue::Undefined),
                                         "",
                                         QJsonValue(QJsonValue::Null),
                                         1,
                                         true,
                                         "fem_q2_nodes",
                                         QJsonArray{"not_solved"},
                                         QJsonObject{{"method", "fvm_cell_centers"}}}) {
                context = QString(name) +
                          " selected blocking method type=" + QString::number(method.type());
                withPackage({mechanical(name, method), fvm, unsolved},
                            [&](auto& view, auto&) { checkOrdinary(view, check); });
            }
            for (bool selectedFvm : {true, false}) {
                auto invalid = selectedFvm ? unsolved : fvm;
                invalid["value"] = "not numeric";
                auto otherStep = selectedFvm ? unsolved : fvm;
                otherStep["step"] = 1;
                auto noStep = otherStep;
                noStep.remove("step");
                auto noValue = invalid;
                noValue.remove("value");
                context = QString(name) + (selectedFvm ? " skip to FVM" : " skip to unsolved");
                withPackage({invalid, otherStep, noStep, noValue, selectedFvm ? fvm : unsolved},
                            [&](auto& view, auto&) {
                                if (selectedFvm)
                                    checkFvm(view, check);
                                else
                                    checkUnsolved(view, check);
                            });
            }
        }
    });
    run("missing temperature and notice share label", [&](auto check) {
        withPackage(
            enums,
            [&](auto& view, auto&) {
                view.setColorField(ResultColorField::Temperature);
                check(!view.colorRange().valid, "missing T fixture unexpectedly has temperature");
                check(exactStatus(view,
                                  QStringLiteral("无温度场\nFVM 弹性结果：精度未验证，仅供演示")),
                      "missing exact two-line temperature/FVM status");
                check(matchingTextActors(view, notice).size() == 1, "missing T lost FVM notice");
            },
            true);
        withPackage(
            {mechanical("warpage_pv", "not_solved")},
            [&](auto& view, auto&) {
                view.setColorField(ResultColorField::Temperature);
                check(exactStatus(view, QStringLiteral("无温度场")),
                      "unsolved missing-T status changed");
                check(!view.colorRange().valid, "unsolved missing-T range must be invalid");
                check(matchingTextActors(view, notice, false).empty(), "unsolved missing-T notice");
            },
            true);
    });
    run("notice coexists with provenance watermark", [&](auto check) {
        withPackage(enums, [&](auto& view, auto&) {
            for (const auto& source : {"mock", "precomputed"}) {
                context = source;
                require(
                    view.setProvenance({{"schema", "chipcae.provenance/0.1"}, {"source", source}}),
                    "valid provenance rejected");
                check(matchingTextActors(view, source == QString("mock") ? "MOCK" : "PRECOMPUTED")
                              .size() == 1,
                      "missing visible provenance actor");
                check(matchingTextActors(view, notice).size() == 1,
                      "provenance and exactly one FVM notice must coexist");
                check(exactStatus(view, fvmStatus), "provenance lost FVM status");
            }
        });
    });
    run("notice actor has fixed viewport properties", [&](auto check) {
        withPackage(enums, [&](auto& view, auto&) {
            const auto actors = matchingTextActors(view, notice);
            check(actors.size() == 1, "missing unique notice actor for properties");
            for (auto actor : actors) {
                check(actor->GetPositionCoordinate()->GetCoordinateSystem() ==
                          VTK_NORMALIZED_VIEWPORT,
                      "notice coordinates must be normalized viewport");
                check(actor->GetPosition()[0] == 0.02 && actor->GetPosition()[1] == 0.02,
                      "notice position must be exactly (0.02, 0.02)");
                check(actor->GetTextProperty()->GetFontSize() == 18, "notice font must be 18");
                check(!actor->GetPickable(), "notice must not intercept picking");
            }
        });
    });
    run("reload clears notice", [&](auto check) {
        withPackage(enums, [&](auto& view, auto& session) {
            check(matchingTextActors(view, notice).size() == 1, "reload needs initial FVM notice");
            require(view.loadStep(baselineSession, 0), qPrintable(view.error()));
            checkOrdinary(view, check);
            // Reload FVM immediately before failing: the retained session has FVM metadata.
            require(view.loadStep(session, 0), qPrintable(view.error()));
            check(matchingTextActors(view, notice).size() == 1, "FVM reload lost notice");
            require(!view.loadStep(session, 999), "nonexistent step unexpectedly loaded");
            noNotice(view, check);
            check(exactStatus(view, {}), "failed load did not clear status");
            for (auto field : fields) {
                view.setColorField(field);
                view.setWarpageScale(1000);
                noNotice(view, check);
                check(exactStatus(view, {}), "rebuild resurrected failed-load status");
            }
        });
    });
    run("status updated before displayChanged", [&](auto check) {
        withPackage(enums, [&](auto& view, auto&) {
            int signalCount = 0;
            QObject::connect(&view, &ResultView::displayChanged, &view, [&] {
                ++signalCount;
                check(exactStatus(view, fvmStatus), "FVM signal preceded status update");
                check(matchingTextActors(view, notice).size() == 1,
                      "FVM signal preceded notice update");
            });
            for (auto field : fields)
                view.setColorField(field);
            check(signalCount == static_cast<int>(fields.size()), "missing field-change signals");
        });
        withPackage({mechanical("warpage_pv", "not_solved")}, [&](auto& view, auto&) {
            int signalCount = 0;
            QObject::connect(&view, &ResultView::displayChanged, &view, [&] {
                ++signalCount;
                const bool mechanics =
                    view.colorField() == ResultColorField::DisplacementMagnitude ||
                    view.colorField() == ResultColorField::WarpageUz;
                check(exactStatus(view, mechanics ? unsolvedStatus : QString{}),
                      "unsolved signal preceded status update");
                check(matchingTextActors(view, notice, false).empty(),
                      "unsolved signal has notice");
            });
            for (auto field : {ResultColorField::WarpageUz,
                               ResultColorField::Temperature,
                               ResultColorField::DisplacementMagnitude,
                               ResultColorField::LayerId})
                view.setColorField(field);
            check(signalCount == 4, "missing unsolved transition signals");
        });
    });
    return failures;
}

int methodSummaryAcceptance(const QDir& root) {
    int failures = 0;
    auto run = [&](const QString& name, auto test) {
        const auto before = failures;
        try {
            // Accumulate text mismatches so every QoI is checked even during the RED pass.
            auto equal = [&](const QString& actual, const QString& expected) {
                if (actual != expected) {
                    ++failures;
                    std::cerr << "FAIL D14e-3 " << name.toStdString() << ": expected ["
                              << expected.toStdString() << "] got [" << actual.toStdString()
                              << "]\n";
                }
            };
            test(equal);
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL D14e-3 " << name.toStdString() << ": " << e.what() << '\n';
        }
        if (failures == before)
            std::cout << "PASS D14e-3 " << name.toStdString() << '\n';
    };
    auto temperature = [] {
        return QJsonObject{{"step", 0}, {"name", "T_max"}, {"value", 987.5}, {"unit", "K"}};
    };
    auto warpage = [] {
        return QJsonObject{{"step", 0}, {"name", "warpage_pv"}, {"value", 0}, {"unit", "m"}};
    };
    run("long sentences, frozen enums and disabled mechanics stay distinct", [&](auto equal) {
        struct Example {
            QString thermalMethod;
            QString mechanicalMethod;
            QString expectedTmax;
            QString expectedWarpage;
            QString expectedMagnitude;
        };
        const std::array<Example, 3> examples{{
            {"FVM cell centers",
             "not solved: fvm thermal-only stage",
             "T_max: 300.2803835909066 K [FVM cell centers]",
             "warpage_pv: 0 m [not solved: fvm thermal-only stage]",
             "U_mag_max: 0 m [not solved: fvm thermal-only stage]"},
            {"fvm_cell_centers",
             "not_solved",
             "T_max: 300.2803835909066 K [fvm_cell_centers]",
             "warpage_pv: 0 m [not_solved]",
             "U_mag_max: 0 m [not_solved]"},
            {"fvm_cell_centers",
             "mechanical_disabled",
             "T_max: 300.2803835909066 K [fvm_cell_centers]",
             "warpage_pv: 0 m [mechanical_disabled]",
             "U_mag_max: 0 m [mechanical_disabled]"},
        }};
        std::array<QString, 3> warpages, magnitudes;
        for (std::size_t i = 0; i < examples.size(); ++i) {
            const auto& example = examples[i];
            QTemporaryDir temp; // A, B and C each own a separate package directory.
            auto t = temperature();
            t["value"] = 300.2803835909066;
            t["method"] = example.thermalMethod;
            auto w = warpage();
            w["method"] = example.mechanicalMethod;
            auto u = w;
            u["name"] = "U_mag_max";
            writeSummaryFixture(root, temp, {t, w, u});
            ResultSession session;
            require(session.open(temp.path()), qPrintable(session.error()));
            ResultView view;
            require(view.loadStep(session, 0), qPrintable(view.error()));
            // Summary metadata must not replace the already working PointData T coloring.
            require(view.colorRange().valid, "method package lost PointData T range");
            equal(view.colorBarTitle(), "T (K)");
            equal(view.tmaxText(), example.expectedTmax);
            equal(view.summary("T_max"), example.expectedTmax);
            equal(view.tmaxText(), view.summary("T_max"));
            warpages[i] = view.summary("warpage_pv");
            magnitudes[i] = view.summary("U_mag_max");
            equal(warpages[i], example.expectedWarpage);
            equal(magnitudes[i], example.expectedMagnitude);
        }
        for (std::size_t i = 0; i < examples.size(); ++i) {
            require(warpages[i] != "warpage_pv: 0 m", "mechanical placeholder looks solved");
            require(magnitudes[i] != "U_mag_max: 0 m", "displacement placeholder looks solved");
            for (std::size_t j = i + 1; j < examples.size(); ++j) {
                require(warpages[i] != warpages[j], "mechanical methods conflated");
                require(magnitudes[i] != magnitudes[j], "displacement methods conflated");
            }
        }
    });
    struct Location {
        QJsonObject fields;
        QString expected;
    };
    for (const auto& location : std::vector<Location>{
             {{{"region", "die"}}, "T_max: 987.5 K [fvm_cell_centers] region=die"},
             {{{"region", "die"},
               {"region_id", "ignored"},
               {"position", QJsonArray{1, 2, 3}},
               {"xyz", QJsonArray{4, 5, 6}},
               {"location", "surface"}},
              "T_max: 987.5 K [fvm_cell_centers] region=die at=(1,2,3)"},
             {{{"region", "die"}, {"location", "surface"}},
              "T_max: 987.5 K [fvm_cell_centers] region=die location=surface"}}) {
        run("method precedes location: " + location.expected, [&](auto equal) {
            QTemporaryDir temp;
            auto t = temperature();
            t["method"] = "fvm_cell_centers";
            for (auto it = location.fields.begin(); it != location.fields.end(); ++it)
                t[it.key()] = it.value();
            writeSummaryFixture(root, temp, {t});
            ResultSession session;
            require(session.open(temp.path()), qPrintable(session.error()));
            ResultView view;
            require(view.loadStep(session, 0), qPrintable(view.error()));
            equal(view.tmaxText(), location.expected);
            equal(view.summary("T_max"), "T_max: 987.5 K [fvm_cell_centers]");
        });
    }
    for (const auto& method : std::vector<QJsonValue>{QJsonValue(QJsonValue::Undefined),
                                                      "",
                                                      QJsonValue(QJsonValue::Null),
                                                      17,
                                                      true,
                                                      QJsonArray{"not_solved"},
                                                      QJsonObject{{"method", "not_solved"}}}) {
        run("absent, empty or non-string method type " + QString::number(method.type()),
            [&](auto equal) {
                QTemporaryDir temp;
                auto t = temperature();
                auto w = warpage();
                if (!method.isUndefined()) {
                    t["method"] = method;
                    w["method"] = method;
                }
                auto u = w;
                u["name"] = "U_mag_max";
                writeSummaryFixture(root, temp, {t, w, u});
                ResultSession session;
                require(session.open(temp.path()), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                equal(view.tmaxText(), "T_max: 987.5 K");
                equal(view.summary("T_max"), "T_max: 987.5 K");
                equal(view.summary("warpage_pv"), "warpage_pv: 0 m");
                equal(view.summary("U_mag_max"), "U_mag_max: 0 m");
            });
    }
    run("preserve method whitespace and empty unit spacing", [&](auto equal) {
        QTemporaryDir temp;
        auto t = temperature();
        t["method"] = " FVM cell centers ";
        auto w = warpage();
        w["unit"] = "";
        w["method"] = "not_solved";
        writeSummaryFixture(root, temp, {t, w});
        ResultSession session;
        require(session.open(temp.path()), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        equal(view.tmaxText(), "T_max: 987.5 K [ FVM cell centers ]");
        equal(view.summary("T_max"), "T_max: 987.5 K [ FVM cell centers ]");
        equal(view.summary("warpage_pv"), "warpage_pv: 0  [not_solved]");
    });
    for (bool absent : {false, true}) {
        run(absent ? "missing names stay unavailable" : "non-numeric values stay unavailable",
            [&](auto equal) {
                QTemporaryDir temp;
                auto t = temperature();
                auto w = warpage();
                t["method"] = "fvm_cell_centers";
                w["method"] = "not_solved";
                if (absent) {
                    t.remove("name");
                    w.remove("name");
                } else {
                    t["value"] = "987.5";
                    w["value"] = QJsonValue(QJsonValue::Null);
                }
                writeSummaryFixture(root, temp, {t, w});
                ResultSession session;
                require(session.open(temp.path()), qPrintable(session.error()));
                ResultView view;
                equal(view.tmaxText(), "T_max: unavailable");
                equal(view.summary("T_max"), "T_max: unavailable");
                equal(view.summary("warpage_pv"), "warpage_pv: unavailable");
                require(view.loadStep(session, 0), qPrintable(view.error()));
                equal(view.tmaxText(), "T_max: unavailable");
                equal(view.summary("T_max"), "T_max: unavailable");
                equal(view.summary("warpage_pv"), "warpage_pv: unavailable");
            });
    }
    for (bool firstHasMethod : {true, false}) {
        run(firstHasMethod ? "only selected exact-step method is displayed"
                           : "selected entry cannot borrow a later method",
            [&](auto equal) {
                QTemporaryDir temp;
                auto t = temperature();
                auto w = warpage();
                auto global = t;
                global.remove("step");
                global["value"] = -123;
                global["method"] = "global method";
                auto other = t;
                other["step"] = 1;
                other["value"] = -456;
                other["method"] = "other step method";
                auto invalid = t;
                invalid["value"] = "not a number";
                invalid["method"] = "invalid value method";
                auto invalidW = invalid;
                invalidW["name"] = "warpage_pv";
                auto globalW = global;
                globalW["name"] = "warpage_pv";
                auto otherW = other;
                otherW["name"] = "warpage_pv";
                auto duplicateT = t;
                duplicateT["method"] = "later thermal method";
                auto duplicateW = w;
                duplicateW["method"] = "later mechanical method";
                if (firstHasMethod) {
                    t["method"] = "fvm_cell_centers_lsq";
                    w["method"] = "not_solved";
                }
                writeSummaryFixture(root,
                                    temp,
                                    {global,
                                     globalW,
                                     other,
                                     otherW,
                                     invalid,
                                     invalidW,
                                     t,
                                     w,
                                     duplicateT,
                                     duplicateW});
                ResultSession session;
                require(session.open(temp.path()), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                equal(view.tmaxText(),
                      firstHasMethod ? "T_max: 987.5 K [fvm_cell_centers_lsq]" : "T_max: 987.5 K");
                equal(view.summary("T_max"),
                      firstHasMethod ? "T_max: 987.5 K [fvm_cell_centers_lsq]" : "T_max: 987.5 K");
                equal(view.summary("warpage_pv"),
                      firstHasMethod ? "warpage_pv: 0 m [not_solved]" : "warpage_pv: 0 m");
            });
    }
    run("global T_max fallback does not change summary step matching", [&](auto equal) {
        QTemporaryDir temp;
        auto t = temperature();
        t.remove("step");
        t["method"] = "fvm_cell_centers";
        writeSummaryFixture(root, temp, {t});
        ResultSession session;
        require(session.open(temp.path()), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        equal(view.tmaxText(), "T_max: 987.5 K [fvm_cell_centers]");
        equal(view.summary("T_max"), "T_max: unavailable");
    });
    for (bool firstHasMethod : {true, false}) {
        run(firstHasMethod ? "global fallback uses only the first valid entry's method"
                           : "global fallback cannot borrow another entry's method",
            [&](auto equal) {
                QTemporaryDir temp;
                auto selected = temperature();
                selected.remove("step");
                auto invalidGlobal = selected;
                invalidGlobal["value"] = "not a number";
                invalidGlobal["method"] = "invalid global method";
                auto invalidExact = invalidGlobal;
                invalidExact["step"] = 0;
                invalidExact["method"] = "invalid exact-step method";
                auto otherStep = temperature();
                otherStep["step"] = 1;
                otherStep["method"] = "other step method";
                auto duplicate = selected;
                duplicate["value"] = -123;
                duplicate["method"] = "later global method";
                if (firstHasMethod)
                    selected["method"] = "fvm_cell_centers";
                writeSummaryFixture(
                    root, temp, {invalidGlobal, invalidExact, otherStep, selected, duplicate});
                ResultSession session;
                require(session.open(temp.path()), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                require(view.colorRange().valid, "global fallback lost PointData T range");
                equal(view.colorBarTitle(), "T (K)");
                equal(view.tmaxText(),
                      firstHasMethod ? "T_max: 987.5 K [fvm_cell_centers]" : "T_max: 987.5 K");
                equal(view.summary("T_max"), "T_max: unavailable");
            });
    }
    return failures;
}
// Two thin hexes have the shared face z=0.1 and continuous, nonzero affine U.
// Extracting each cell into its own partition duplicates the four interface points.
void twoLayers(vtkUnstructuredGrid* grid, const QString& variant = {}) {
    grid->Initialize();
    require(grid->AllocateEstimate(2, 8), "allocate adjacent fixture connectivity");
    vtkNew<vtkPoints> points;
    points->SetDataTypeToDouble();
    for (double z : {0.0, 0.1, 0.2})
        for (auto xy : {std::array<double, 2>{0, 0}, {2, 0}, {2, 3}, {0, 3}})
            points->InsertNextPoint(xy[0], xy[1], z);
    grid->SetPoints(points);
    vtkIdType lower[] = {0, 1, 2, 3, 4, 5, 6, 7};
    vtkIdType upper[] = {4, 5, 6, 7, 8, 9, 10, 11};
    if (variant == "separated layers")
        for (int i = 0; i < 8; ++i) {
            double point[3];
            points->GetPoint(upper[i], point);
            point[0] += 4;
            upper[i] = points->InsertNextPoint(point);
        }
    if (variant == "roundoff coordinates" || variant == "discontinuous displacement")
        for (int i = 0; i < 8; ++i) {
            double point[3];
            points->GetPoint(upper[i], point);
            if (variant == "roundoff coordinates")
                point[0] += 1e-12;
            upper[i] = points->InsertNextPoint(point);
        }
    if (variant == "surface cells" || variant == "particles") {
        const int type = variant == "surface cells" ? VTK_QUAD : VTK_VERTEX;
        const vtkIdType count = variant == "surface cells" ? 4 : 1;
        grid->InsertNextCell(type, count, upper);
        grid->InsertNextCell(type, count, upper);
    } else {
        grid->InsertNextCell(VTK_HEXAHEDRON, 8, lower);
        grid->InsertNextCell(VTK_HEXAHEDRON, 8, upper);
    }
    vtkNew<vtkDoubleArray> temperature;
    temperature->SetName("T");
    vtkNew<vtkDoubleArray> displacement;
    displacement->SetName("U");
    displacement->SetNumberOfComponents(3);
    for (vtkIdType i = 0; i < points->GetNumberOfPoints(); ++i) {
        double point[3];
        points->GetPoint(i, point);
        temperature->InsertNextValue(300 + point[0] + point[2]);
        displacement->InsertNextTuple3(1e-3 * (1 + point[0]),
                                       2e-3 * (1 + point[1]),
                                       1e-3 * (1 + point[0] + point[1] + point[2]));
    }
    if (variant == "discontinuous displacement")
        for (vtkIdType i = 12; i < points->GetNumberOfPoints(); ++i)
            displacement->SetComponent(i, 0, displacement->GetComponent(i, 0) + 0.025);
    grid->GetPointData()->AddArray(temperature);
    grid->GetPointData()->AddArray(displacement);
    for (auto name : {"cell_kind", "layer_id", "material_id"}) {
        vtkNew<vtkIntArray> array;
        array->SetName(name);
        array->InsertNextValue(QString(name) == "cell_kind" ? 0 : 1);
        array->InsertNextValue(QString(name) == "cell_kind" ? 1 : 2);
        grid->GetCellData()->AddArray(array);
    }
    if (variant == "equal layers")
        grid->GetCellData()->GetArray("layer_id")->SetComponent(1, 0, 1);
    if (variant == "missing layer_id")
        grid->GetCellData()->RemoveArray("layer_id");
}
void checkWarpedInterface(ResultView& view, double scale) {
    auto input = boundaryInput(view);
    require(input && input->GetNumberOfCells() >= 1,
            "adjacent layers have no layerBoundaries cells");
    auto actor = boundaryActor(view);
    require(!actor->GetPickable(), "layer boundaries must not intercept picking");
    require(!actor->GetMapper()->GetScalarVisibility(), "layer boundaries use field coloring");
    require(actor->GetProperty()->GetLineWidth() >= 2, "layer boundary line is too thin");
    const auto color = actor->GetProperty()->GetColor();
    require(std::max({color[0], color[1], color[2]}) < 0.5, "layer boundary lines must be dark");
    // Expected coordinates come from the manufactured affine U, independently of shown.
    std::array<std::array<double, 3>, 4> corners = {
        std::array<double, 3>{0, 0, 0.1}, {2, 0, 0.1}, {2, 3, 0.1}, {0, 3, 0.1}};
    for (auto& p : corners) {
        const auto original = p;
        p[0] += scale * 1e-3 * (1 + original[0]);
        p[1] += scale * 2e-3 * (1 + original[1]);
        p[2] += scale * 1e-3 * (1 + original[0] + original[1] + original[2]);
    }
    auto corner = [&](const double* point) {
        for (int i = 0; i < 4; ++i) {
            bool matches = true;
            for (int axis = 0; axis < 3; ++axis)
                matches &= std::abs(point[axis] - corners[i][axis]) <=
                           1e-10 + 5.5e-6 * std::abs(corners[i][axis]);
            if (matches)
                return i;
        }
        return -1;
    };
    std::array<bool, 4> seen{};
    for (vtkIdType i = 0; i < input->GetNumberOfCells(); ++i) {
        auto cell = input->GetCell(i);
        require(cell->GetCellDimension() == 1 && cell->GetNumberOfPoints() >= 2,
                "layer boundary input contains a non-line cell");
        for (vtkIdType segment = 1; segment < cell->GetNumberOfPoints(); ++segment) {
            double a[3], b[3];
            input->GetPoint(cell->GetPointId(segment - 1), a);
            input->GetPoint(cell->GetPointId(segment), b);
            const int first = corner(a), second = corner(b);
            require(first >= 0 && second >= 0,
                    "boundary endpoint is not on the currently warped shared face");
            require((first + 1) % 4 == second || (second + 1) % 4 == first,
                    "boundary segment is an exterior edge, diagonal, or degenerate edge");
            seen[(first + 1) % 4 == second ? first : second] = true;
        }
    }
    require(std::all_of(seen.begin(), seen.end(), [](bool edge) { return edge; }),
            "layer boundary omits part of the shared-face perimeter");
}
int obliqueAndTemperatureAcceptance(const QDir& root) {
    int failures = 0;
    auto run = [&](const QString& name, auto test) {
        try {
            test();
            std::cout << "PASS D14e-2 " << name.toStdString() << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL D14e-2 " << name.toStdString() << ": " << e.what() << '\n';
        }
    };
    run("default camera is oblique", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkOblique(view);
        checkNoBoundaries(view);
    });
    run("default single layer has no boundaries", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkNoBoundaries(view);
    });
    for (bool manual : {false, true})
        run(manual ? "rebuild preserves manually rotated camera" : "rebuild preserves load camera",
            [&] {
                ResultSession session;
                require(session.open(root.filePath("default")), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                if (manual) {
                    camera(view)->Azimuth(17);
                    camera(view)->Elevation(-11);
                    camera(view)->OrthogonalizeViewUp();
                }
                const auto direction = cameraDirection(view);
                view.setColorField(ResultColorField::LayerId);
                checkCameraDirection(view, direction);
                view.setWarpageScale(10);
                checkCameraDirection(view, direction);
                view.setLayer(1);
                checkCameraDirection(view, direction);
                view.setLayer(0);
                checkCameraDirection(view, direction);
                view.setShowVoid(true);
                checkCameraDirection(view, direction);
                view.setShowVoid(false);
                checkCameraDirection(view, direction);
                view.setBlockVisible("hex", false);
                checkCameraDirection(view, direction);
                view.setBlockVisible("hex", true);
                checkCameraDirection(view, direction);
                view.setClip(true, {0.2, 0, 0}, {1, 0, 0});
                require(view.error().isEmpty(), qPrintable(view.error()));
                checkCameraDirection(view, direction);
                view.setClip(false);
                checkCameraDirection(view, direction);
            });
    run("reload preserves camera orientation and roll", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkOblique(view);
        for (int repeat = 0; repeat < 3; ++repeat) {
            camera(view)->Azimuth(37);
            camera(view)->Elevation(-13);
            camera(view)->Roll(23);
            camera(view)->OrthogonalizeViewUp();
            const auto direction = cameraDirection(view);
            std::array<double, 3> up;
            camera(view)->GetViewUp(up.data());
            if (repeat == 1)
                require(!view.loadStep(session, 999), "invalid reload unexpectedly succeeded");
            require(view.loadStep(session, 0), qPrintable(view.error()));
            checkCameraDirection(view, direction);
            for (int axis = 0; axis < 3; ++axis)
                near(camera(view)->GetViewUp()[axis], up[axis], 1e-12);
        }
    });
    run("failed first load still initializes an oblique camera", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(!view.loadStep(session, 999), "invalid first load unexpectedly succeeded");
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkOblique(view);
    });
    run("thin adjacent stack has oblique default and no default clipping", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "adjacent fixture directory");
        const auto path = temp.filePath("stack.vtkhdf");
        writeFixture(root, path, [](vtkUnstructuredGrid* grid) { twoLayers(grid); });
        ResultSession session;
        require(session.open(path), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        auto hex = view.displayed("hex");
        require(hex && hex->GetNumberOfCells() == 2 && hex->GetCellType(0) == VTK_HEXAHEDRON &&
                    hex->GetCellType(1) == VTK_HEXAHEDRON,
                "default load clipped or hid part of the adjacent stack");
        checkOblique(view);
    });
    for (bool partitioned : {false, true})
        run(partitioned ? "partition duplicate-point interface follows warpage"
                        : "shared-point interface follows warpage",
            [&] {
                QTemporaryDir temp;
                require(temp.isValid(), "boundary fixture directory");
                const auto path = temp.filePath("stack.vtkhdf");
                writeFixture(
                    root, path, [](vtkUnstructuredGrid* grid) { twoLayers(grid); }, partitioned);
                ResultSession session;
                require(session.open(path), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                auto hex = view.displayed("hex");
                require(hex && hex->GetNumberOfCells() == 2 &&
                            hex->GetNumberOfPoints() == (partitioned ? 16 : 12),
                        "boundary fixture lost its intended shared/partition point topology");
                checkWarpedInterface(view, 1);
                view.setWarpageScale(10);
                checkWarpedInterface(view, 10);
                view.setColorField(ResultColorField::LayerId);
                checkWarpedInterface(view, 10);
                view.setLayer(1);
                checkNoBoundaries(view);
                view.setLayer(0);
                checkWarpedInterface(view, 10);
                view.setBlockVisible("hex", false);
                checkNoBoundaries(view);
                view.setBlockVisible("hex", true);
                checkWarpedInterface(view, 10);
            });
    for (const auto& variant : {"roundoff coordinates", "discontinuous displacement"})
        run(QString("undeformed adjacency survives ") + variant, [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "duplicate interface fixture directory");
            const auto path = temp.filePath("stack.vtkhdf");
            writeFixture(root, path, [&](vtkUnstructuredGrid* grid) { twoLayers(grid, variant); });
            ResultSession session;
            require(session.open(path), qPrintable(session.error()));
            ResultView view;
            require(view.loadStep(session, 0), qPrintable(view.error()));
            for (double scale : {0.0, 1.0, 10.0}) {
                view.setWarpageScale(scale);
                auto input = boundaryInput(view);
                require(input && input->GetNumberOfCells() > 0,
                        "undeformed adjacent layers lost their boundary actor");
                // Both interface perimeters must follow their own displayed solution points.
                const auto shown = view.displayed("hex");
                require(shown && shown->GetNumberOfCells() == 2,
                        "duplicate interface fixture lost its cells");
                std::array<std::array<vtkIdType, 4>, 2> faces;
                for (int corner = 0; corner < 4; ++corner) {
                    faces[0][corner] = shown->GetCell(0)->GetPointId(4 + corner);
                    faces[1][corner] = shown->GetCell(1)->GetPointId(corner);
                    require(faces[0][corner] != faces[1][corner],
                            "interface solution points were merged");
                }
                for (const auto& face : faces)
                    for (int edge = 0; edge < 4; ++edge) {
                        double expectedA[3], expectedB[3];
                        shown->GetPoint(face[edge], expectedA);
                        shown->GetPoint(face[(edge + 1) % 4], expectedB);
                        auto matches = [](const double* a, const double* b) {
                            for (int axis = 0; axis < 3; ++axis)
                                if (std::abs(a[axis] - b[axis]) > 1e-10)
                                    return false;
                            return true;
                        };
                        bool found = false;
                        for (vtkIdType i = 0; i < input->GetNumberOfCells(); ++i) {
                            auto line = input->GetCell(i);
                            require(line->GetNumberOfPoints() == 2, "boundary must be an edge");
                            double a[3], b[3];
                            input->GetPoint(line->GetPointId(0), a);
                            input->GetPoint(line->GetPointId(1), b);
                            found |= (matches(a, expectedA) && matches(b, expectedB)) ||
                                     (matches(a, expectedB) && matches(b, expectedA));
                        }
                        require(found, "boundary edge does not follow its displayed face");
                    }
            }
            view.setLayer(1);
            checkNoBoundaries(view);
            view.setLayer(0);
            require(boundaryInput(view), "restoring layer visibility lost interface");
        });
    run("warpage cannot create adjacency between disconnected layers", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "disconnected interface fixture directory");
        const auto path = temp.filePath("stack.vtkhdf");
        writeFixture(root, path, [](vtkUnstructuredGrid* grid) {
            twoLayers(grid, "separated layers");
            auto u = grid->GetPointData()->GetArray("U");
            for (vtkIdType i = 0; i < grid->GetNumberOfPoints(); ++i) {
                u->SetComponent(i, 0, i >= 12 ? -4 : 0);
                u->SetComponent(i, 1, 0);
                u->SetComponent(i, 2, 0);
            }
        });
        ResultSession session;
        require(session.open(path), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkNoBoundaries(view);
    });
    for (const auto& variant :
         {"equal layers", "missing layer_id", "separated layers", "surface cells", "particles"})
        run(QString("no interface for ") + variant, [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "negative boundary fixture directory");
            const auto path = temp.filePath("stack.vtkhdf");
            writeFixture(root, path, [&](vtkUnstructuredGrid* grid) { twoLayers(grid, variant); });
            ResultSession session;
            require(session.open(path), qPrintable(session.error()));
            ResultView view;
            require(view.loadStep(session, 0), qPrintable(view.error()));
            checkNoBoundaries(view);
            view.setWarpageScale(10);
            checkNoBoundaries(view);
            checkOblique(view);
        });
    run("void filtering removes one side of interface", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "void interface fixture directory");
        const auto path = temp.filePath("stack.vtkhdf");
        writeFixture(root, path, [](vtkUnstructuredGrid* grid) {
            twoLayers(grid);
            grid->GetCellData()
                ->GetArray("cell_kind")
                ->SetComponent(1, 0, ResultSession::hiddenCellKind);
        });
        ResultSession session;
        require(session.open(path), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkNoBoundaries(view);
        view.setShowVoid(true);
        checkWarpedInterface(view, 1);
        view.setShowVoid(false);
        checkNoBoundaries(view);
    });
    run("mesh-only temperature status", [&] {
        ResultSession session;
        require(session.open(root.filePath("viewer_mesh")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        view.setColorField(ResultColorField::Temperature);
        checkUnavailable(view, true);
    });
    run("mesh-only controls explain missing temperature", [&] {
        ResultSession session;
        require(session.open(root.filePath("viewer_mesh")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        ResultControls controls(view, session);
        checkTemperatureControls(controls);
    });
    run("no PointData T retains U and shows temperature status", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "missing T fixture directory");
        const auto path = temp.filePath("no-T.vtkhdf");
        writeFixture(
            root, path, [](vtkUnstructuredGrid* grid) { grid->GetPointData()->RemoveArray("T"); });
        ResultSession session;
        require(session.open(path), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        require(view.displayed("hex")->GetPointData()->HasArray("U"), "missing T fixture lost U");
        checkUnavailable(view, true);
        ResultControls controls(view, session);
        checkTemperatureControls(controls);
    });
    for (auto field : {ResultColorField::DisplacementMagnitude,
                       ResultColorField::WarpageUz,
                       ResultColorField::LayerId,
                       ResultColorField::MaterialId})
        run("missing T status follows selected field " + QString::number(static_cast<int>(field)),
            [&] {
                QTemporaryDir temp;
                require(temp.isValid(), "field transition fixture directory");
                const auto path = temp.filePath("no-T.vtkhdf");
                writeFixture(root, path, [](vtkUnstructuredGrid* grid) {
                    addMaterialIds(grid);
                    grid->GetPointData()->RemoveArray("T");
                });
                ResultSession session;
                require(session.open(path), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                ResultControls controls(view, session);
                view.setColorField(field);
                checkFieldStatus(view, false);
                require(view.colorRange().valid, "removing T made another field unavailable");
                if (field == ResultColorField::DisplacementMagnitude ||
                    field == ResultColorField::WarpageUz)
                    checkRange(view, field);
                else
                    checkIdRange(view, session, field);
                checkSummaryLabel(controls, view);
                view.setColorField(ResultColorField::Temperature);
                checkUnavailable(view, true);
                checkTemperatureControls(controls);
            });
    run("missing temperature status is ready at displayChanged", [&] {
        ResultSession session;
        require(session.open(root.filePath("viewer_mesh")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        view.setColorField(ResultColorField::LayerId);
        bool observed = false, correct = false;
        QObject::connect(&view, &ResultView::displayChanged, &view, [&] {
            observed = true;
            auto label = view.findChild<QLabel*>("fieldStatusLabel");
            correct = label && label->text() == QStringLiteral("无温度场") && !label->isHidden();
        });
        view.setColorField(ResultColorField::Temperature);
        require(observed && correct, "displayChanged precedes missing-temperature status update");
        checkFieldStatus(view, true);
    });
    run("mock provenance and missing temperature remain simultaneously readable", [&] {
        ResultSession session;
        require(session.open(root.filePath("viewer_mesh")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        require(view.setProvenance({{"schema", "chipcae.provenance/0.1"}, {"source", "mock"}}),
                "valid mock provenance rejected");
        require(!view.provenanceBanner().isEmpty(), "mock provenance banner empty");
        checkFieldStatus(view, true);
        VisibleStatusView visible(view);
        auto status = view.findChild<QLabel*>("fieldStatusLabel");
        QLabel* banner = nullptr;
        for (auto label : view.findChildren<QLabel*>())
            if (label->text() == view.provenanceBanner())
                banner = label;
        require(banner && banner->isVisible() && status && status->isVisible(),
                "provenance or missing-temperature label hidden");
        const QRect bannerRect(banner->mapTo(&view, QPoint{}), banner->size());
        const QRect statusRect(status->mapTo(&view, QPoint{}), status->size());
        require(!bannerRect.isEmpty() && !statusRect.isEmpty() &&
                    !bannerRect.intersects(statusRect),
                "provenance and missing-temperature labels overlap");
    });
    run("empty and failed load have no new overlays", [&] {
        ResultView view;
        checkUnavailable(view);
        checkNoBoundaries(view);
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        require(!view.loadStep(session, 999), "nonexistent step unexpectedly loaded");
        require(!view.error().isEmpty(), "failed load lost existing error path");
        checkFieldStatus(view, false);
        checkNoBoundaries(view);
    });
    struct FailedReloadCase {
        const char* name;
        bool missingT, missingFile, provenance;
    };
    for (const auto& test :
         {FailedReloadCase{
              "failed reload clears boundary overlay and result scene", false, false, false},
          FailedReloadCase{
              "failed reload clears temperature overlay and result scene", true, false, false},
          FailedReloadCase{"missing file reload clears result scene", false, true, false},
          FailedReloadCase{"missing file reload preserves provenance while clearing results",
                           false,
                           true,
                           true}})
        run(test.name, [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "failed reload fixture directory");
            const auto path = temp.filePath("stack.vtkhdf");
            writeFixture(root, path, [&](vtkUnstructuredGrid* grid) {
                twoLayers(grid);
                if (test.missingT)
                    grid->GetPointData()->RemoveArray("T");
            });
            ResultSession session;
            require(session.open(path), qPrintable(session.error()));
            ResultView view;
            if (test.provenance)
                require(
                    view.setProvenance({{"schema", "chipcae.provenance/0.1"}, {"source", "mock"}}),
                    "valid failed reload provenance rejected");
            require(view.loadStep(session, 0), qPrintable(view.error()));
            checkLoadedResultScene(view);
            if (test.missingT)
                checkUnavailable(view, true);
            else
                checkRange(view, ResultColorField::Temperature);
            camera(view)->Azimuth(17);
            camera(view)->Elevation(-11);
            camera(view)->OrthogonalizeViewUp();
            const auto direction = cameraDirection(view);
            const auto watermark = view.provenanceWatermark();
            const auto banner = view.provenanceBanner();
            auto checkProvenance = [&] {
                if (!test.provenance)
                    return;
                require(watermark == "MOCK" && !banner.isEmpty(),
                        "failed reload provenance fixture needs watermark and banner");
                require(view.provenanceWatermark() == watermark &&
                            view.provenanceBanner() == banner,
                        "failed reload changed provenance");
                auto props =
                    view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
                bool foundWatermark = false;
                props->InitTraversal();
                while (auto prop = props->GetNextProp())
                    if (auto text = vtkTextActor::SafeDownCast(prop))
                        foundWatermark |= QString::fromUtf8(text->GetInput()) == watermark &&
                                          text->GetVisibility();
                require(foundWatermark, "failed reload removed provenance watermark actor");
                VisibleStatusView visible(view);
                bool foundBanner = false;
                for (auto label : view.findChildren<QLabel*>())
                    foundBanner |= label->text() == banner && label->isVisible();
                require(foundBanner, "failed reload hid provenance banner");
            };
            checkProvenance();
            if (test.missingFile)
                require(QFile::remove(path), "could not remove loaded result fixture");
            require(!view.loadStep(session, test.missingFile ? 0 : 999),
                    "failed reload unexpectedly succeeded");
            require(view.error() == session.error(), "failed reload changed session error");
            if (!test.missingFile)
                require(!view.error().isEmpty(), "failed reload lost existing error path");
            checkProvenance();
            checkClearedResultScene(view);
            checkCameraDirection(view, direction);
            // Exercise rebuilds: retained raw blocks must not resurrect stale geometry.
            // No public loaded() accessor exists; hidden missing-T status also checks
            // the observable unloaded state after rebuilding.
            view.setColorField(ResultColorField::Temperature);
            checkClearedResultScene(view);
            checkProvenance();
            view.setWarpageScale(2);
            checkClearedResultScene(view);
            checkProvenance();
            checkCameraDirection(view, direction);
            writeFixture(root, path, [](vtkUnstructuredGrid* grid) { twoLayers(grid); });
            require(view.loadStep(session, 0), qPrintable(view.error()));
            require(view.error().isEmpty(), "successful recovery retained load error");
            checkLoadedResultScene(view);
            checkRange(view, ResultColorField::Temperature);
            checkFieldStatus(view, false);
            checkWarpedInterface(view, 2);
            checkProvenance();
            checkCameraDirection(view, direction);
        });
    run("restoring PointData T clears missing-temperature status", [&] {
        ResultSession mesh, withT;
        require(mesh.open(root.filePath("viewer_mesh")), qPrintable(mesh.error()));
        require(withT.open(root.filePath("default")), qPrintable(withT.error()));
        ResultView view;
        require(view.loadStep(mesh, 0), qPrintable(view.error()));
        require(view.loadStep(withT, 0), qPrintable(view.error()));
        checkRange(view, ResultColorField::Temperature);
        require(view.colorBarTitle() == "T (K)", "restored temperature title");
        checkFieldStatus(view, false);
    });
    run("summary T_max and T_cell cannot supply missing PointData T", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "cell temperature fixture directory");
        const auto path = temp.filePath("no-T.vtkhdf");
        writeFixture(root, path, [](vtkUnstructuredGrid* grid) {
            grid->GetPointData()->RemoveArray("T");
            vtkNew<vtkDoubleArray> cellT;
            cellT->SetName("T_cell");
            for (vtkIdType i = 0; i < grid->GetNumberOfCells(); ++i)
                cellT->InsertNextValue(900 + i);
            grid->GetCellData()->AddArray(cellT);
        });
        const QJsonObject manifest{
            {"schema_version", "chipcae.results/1.0"},
            {"fields", QJsonArray{}},
            {"steps", QJsonArray{QJsonObject{{"n", 0}, {"time", 0}, {"file", "no-T.vtkhdf"}}}},
            {"summary",
             QJsonArray{
                 QJsonObject{{"step", 0}, {"name", "T_max"}, {"value", 1234}, {"unit", "K"}}}}};
        QFile file(temp.filePath("manifest.json"));
        require(file.open(QIODevice::WriteOnly), "cell temperature manifest open");
        const auto bytes = QJsonDocument(manifest).toJson();
        require(file.write(bytes) == bytes.size(), "cell temperature manifest write");
        file.close();
        ResultSession session;
        require(session.open(temp.path()), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        require(view.tmaxText().contains("1234"), "summary temperature fixture not loaded");
        require(!view.colorRange().valid && view.colorBarTitle().isEmpty() &&
                    view.error().isEmpty(),
                "cell or summary temperature supplied point coloring");
        checkScalarBar(view);
        checkFieldStatus(view, true);
    });
    run("PointData T keeps valid temperature title and no missing status", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        checkRange(view, ResultColorField::Temperature);
        require(view.colorBarTitle() == "T (K)", "temperature title regression");
        checkFieldStatus(view, false);
        ResultControls controls(view, session);
        checkSummaryLabel(controls, view);
    });
    // Optional local packages are supplied by the caller; never commit real data
    // or developer-machine paths. Each check runs separately so camera failures
    // do not hide missing interfaces or temperature status on the same sample.
    struct LocalSample {
        QString name, path;
        bool missingT;
    };
    const auto sampleRoot = qEnvironmentVariable("CHIPCAE_VIEWER_SAMPLE_ROOT");
    const std::array<LocalSample, 4> samples = {
        LocalSample{
            "s1", sampleRoot.isEmpty() ? QString{} : QDir(sampleRoot).filePath("s1"), false},
        {"s1b", sampleRoot.isEmpty() ? QString{} : QDir(sampleRoot).filePath("s1b"), false},
        {"DEMO-GATE", qEnvironmentVariable("CHIPCAE_VIEWER_DEMO_GATE"), false},
        {"WP-0", qEnvironmentVariable("CHIPCAE_VIEWER_WP0"), true}};
    for (const auto& sample : samples) {
        if (sample.path.isEmpty() || !QFileInfo::exists(sample.path)) {
            std::cout << "SKIP D14e-2 optional local " << sample.name.toStdString() << '\n';
            continue;
        }
        for (int check = 0; check < 3; ++check)
            run("local " + sample.name +
                    (check == 0   ? " default camera"
                     : check == 1 ? " layer boundaries"
                                  : " temperature"),
                [&] {
                    ResultSession session;
                    require(session.open(sample.path), qPrintable(session.error()));
                    ResultView view;
                    require(view.loadStep(session, 0), qPrintable(view.error()));
                    if (check == 0) {
                        checkOblique(view);
                    } else if (check == 1) {
                        for (double scale : {1.0, 10.0}) {
                            view.setWarpageScale(scale);
                            auto input = boundaryInput(view);
                            require(input && input->GetNumberOfCells() >= 1,
                                    "local stacked package has no layer interface edges");
                        }
                    } else {
                        require(view.error().isEmpty(), "local package failed in temperature mode");
                        if (sample.missingT) {
                            require(!view.colorRange().valid && view.colorBarTitle().isEmpty(),
                                    "local missing T used cell/summary temperature");
                            checkFieldStatus(view, true);
                            ResultControls controls(view, session);
                            checkTemperatureControls(controls);
                        } else {
                            require(view.colorRange().valid && view.colorBarTitle() == "T (K)",
                                    "local PointData T unavailable");
                            checkFieldStatus(view, false);
                        }
                    }
                });
    }
    return failures;
}
int acceptance(const QDir& root) {
    int failures = 0;
    auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    run("ID enum indices", [&] {
        const std::array fields = {ResultColorField::Temperature,
                                   ResultColorField::DisplacementMagnitude,
                                   ResultColorField::WarpageUz,
                                   ResultColorField::LayerId,
                                   ResultColorField::MaterialId};
        for (int index = 0; index < 5; ++index)
            require(static_cast<int>(fields[index]) == index, "color field index changed");
    });
    run("five field controls and bidirectional synchronization", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        ResultControls controls(view, session);
        QComboBox* fields = nullptr;
        for (auto combo : controls.findChildren<QComboBox*>())
            if (combo->itemText(0) == QStringLiteral("温度"))
                fields = combo;
        require(fields, "field combo missing");
        require(fields->count() == 5, "field combo must contain five choices");
        const QStringList labels = {QStringLiteral("温度"),
                                    QStringLiteral("位移幅值"),
                                    QStringLiteral("翘曲"),
                                    QStringLiteral("叠层"),
                                    QStringLiteral("材料")};
        for (int index = 0; index < 5; ++index)
            require(fields->itemText(index) == labels[index], "field combo label or order");
        fields->setCurrentIndex(3);
        require(view.colorField() == ResultColorField::LayerId, "controls did not select LayerId");
        view.setColorField(ResultColorField::MaterialId);
        require(fields->currentIndex() == 4, "MaterialId left field combo stale");
        for (int round = 0; round < 2; ++round)
            for (int index = 0; index < 5; ++index) {
                fields->setCurrentIndex(index);
                require(view.colorField() == static_cast<ResultColorField>(index),
                        "field combo did not update view");
                view.setColorField(static_cast<ResultColorField>((index + 1) % 5));
                require(fields->currentIndex() == (index + 1) % 5,
                        "displayChanged did not synchronize field combo");
            }
    });
    for (auto field : {ResultColorField::LayerId, ResultColorField::MaterialId}) {
        const QString label = idArrayName(field);
        run(qPrintable(label + " named session coloring and step change"), [&] {
            ResultSession session;
            require(session.open(root.filePath("default")), qPrintable(session.error()));
            ResultView view;
            for (qint64 step : {0, 1}) {
                require(view.loadStep(session, step), qPrintable(view.error()));
                require(!(field == ResultColorField::LayerId ? session.layerNames()
                                                             : session.materialNames())
                             .isEmpty(),
                        "default fixture must have ID names");
                view.setColorField(field);
                checkIdRange(view, session, field);
                view.setColorField(ResultColorField::Temperature);
                checkRange(view, ResultColorField::Temperature);
                require(view.colorBarTitle() == "T (K)", "temperature title after ID coloring");
            }
        });
        for (int stage : {0, 1, 2}) {
            run(qPrintable(label + " visible bounds stage " + QString::number(stage)), [&] {
                QTemporaryDir temp;
                require(temp.isValid(), "ID fixture directory");
                const bool material = field == ResultColorField::MaterialId;
                const auto path = temp.filePath("step.vtkhdf");
                if (material)
                    writeFixture(root, path, addMaterialIds);
                ResultSession session;
                require(session.open(material ? path : root.filePath("viewer_mixed")),
                        qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                require(session.layerNames().isEmpty() && session.materialNames().isEmpty(),
                        "mixed ID fixture must have empty name tables");
                view.setColorField(field);
                if (stage >= 1)
                    view.setShowVoid(true);
                if (stage == 2)
                    view.setLayer(2);
                const double factor = material ? 10 : 1;
                checkIdBounds(view, (stage == 1 ? 1 : 2) * factor, (stage == 2 ? 2 : 3) * factor);
                checkIdRange(view, session, field);
                if (!material)
                    require(view.colorBarTitle() ==
                                (stage == 0   ? QStringLiteral("叠层: layer 2, layer 3")
                                 : stage == 1 ? QStringLiteral("叠层: layer 1, layer 2, layer 3")
                                              : QStringLiteral("叠层: layer 2")),
                            "unnamed layer title matches independent fixture IDs");
                if (stage == 2)
                    for (const auto& blockName : displayedBlocks)
                        if (auto block = view.displayed(blockName)) {
                            auto layers = block->GetCellData()->GetArray("layer_id");
                            for (vtkIdType cell = 0; cell < block->GetNumberOfCells(); ++cell)
                                require(layers && layers->GetComponent(cell, 0) == 2,
                                        "ID coloring ignored layer filter");
                        }
            });
        }
        for (bool hideBlocks : {false, true}) {
            run(qPrintable(label + (hideBlocks ? " all blocks hidden" : " absent layer")), [&] {
                ResultSession session;
                require(session.open(root.filePath("default")), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                view.setColorField(field);
                if (hideBlocks)
                    for (const auto& block : displayedBlocks)
                        view.setBlockVisible(block, false);
                else
                    view.setLayer(999);
                checkIdUnavailable(view, field);
            });
        }
        for (bool keepU : {false, true}) {
            run(qPrintable(label + (keepU ? " missing array with U" : " missing array T-only")),
                [&] {
                    QTemporaryDir temp;
                    require(temp.isValid(), "missing ID fixture directory");
                    const auto path = temp.filePath("step.vtkhdf");
                    writeFixture(root, path, [=](vtkUnstructuredGrid* grid) {
                        grid->GetCellData()->RemoveArray("layer_id");
                        grid->GetCellData()->RemoveArray("material_id");
                        if (!keepU)
                            grid->GetPointData()->RemoveArray("U");
                    });
                    ResultSession session;
                    require(session.open(path), qPrintable(session.error()));
                    ResultView view;
                    require(view.loadStep(session, 0), qPrintable(view.error()));
                    checkRange(view, ResultColorField::Temperature);
                    bool nonempty = false;
                    for (const auto& name : displayedBlocks)
                        if (auto block = view.displayed(name); block && block->GetNumberOfCells()) {
                            nonempty = true;
                            require(!block->GetCellData()->HasArray("layer_id") &&
                                        !block->GetCellData()->HasArray("material_id"),
                                    "missing ID fixture retained ID arrays");
                            require(block->GetPointData()->HasArray("T") &&
                                        bool(block->GetPointData()->HasArray("U")) == keepU,
                                    "missing ID fixture point arrays");
                        }
                    require(nonempty, "missing ID fixture is empty");
                    view.setColorField(field);
                    checkIdUnavailable(view, field);
                });
        }
        run(qPrintable(label + " clipping and repeated selection preserve coordinates"), [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "clipped ID fixture directory");
            const auto path = temp.filePath("step.vtkhdf");
            writeFixture(root, path, addMaterialIds);
            ResultSession session;
            require(session.open(path), qPrintable(session.error()));
            ResultView view;
            require(view.loadStep(session, 0), qPrintable(view.error()));
            view.setLayer(2);
            view.setClip(true, {2.5, 0, 0}, {1, 0, 0});
            require(view.error().isEmpty(), qPrintable(view.error()));
            const auto points = coordinates(view);
            for (int round = 0; round < 2; ++round) {
                view.setColorField(field);
                require(coordinates(view) == points, "ID selection moved clipped points");
                checkIdRange(view, session, field);
                const double id = field == ResultColorField::LayerId ? 2 : 20;
                checkIdBounds(view, id, id);
                view.setColorField(ResultColorField::Temperature);
                checkRange(view, ResultColorField::Temperature);
                require(coordinates(view) == points, "temperature selection moved clipped points");
            }
        });
        run(qPrintable(label + " session replacement drops old names"), [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "replacement ID fixture directory");
            const auto path = temp.filePath("step.vtkhdf");
            writeFixture(root, path, addMaterialIds);
            ResultSession named, unnamed;
            require(named.open(root.filePath("default")), qPrintable(named.error()));
            require(unnamed.open(path), qPrintable(unnamed.error()));
            ResultView view;
            require(view.loadStep(named, 0), qPrintable(view.error()));
            require(!named.layerNames().isEmpty() && !named.materialNames().isEmpty(),
                    "initial session must have names");
            view.setColorField(field);
            require(view.loadStep(unnamed, 0), qPrintable(view.error()));
            require(unnamed.layerNames().isEmpty() && unnamed.materialNames().isEmpty(),
                    "replacement session must have no names");
            require(view.colorField() == field, "session replacement lost ID field selection");
            checkIdRange(view, unnamed, field);
        });
    }
    // Unlike default (whose cells all have id 1), this fixture exercises a named multi-ID range.
    for (auto field : {ResultColorField::LayerId, ResultColorField::MaterialId})
        for (int stage : {0, 1, 2})
            run(qPrintable(QString(idArrayName(field)) + " exact named range stage " +
                           QString::number(stage)),
                [&] {
                    QTemporaryDir temp;
                    require(temp.isValid(), "named multi-ID directory");
                    const auto path = temp.filePath("step.vtkhdf");
                    writeFixture(root, path, [](vtkUnstructuredGrid* grid) {
                        addMaterialIds(grid);
                        auto materials = grid->GetCellData()->GetArray("material_id");
                        for (vtkIdType cell = 0; cell < grid->GetNumberOfCells(); ++cell)
                            materials->SetComponent(cell, 0, materials->GetComponent(cell, 0) / 10);
                    });
                    QProcess python;
                    python.start(qEnvironmentVariable("CHIPCAE_FIXTURE_PYTHON", "python3"),
                                 {"-c",
                                  QString::fromUtf8(R"(
import h5py, numpy as np, sys
with h5py.File(sys.argv[1], 'r+') as f:
    groups = []
    f.visititems(lambda name, obj: groups.append(obj) if isinstance(obj, h5py.Group) and 'CellData' in obj else None)
    assert groups, 'synthetic VTKHDF has no data blocks'
    for group in groups:
        fields = group.require_group('FieldData')
        for key, names in [('layer_names', ['Base_CASE', 'die_attach', 'Si']),
                           ('material_names', ['Organic_EQ', 'Solder', 'Si_Mat'])]:
            if key in fields:
                del fields[key]
            fields.create_dataset(key, data=np.array(names, dtype='S32'))
)"),
                                  path});
                    require(python.waitForFinished(30000) &&
                                python.exitStatus() == QProcess::NormalExit &&
                                python.exitCode() == 0,
                            qPrintable("write named fixture: " +
                                       QString::fromUtf8(python.readAllStandardError())));
                    ResultSession session;
                    require(session.open(path), qPrintable(session.error()));
                    ResultView view;
                    require(view.loadStep(session, 0), qPrintable(view.error()));
                    require(session.layerNames() == QStringList{"Base_CASE", "die_attach", "Si"} &&
                                session.materialNames() ==
                                    QStringList{"Organic_EQ", "Solder", "Si_Mat"},
                            "independent named multi-ID fixture tables");
                    if (stage >= 1)
                        view.setShowVoid(true);
                    if (stage == 2)
                        view.setLayer(2);
                    view.setColorField(field);
                    const bool layer = field == ResultColorField::LayerId;
                    const QString expected =
                        stage == 0   ? (layer ? QStringLiteral("叠层: die_attach, Si")
                                              : QStringLiteral("材料: Solder, Si_Mat"))
                        : stage == 1 ? (layer ? QStringLiteral("叠层: Base_CASE, die_attach, Si")
                                              : QStringLiteral("材料: Organic_EQ, Solder, Si_Mat"))
                                     : (layer ? QStringLiteral("叠层: die_attach")
                                              : QStringLiteral("材料: Solder"));
                    require(view.colorBarTitle() == expected,
                            qPrintable("named range title expected [" + expected + "] got [" +
                                       view.colorBarTitle() + "]"));
                    checkIdRange(view, session, field);
                    checkScalarBar(view);
                });
    for (auto field : {ResultColorField::LayerId, ResultColorField::MaterialId})
        for (bool single : {false, true})
            run(qPrintable(QString(idArrayName(field)) + " range ending at INT_MAX " +
                           (single ? "single" : "multiple")),
                [&] {
                    QTemporaryDir temp;
                    require(temp.isValid(), "INT_MAX fixture directory");
                    const auto path = temp.filePath("step.vtkhdf");
                    writeFixture(root, path, [&](vtkUnstructuredGrid* grid) {
                        vtkNew<vtkIntArray> ids;
                        ids->SetName(idArrayName(field));
                        for (vtkIdType cell = 0; cell < grid->GetNumberOfCells(); ++cell)
                            ids->InsertNextValue(std::numeric_limits<int>::max() -
                                                 (single ? 0 : static_cast<int>(cell % 2)));
                        grid->GetCellData()->AddArray(ids);
                    });
                    ResultSession session;
                    require(session.open(path), qPrintable(session.error()));
                    ResultView view;
                    require(view.loadStep(session, 0), qPrintable(view.error()));
                    view.setColorField(field);
                    checkIdBounds(view,
                                  std::numeric_limits<int>::max() - (single ? 0 : 1),
                                  std::numeric_limits<int>::max());
                    const bool layer = field == ResultColorField::LayerId;
                    const auto expected =
                        single ? (layer ? QStringLiteral("叠层: layer 2147483647")
                                        : QStringLiteral("材料: material 2147483647"))
                               : (layer ? QStringLiteral("叠层: layer 2147483646, layer 2147483647")
                                        : QStringLiteral(
                                              "材料: material 2147483646, material 2147483647"));
                    require(view.colorBarTitle() == expected,
                            "range ending at INT_MAX lists all IDs and terminates");
                    checkIdRange(view, session, field);
                    checkScalarBar(view);
                });
    run("five field repeated switching", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        const auto points = coordinates(view);
        for (int round = 0; round < 2; ++round)
            for (auto field : {ResultColorField::Temperature,
                               ResultColorField::DisplacementMagnitude,
                               ResultColorField::WarpageUz,
                               ResultColorField::LayerId,
                               ResultColorField::MaterialId,
                               ResultColorField::Temperature}) {
                view.setColorField(field);
                require(view.colorField() == field, "repeated field selection ignored");
                require(coordinates(view) == points, "five field switching moved points");
                if (field == ResultColorField::LayerId || field == ResultColorField::MaterialId)
                    checkIdRange(view, session, field);
                else {
                    checkRange(view, field);
                    require(view.colorBarTitle() ==
                                (field == ResultColorField::Temperature             ? "T (K)"
                                 : field == ResultColorField::DisplacementMagnitude ? "|U| (m)"
                                                                                    : "U_z (m)"),
                            "original field title after repeated ID switching");
                }
            }
    });
    run("independent extreme void range", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "void fixture directory");
        writeFixture(root, temp.filePath("step.vtkhdf"), [](vtkUnstructuredGrid* grid) {
            auto kinds = grid->GetCellData()->GetArray("cell_kind");
            auto t = grid->GetPointData()->GetArray("T");
            require(kinds && t, "synthetic fixture requires cell_kind and T");
            // Shared points take the lowest non-void temperature; only private void
            // points get the extreme value, independently of cell iteration order.
            std::vector<double> temperatures(grid->GetNumberOfPoints(), 1e12);
            vtkNew<vtkIdList> points;
            for (vtkIdType cell = 0; cell < grid->GetNumberOfCells(); ++cell) {
                const double kind = kinds->GetComponent(cell, 0);
                grid->GetCellPoints(cell, points);
                for (vtkIdType i = 0; i < points->GetNumberOfIds(); ++i) {
                    auto& temperature = temperatures[points->GetId(i)];
                    temperature = std::min(temperature, kind == 2 ? 1e12 : kind == 1 ? 300 : 400);
                }
            }
            for (vtkIdType i = 0; i < grid->GetNumberOfPoints(); ++i)
                t->SetComponent(i, 0, temperatures[i]);
        });
        ResultSession session;
        require(session.open(temp.filePath("step.vtkhdf")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        auto check = [&](double maximum) {
            const auto range = view.colorRange();
            require(range.valid && range.unit == "K" && range.min == 300 && range.max == maximum,
                    "void temperature contaminated independent range");
            checkRange(view, ResultColorField::Temperature);
        };
        check(400);
        view.setShowVoid(true);
        check(1e12);
        view.setShowVoid(false);
        check(400);
    });
    run("empty and mesh-only state", [&] {
        ResultView view;
        checkUnavailable(view);
        ResultSession session;
        require(session.open(root.filePath("viewer_mesh")), qPrintable(session.error()));
        require(view.loadStep(session, 0), qPrintable(view.error()));
        view.setColorField(ResultColorField::Temperature);
        checkUnavailable(view, true);
    });
    for (int components : {0, 2}) {
        run(components == 0 ? "T-only derived fields" : "two-component U derived fields", [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "missing U fixture directory");
            const auto path = temp.filePath("step.vtkhdf");
            writeFixture(root, path, [=](vtkUnstructuredGrid* grid) {
                auto original = grid->GetPointData()->GetArray("U");
                require(original && original->GetNumberOfComponents() == 3,
                        "synthetic fixture requires three-component U");
                vtkNew<vtkDoubleArray> shortU;
                shortU->SetName("U");
                shortU->SetNumberOfComponents(2);
                for (vtkIdType i = 0; i < original->GetNumberOfTuples(); ++i)
                    shortU->InsertNextTuple2(original->GetComponent(i, 0),
                                             original->GetComponent(i, 1));
                grid->GetPointData()->RemoveArray("U");
                if (components == 2)
                    grid->GetPointData()->AddArray(shortU);
            });
            ResultSession session;
            require(session.open(path), qPrintable(session.error()));
            ResultView view;
            require(view.loadStep(session, 0), qPrintable(view.error()));
            checkRange(view, ResultColorField::Temperature);
            for (auto field :
                 {ResultColorField::DisplacementMagnitude, ResultColorField::WarpageUz}) {
                view.setColorField(field);
                checkUnavailable(view);
                auto actors = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetActors();
                actors->InitTraversal();
                while (auto actor = actors->GetNextActor())
                    require(!actor->GetMapper()->GetScalarVisibility(),
                            "missing U scalar coloring");
            }
        });
    }
    for (auto field : {ResultColorField::Temperature,
                       ResultColorField::DisplacementMagnitude,
                       ResultColorField::WarpageUz}) {
        run(field == ResultColorField::Temperature             ? "Temperature"
            : field == ResultColorField::DisplacementMagnitude ? "DisplacementMagnitude"
                                                               : "WarpageUz",
            [&] {
                ResultSession session;
                require(session.open(root.filePath("default")), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                require(view.colorField() == ResultColorField::Temperature, "default field");
                view.setColorField(field);
                require(view.colorField() == field, "field selection ignored");
                require(view.colorBarTitle() == (field == ResultColorField::Temperature ? "T (K)"
                                                 : field == ResultColorField::DisplacementMagnitude
                                                     ? "|U| (m)"
                                                     : "U_z (m)"),
                        "selected color bar title missing");
                checkRange(view, field);
                const auto points = coordinates(view);
                const auto range = view.colorRange();
                view.setColorField(field);
                require(coordinates(view) == points, "repeated selection moves points");
                require(view.colorRange().min == range.min && view.colorRange().max == range.max,
                        "repeated range differs");
                checkRange(view, field);
                view.setBlockVisible("hex", false);
                checkRange(view, field, {"hex"});
                view.setBlockVisible("hex", true);
                checkRange(view, field);
                view.setLayer(1);
                checkRange(view, field);
                view.setLayer(0);
                view.setShowVoid(true);
                checkRange(view, field);
                view.setClip(true, {0.2, 0, 0}, {1, 0, 0});
                checkRange(view, field);
                for (const auto& block : displayedBlocks)
                    view.setBlockVisible(block, false);
                require(!view.colorRange().valid && view.colorBarTitle().isEmpty(),
                        "hidden blocks still contribute range");
                checkScalarBar(view);
                view.setClip(false);
                checkScalarBar(view);
                require(!view.colorRange().valid && view.colorBarTitle().isEmpty(),
                        "hidden unclipped blocks still contribute range");
            });
    }
    run("geometry invariance", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        auto original = coordinates(view);
        require(!original.empty(), "empty coordinate snapshot");
        auto switchFields = [&] {
            auto before = coordinates(view);
            for (auto field : {ResultColorField::DisplacementMagnitude,
                               ResultColorField::WarpageUz,
                               ResultColorField::Temperature}) {
                view.setColorField(field);
                require(coordinates(view) == before, "field selection changed coordinates");
            }
        };
        switchFields();
        view.setWarpageScale(100);
        require(coordinates(view) != original, "warpage scale did not move points");
        switchFields();
    });
    run("summary and controls", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        for (auto value : session.summary()) {
            auto entry = value.toObject();
            auto name = entry["name"].toString();
            if (entry["step"].toInteger() == 0 && (name == "warpage_pv" || name == "U_mag_max")) {
                auto expected = QString("%1: %2 %3")
                                    .arg(name)
                                    .arg(entry["value"].toDouble(), 0, 'g', 16)
                                    .arg(entry["unit"].toString());
                const auto method = entry["method"];
                if (method.isString() && !method.toString().isEmpty())
                    expected += QString(" [%1]").arg(method.toString());
                require(view.summary(name) == expected, "official summary regression");
            }
        }
        require(view.tmaxText() == view.summary("T_max"), "T_max does not use official summary");
        ResultControls controls(view, session);
        QComboBox* fields = nullptr;
        int magnitude = -1;
        for (auto combo : controls.findChildren<QComboBox*>()) {
            bool temperature = false, warpage = false;
            int candidate = -1;
            for (int i = 0; i < combo->count(); ++i) {
                temperature |= combo->itemText(i).contains(QStringLiteral("温度"));
                warpage |= combo->itemText(i).contains(QStringLiteral("翘曲"));
                if (combo->itemText(i).contains(QStringLiteral("位移幅值")))
                    candidate = i;
            }
            if (temperature && warpage && candidate >= 0) {
                fields = combo;
                magnitude = candidate;
            }
        }
        require(fields, "field combo with all three labels missing");
        fields->setCurrentIndex(magnitude);
        require(view.colorField() == ResultColorField::DisplacementMagnitude,
                "controls did not select magnitude");
        checkSummaryLabel(controls, view);
        view.setColorField(ResultColorField::WarpageUz);
        require(fields->currentIndex() == static_cast<int>(ResultColorField::WarpageUz),
                "programmatic field change left combo stale");
        view.setClip(true, {0, 0, 0}, {0, 0, 0});
        require(!view.colorRange().valid, "invalid clip retained range");
        bool unavailable = false;
        for (auto label : controls.findChildren<QLabel*>())
            unavailable |= label->text().contains("Field: unavailable");
        require(unavailable, "invalid clip left summary range stale");
        view.setClip(false);
        QComboBox* steps = nullptr;
        for (auto combo : controls.findChildren<QComboBox*>())
            if (combo != fields && combo->count() > 1 && combo->itemText(0) == "0" &&
                combo->itemText(1) == "1")
                steps = combo;
        require(steps, "step combo missing");
        const auto previousTmax = view.tmaxText();
        steps->setCurrentIndex(1);
        require(view.tmaxText() != previousTmax, "step combo did not load the next step");
        checkSummaryLabel(controls, view);
    });
    run("optional T_max location", [&] {
        ResultSession source;
        require(source.open(root.filePath("default")), qPrintable(source.error()));
        QTemporaryDir temp;
        require(temp.isValid(), "temporary directory");
        auto manifest = source.manifest();
        // Preserve relative step pointers and copy existing synthetic step files into the temp run.
        for (auto value : manifest["steps"].toArray()) {
            const auto relative = value.toObject()["file"].toString();
            const auto target = temp.filePath(relative);
            require(QDir().mkpath(QFileInfo(target).absolutePath()), "temporary step directory");
            if (!QFileInfo::exists(target))
                require(
                    QFile::copy(QDir(root.filePath("default")).absoluteFilePath(relative), target),
                    "temporary step copy");
        }
        auto summary = manifest["summary"].toArray();
        bool modified = false;
        for (int i = 0; i < summary.size(); ++i) {
            auto entry = summary[i].toObject();
            if (entry["name"] == "T_max" && entry["step"].toInteger() == 0) {
                entry["region"] = "die";
                entry["region_id"] = "lower-priority";
                entry["position"] = QJsonArray{1.0, 2.0, 3.0};
                entry["xyz"] = QJsonArray{4.0, 5.0, 6.0};
                entry["location"] = "die top surface";
                summary[i] = entry;
                modified = true;
            }
        }
        require(modified, "fixture T_max missing");
        manifest["summary"] = summary;
        QFile file(temp.filePath("manifest.json"));
        require(file.open(QIODevice::WriteOnly), "temporary manifest open");
        auto bytes = QJsonDocument(manifest).toJson();
        require(file.write(bytes) == bytes.size(), "temporary manifest write");
        file.close();
        ResultSession session;
        require(session.open(temp.path()), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        const auto text = view.tmaxText();
        require(text.startsWith(view.summary("T_max")),
                "optional location replaced official value");
        require(text.contains("region=die") && !text.contains("lower-priority"),
                "region precedence");
        require(text.contains(QString("at=(%1,%2,%3)")
                                  .arg(1.0, 0, 'g', 16)
                                  .arg(2.0, 0, 'g', 16)
                                  .arg(3.0, 0, 'g', 16)),
                "position formatting or precedence");
        require(!text.contains(" location="), "string location duplicated selected array");
        auto checkLocation = [&](const QJsonObject& changes, const QString& suffix) {
            auto entries = summary;
            for (int i = 0; i < entries.size(); ++i) {
                auto entry = entries[i].toObject();
                if (entry["name"] != "T_max" || entry["step"].toInteger(-1) != 0)
                    continue;
                entry.remove("position");
                entry.remove("xyz");
                entry.remove("location");
                for (auto it = changes.begin(); it != changes.end(); ++it)
                    entry[it.key()] = it.value();
                entries[i] = entry;
            }
            manifest["summary"] = entries;
            require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "rewrite manifest");
            const auto bytes = QJsonDocument(manifest).toJson();
            require(file.write(bytes) == bytes.size(), "rewrite summary");
            file.close();
            ResultSession updated;
            require(updated.open(temp.path()), qPrintable(updated.error()));
            ResultView current;
            require(current.loadStep(updated, 0), qPrintable(current.error()));
            require(current.tmaxText() == current.summary("T_max") + " region=die" + suffix,
                    "location precedence or formatting");
        };
        checkLocation({{"position", QJsonArray{1, "invalid", 3}},
                       {"xyz", QJsonArray{4, 5, 6}},
                       {"location", "surface"}},
                      " at=(4,5,6)");
        checkLocation({{"position", QJsonArray{1, 2}}, {"xyz", QJsonArray{4, 5, 6}}},
                      " at=(4,5,6)");
        checkLocation({{"position", QJsonArray{1, 2}}, {"location", "surface"}},
                      " location=surface");
        checkLocation({{"location", "surface"}}, " location=surface");
        checkLocation({{"location", QJsonArray{7, 8, 9}}}, " at=(7,8,9)");
        auto globalFirst = summary;
        globalFirst.prepend(QJsonObject{{"name", "T_max"}, {"value", -123}, {"unit", "K"}});
        manifest["summary"] = globalFirst;
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "mixed summary open");
        const auto mixedBytes = QJsonDocument(manifest).toJson();
        require(file.write(mixedBytes) == mixedBytes.size(), "mixed summary write");
        file.close();
        ResultSession mixed;
        require(mixed.open(temp.path()), qPrintable(mixed.error()));
        ResultView mixedView;
        require(mixedView.loadStep(mixed, 0), qPrintable(mixedView.error()));
        require(mixedView.tmaxText() == text, "global T_max shadows exact step");
        manifest["summary"] = QJsonArray{
            QJsonObject{{"name", "T_max"}, {"value", 987.5}, {"unit", "K"}, {"region", "global"}}};
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "global manifest open");
        const auto globalBytes = QJsonDocument(manifest).toJson();
        require(file.write(globalBytes) == globalBytes.size(), "global manifest write");
        file.close();
        ResultSession global;
        require(global.open(temp.path()), qPrintable(global.error()));
        ResultView globalView;
        for (qint64 step : {0, 1}) {
            require(globalView.loadStep(global, step), qPrintable(globalView.error()));
            require(globalView.tmaxText() == "T_max: 987.5 K region=global",
                    "global T_max missing or formatted from another entry");
        }
    });
    for (const auto& source : {QString("mock"),
                               QString("precomputed"),
                               QString("real"),
                               QString("Mock"),
                               QString("REAL"),
                               QString("invalid"),
                               QString()}) {
        for (bool degraded : {false, true}) {
            run(qPrintable("provenance " + source + (degraded ? " degraded" : "")), [&] {
                ResultSession session;
                require(session.open(root.filePath("default")), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                QJsonObject object;
                if (!source.isEmpty())
                    object = {{"schema", "chipcae.provenance/0.1"}, {"source", source}};
                if (degraded)
                    object["degraded"] = true;
                bool valid = source == "mock" || source == "precomputed" || source == "real";
                require(view.setProvenance(object) == valid, "provenance return value");
                auto banner = view.provenanceBanner();
                if (source == "real" && degraded)
                    require(banner == QStringLiteral("真实计算 — 降级数据"),
                            "degraded real banner");
                const QString watermark = source == "real" && !degraded ? ""
                                          : source == "real"            ? "降级"
                                          : source == "precomputed"     ? "预计算"
                                                                        : "MOCK";
                require(view.provenanceWatermark() == watermark, "provenance watermark");
                QLabel* bannerLabel = nullptr;
                if (source == "real" && !degraded)
                    require(banner.isEmpty(), "real banner");
                else {
                    require(banner.contains(valid ? watermark : QStringLiteral("来源未知")) &&
                                (source == "real" || !banner.contains(QStringLiteral("真实计算"))),
                            "unsafe provenance banner");
                    for (auto label : view.findChildren<QLabel*>())
                        if (label->text() == banner)
                            bannerLabel = label;
                    require(bannerLabel && !bannerLabel->isHidden() &&
                                bannerLabel->isVisibleTo(&view),
                            "provenance banner hidden");
                    require(
                        bannerLabel->findChildren<QAbstractButton*>().isEmpty() &&
                            bannerLabel->parentWidget()->findChildren<QAbstractButton*>().isEmpty(),
                        "banner close button");
                }
                require(view.setProvenance(object) == valid && view.provenanceBanner() == banner &&
                            view.provenanceWatermark() == watermark,
                        "repeated provenance changed");
                view.clearProvenance();
                require(view.provenanceBanner().isEmpty() && view.provenanceWatermark().isEmpty(),
                        "clear provenance text");
                require(!bannerLabel || bannerLabel->isHidden(), "cleared banner visible");
            });
        }
    }
    for (const auto& origin :
         {QJsonObject{{"source", "real"}},
          QJsonObject{{"source", "real"}, {"effective_source", "real"}},
          QJsonObject{{"source", "real"}, {"effective_source", "precomputed"}},
          QJsonObject{{"source", "real"}, {"effective_source", "mock"}},
          QJsonObject{{"source", "precomputed"}},
          QJsonObject{{"source", "precomputed"}, {"effective_source", "precomputed"}},
          QJsonObject{{"source", "precomputed"}, {"effective_source", "mock"}},
          QJsonObject{{"source", "mock"}},
          QJsonObject{{"source", "mock"}, {"effective_source", "mock"}}}) {
        for (const auto& degraded : {QJsonValue("true"),
                                     QJsonValue(1),
                                     QJsonValue(QJsonValue::Null),
                                     QJsonValue(QJsonArray{}),
                                     QJsonValue(QJsonObject{})}) {
            const auto name = "malformed degraded provenance " + origin["source"].toString() + "/" +
                              origin["effective_source"].toString() +
                              " type=" + QString::number(degraded.type());
            run(qPrintable(name), [&] {
                ResultSession session;
                require(session.open(root.filePath("default")), qPrintable(session.error()));
                ResultView view;
                require(view.loadStep(session, 0), qPrintable(view.error()));
                // Start clean so rejection must restore all non-real markings.
                require(view.setProvenance({{"schema", "chipcae.provenance/0.1"},
                                            {"source", "real"},
                                            {"degraded", false}}),
                        "boolean false degraded rejected");
                require(view.provenanceWatermark().isEmpty() && view.provenanceBanner().isEmpty(),
                        "boolean false degraded marked");
                auto malformed = origin;
                malformed["schema"] = "chipcae.provenance/0.1";
                malformed["degraded"] = degraded;
                require(!view.setProvenance(malformed), "non-boolean degraded accepted");
                require(view.provenanceWatermark() == "MOCK" &&
                            view.provenanceBanner().contains(QStringLiteral("来源未知")) &&
                            !view.provenanceBanner().contains(QStringLiteral("真实计算")),
                        "malformed degraded suppressed provenance markings");
                bool visibleBanner = false;
                for (auto label : view.findChildren<QLabel*>())
                    if (label->text() == view.provenanceBanner())
                        visibleBanner = !label->isHidden() && label->isVisibleTo(&view);
                require(visibleBanner, "malformed degraded banner hidden");
                int overlays = 0;
                auto props =
                    view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
                props->InitTraversal();
                while (auto prop = props->GetNextProp())
                    if (auto text = vtkTextActor::SafeDownCast(prop)) {
                        ++overlays;
                        require(text->GetVisibility() && QString(text->GetInput()) == "MOCK",
                                "malformed degraded viewport marking suppressed");
                    }
                require(overlays == 1, "malformed degraded viewport overlay missing");
                require(view.setProvenance({{"schema", "chipcae.provenance/0.1"},
                                            {"source", "real"},
                                            {"degraded", false}}),
                        "clean provenance rejected after malformed degraded");
                require(view.provenanceWatermark().isEmpty() && view.provenanceBanner().isEmpty(),
                        "clean provenance retained malformed markings");
                for (auto label : view.findChildren<QLabel*>())
                    require(label->isHidden(), "clean provenance retained malformed banner");
                props->InitTraversal();
                while (auto prop = props->GetNextProp())
                    require(!vtkTextActor::SafeDownCast(prop),
                            "clean provenance retained malformed viewport overlay");
            });
        }
    }
    run("mock real mock transition", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        QLabel* banner = nullptr;
        for (auto source : {"mock", "real", "mock"}) {
            const bool mock = QString(source) == "mock";
            require(view.setProvenance({{"schema", "chipcae.provenance/0.1"}, {"source", source}}),
                    "transition provenance rejected");
            require(view.provenanceWatermark() == (mock ? "MOCK" : ""), "transition watermark");
            require(mock ? view.provenanceBanner().contains("MOCK")
                         : view.provenanceBanner().isEmpty(),
                    "transition banner text");
            if (!banner)
                for (auto label : view.findChildren<QLabel*>())
                    if (label->text() == view.provenanceBanner())
                        banner = label;
            require(banner && banner->isHidden() != mock, "transition banner visibility");
            int count = 0;
            auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
            props->InitTraversal();
            while (auto prop = props->GetNextProp())
                if (auto text = vtkTextActor::SafeDownCast(prop)) {
                    ++count;
                    require(text->GetVisibility() && QString(text->GetInput()) == "MOCK",
                            "transition actor");
                }
            require(count == (mock ? 1 : 0), "transition renderer watermark count");
        }
    });
    run("provenance viewport and capture", [&] {
        ResultSession session;
        require(session.open(root.filePath("default")), qPrintable(session.error()));
        ResultView view;
        require(view.loadStep(session, 0), qPrintable(view.error()));
        QTemporaryDir temp;
        require(temp.isValid(), "capture temporary directory");
        const auto baselinePath = temp.filePath("baseline.png");
        require(view.capture(baselinePath), "baseline capture");
        const QImage baseline(baselinePath);
        require(!baseline.isNull(), "baseline image decode");
        for (auto object : {QJsonObject{{"source", "mock"}, {"degraded", true}},
                            QJsonObject{{"source", "precomputed"}, {"degraded", true}},
                            QJsonObject{{"source", "real"}, {"degraded", true}},
                            QJsonObject{{"source", "real"}, {"degraded", "true"}}}) {
            object["schema"] = "chipcae.provenance/0.1";
            const auto source = object["source"].toString();
            const bool malformed = !object["degraded"].isBool();
            require(view.setProvenance(object) == !malformed, "capture provenance");
            auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
            vtkTextActor* overlay = nullptr;
            props->InitTraversal();
            while (auto prop = props->GetNextProp())
                if (auto text = vtkTextActor::SafeDownCast(prop))
                    overlay = text;
            const QString expected = malformed                 ? "MOCK"
                                     : source == "precomputed" ? "PRECOMPUTED"
                                     : source == "real"        ? "DEGRADED"
                                                               : "MOCK";
            require(overlay && QString::fromUtf8(overlay->GetInput()) == expected,
                    "viewport watermark text");
            require(overlay->GetTextProperty()->GetOpacity() > 0 &&
                        overlay->GetTextProperty()->GetOpacity() < 1,
                    "watermark opacity");
            require(overlay->GetTextProperty()->GetOrientation() == 30, "diagonal watermark");
            const auto path = temp.filePath(source + (malformed ? "-malformed.png" : ".png"));
            require(view.capture(path), "watermarked capture");
            const QImage marked(path);
            require(!marked.isNull() && marked.size() == baseline.size(),
                    "watermarked image decode");
            int changed = 0;
            for (int y = 0; y < marked.height(); ++y)
                for (int x = 0; x < marked.width(); ++x)
                    changed += marked.pixel(x, y) != baseline.pixel(x, y);
            require(changed > 100, "watermark did not visibly render into capture");
        }
        view.clearProvenance();
        auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
        props->InitTraversal();
        while (auto prop = props->GetNextProp())
            require(!vtkTextActor::SafeDownCast(prop), "cleared viewport watermark");
    });
    run("effective source and schema validation", [&] {
        ResultView view;
        require(view.setProvenance({{"schema", "chipcae.provenance/0.1"},
                                    {"source", "real"},
                                    {"effective_source", "mock"}}),
                "effective source rejected");
        require(view.provenanceWatermark() == "MOCK", "effective source precedence");
        for (const auto source : {"mock", "precomputed"}) {
            require(!view.setProvenance({{"schema", "chipcae.provenance/0.1"},
                                         {"source", source},
                                         {"effective_source", "real"}}),
                    "trust upgrade accepted");
            require(view.provenanceWatermark() == "MOCK" &&
                        view.provenanceBanner().contains(QStringLiteral("来源未知")) &&
                        !view.provenanceBanner().contains(QStringLiteral("真实计算")),
                    "trust upgrade hid synthetic provenance");
        }
        for (auto object :
             {QJsonObject{{"schema", "chipcae.provenance/0.1"}, {"effective_source", "real"}},
              QJsonObject{{"schema", "chipcae.provenance/0.1"},
                          {"source", 42},
                          {"effective_source", "real"}},
              QJsonObject{{"schema", "chipcae.provenance/0.1"},
                          {"source", "real"},
                          {"effective_source", 42}},
              QJsonObject{{"source", "real"}},
              QJsonObject{{"schema", "wrong"}, {"source", "real"}},
              QJsonObject{{"schema", "chipcae.provenance/0.1"},
                          {"source", "real"},
                          {"effective_source", "REAL"}}}) {
            require(!view.setProvenance(object), "invalid provenance accepted");
            require(view.provenanceWatermark() == "MOCK" &&
                        view.provenanceBanner().contains(QStringLiteral("来源未知")),
                    "invalid provenance treated as real");
        }
    });
    return failures;
}
int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    const QDir root(argv[1]);
    int failures = 0;
    try {
        auto g = sheet();
        auto full = ResultView::quantify(g, "T");
        require(full.error.isEmpty(), qPrintable("full: " + full.error));
        near(full.volume, 6e-7, 1e-18);
        near(full.integral, 301 * 6e-7, 1e-15);
        auto half = ResultView::quantify(g, "T", true, {1, 0, 0}, {1, 0, 0});
        require(half.error.isEmpty(), qPrintable("half: " + half.error));
        near(half.volume, 3e-7, 1e-18);
        near(half.integral, 301.5 * 3e-7, 1e-15);
        auto u = ResultView::quantify(g, "U", true, {1, 0, 0}, {1, 0, 0});
        near(u.integral, 1.5 * 3e-7, 1e-17);
        auto kind = ResultView::quantify(g, "cell_kind", true, {1, 0, 0}, {1, 0, 0});
        near(kind.integral, 3e-7, 1e-18);
        auto bad = sheet();
        bad->GetPoints()->SetPoint(6, 0.5, 0.5, 0.5e-7);
        require(!ResultView::quantify(bad).error.isEmpty(), "nonconvex cell accepted");
        ResultSession s;
        for (auto variant :
             {"default", "hex_fallback", "old_names", "s1_compat", "hex_only", "particles_only"}) {
            require(s.open(root.filePath(variant)), qPrintable(s.error()));
            ResultView view;
            require(view.loadStep(s, 0), qPrintable(view.error()));
            auto hex = view.displayed("hex");
            if (hex && hex->GetNumberOfCells()) {
                auto count = hex->GetNumberOfCells();
                auto original = view.pickedId("hex", 0);
                require(original.toLongLong() > 9007199254740992LL, "gid precision lost");
                double one[3];
                hex->GetPoint(0, one);
                auto array = hex->GetPointData()->GetArray("U");
                double displacement[3];
                array->GetTuple(0, displacement);
                view.setWarpageScale(7);
                hex = view.displayed("hex");
                double seven[3];
                hex->GetPoint(0, seven);
                for (int j = 0; j < 3; ++j) {
                    near(seven[j] - one[j], 6 * displacement[j], 1e-12);
                    near(hex->GetPointData()->GetArray("U")->GetComponent(0, j), displacement[j]);
                }
                auto layers = hex->GetCellData()->GetArray("layer_id");
                if (layers) {
                    int selected = static_cast<int>(layers->GetComponent(0, 0));
                    vtkIdType expected = 0;
                    for (vtkIdType i = 0; i < count; ++i)
                        if (layers->GetComponent(i, 0) == selected)
                            ++expected;
                    view.setLayer(selected);
                    require(view.displayed("hex")->GetNumberOfCells() == expected, "layer count");
                    require(view.pickedId("hex", 0) == original, "filter gid remapped");
                    view.setLayer(999);
                    require(view.displayed("hex")->GetNumberOfCells() == 0, "layer filter ignored");
                    view.setLayer(0);
                }
                view.setBlockVisible("hex", false);
                require(view.displayed("hex")->GetNumberOfCells() == 0, "block visibility");
                view.setBlockVisible("hex", true);
                require(view.displayed("hex")->GetNumberOfCells() == count, "block restore");
                require(view.pickedId("hex", -1) == QStringLiteral("未知"), "unknown pick");
            }
            auto cut = view.displayed("cut");
            if (cut) {
                auto kinds = cut->GetCellData()->GetArray("cell_kind");
                if (kinds)
                    for (vtkIdType i = 0; i < cut->GetNumberOfCells(); ++i)
                        require(kinds->GetComponent(i, 0) != 2, "void visible");
                auto before = cut->GetNumberOfCells();
                view.setShowVoid(true);
                require(view.displayed("cut")->GetNumberOfCells() >= before, "show void count");
                view.setBlockVisible("cut", false);
                require(view.displayed("cut")->GetNumberOfCells() == 0, "cut visibility");
            }
            require(view.summary("absent").contains("unavailable"), "invented summary");
            for (auto v : s.summary()) {
                auto o = v.toObject();
                if (o["step"].toInteger() == 0) {
                    auto text = view.summary(o["name"].toString());
                    require(text.contains(o["unit"].toString()) &&
                                text.contains(QString::number(o["value"].toDouble(), 'g', 16)),
                            "summary source");
                }
            }
            ResultControls controls(view, s);
            require(view.loadStep(s, 1), qPrintable(view.error()));
            for (auto v : s.summary()) {
                auto o = v.toObject();
                if (o["step"].toInteger() == 1)
                    require(view.summary(o["name"].toString())
                                .contains(QString::number(o["value"].toDouble(), 'g', 16)),
                            "stale step summary");
            }
            for (auto name : {"eb_surface", "particles"}) {
                if (auto block = view.displayed(name)) {
                    auto count = block->GetNumberOfCells();
                    view.setBlockVisible(name, false);
                    require(view.displayed(name)->GetNumberOfCells() == 0,
                            "surface/particle visibility");
                    view.setBlockVisible(name, true);
                    require(view.displayed(name)->GetNumberOfCells() == count,
                            "surface/particle restore");
                }
            }
            view.setClip(true, {0.2, 0, 0}, {1, 0, 0});
            require(view.error().isEmpty(), qPrintable(view.error()));
        }
        require(s.open(root.filePath("viewer_mixed")), "mixed fixture");
        ResultView mixed;
        require(mixed.loadStep(s, 0), qPrintable(mixed.error()));
        require(mixed.displayed("hex")->GetNumberOfCells() == 2, "default void count");
        require(mixed.pickedId("hex", 0) == "9007199254741095", "compacted gid precision");
        mixed.setLayer(3);
        require(mixed.displayed("hex")->GetNumberOfCells() == 1, "mixed layer count");
        require(mixed.pickedId("hex", 0) == "9007199254741099", "layer compaction gid");
        mixed.renderWindow()->SetSize(800, 600);
        auto renderer = mixed.renderWindow()->GetRenderers()->GetFirstRenderer();
        renderer->ResetCamera();
        double bounds[6];
        mixed.displayed("hex")->GetBounds(bounds);
        renderer->SetWorldPoint((bounds[0] + bounds[1]) / 2,
                                (bounds[2] + bounds[3]) / 2,
                                (bounds[4] + bounds[5]) / 2,
                                1);
        renderer->WorldToDisplay();
        auto pixel = renderer->GetDisplayPoint();
        require(mixed.pick(static_cast<int>(pixel[0]), static_cast<int>(pixel[1])) ==
                    "9007199254741099",
                "screen pick gid");
        mixed.setLayer(0);
        mixed.setShowVoid(true);
        require(mixed.displayed("hex")->GetNumberOfCells() == 3, "raw void was removed");
        require(mixed.summary("warpage_pv") == "warpage_pv: 12345 m",
                "computed summary substituted");
        mixed.setLayer(2);
        mixed.setClip(true, {2.5, 0, 0}, {1, 0, 0});
        require(mixed.error().isEmpty(), qPrintable(mixed.error()));
        auto clipped = mixed.displayed("hex");
        require(clipped->GetNumberOfCells() > 0, "clip empty");
        require(clipped->GetPointData()->HasArray("T") && clipped->GetPointData()->HasArray("U"),
                "clip fields lost");
        for (vtkIdType i = 0; i < clipped->GetNumberOfCells(); ++i)
            require(mixed.pickedId("hex", i) == "9007199254741095", "clip gid lost");
        require(s.open(root.filePath("viewer_mesh")), "mesh fixture");
        ResultView mesh;
        require(mesh.loadStep(s, 0), "mesh only load");
        require(mesh.pickedId("hex", 0) == QStringLiteral("未知"), "local id substituted");
        mesh.setWarpageScale(100);
        require(mesh.summary("warpage_pv").contains("unavailable"), "mesh invented summary");
        require(s.open(root.filePath("temporal_ug.vtkhdf")), "standalone");
        ResultView legacy;
        require(legacy.loadStep(s, 0), qPrintable(legacy.error()));
        require(legacy.summary("warpage_pv").contains("unavailable"), "missing summary fallback");
        std::cout << "existing viewer checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL legacy viewer checks: " << e.what() << '\n';
        ++failures;
    }
    failures += fvmNoticeAcceptance(root);
    failures += methodSummaryAcceptance(root);
    failures += acceptance(root);
    failures += obliqueAndTemperatureAcceptance(root);
    return failures ? 1 : 0;
}
