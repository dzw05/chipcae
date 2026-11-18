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
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkCellData.h>
#include <vtkCompositeDataIterator.h>
#include <vtkCompositeDataSet.h>
#include <vtkDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkDoubleArray.h>
#include <vtkHDFWriter.h>
#include <vtkIdList.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPropCollection.h>
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
void checkRange(ResultView& view, ResultColorField field) {
    checkScalarBar(view);
    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    for (const auto& name : displayedBlocks) {
        auto block = view.displayed(name);
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
void checkUnavailable(ResultView& view) {
    checkScalarBar(view);
    const auto range = view.colorRange();
    require(!range.valid && range.min == 0 && range.max == 0 && range.unit.isEmpty(),
            "invalid range defaults");
    require(view.colorBarTitle().isEmpty(), "unavailable field title");
    require(view.tmaxText() == "T_max: unavailable", "unavailable official T_max");
    require(view.error().isEmpty(), "unavailable field set error");
}
void checkSummaryLabel(ResultControls& controls, ResultView& view) {
    auto range = view.colorRange();
    bool found = false;
    for (auto label : controls.findChildren<QLabel*>()) {
        const auto text = label->text();
        found |=
            text.contains(view.tmaxText()) && text.contains(QString::number(range.min, 'g', 16)) &&
            text.contains(QString::number(range.max, 'g', 16)) &&
            text.contains(view.summary("warpage_pv")) && text.contains(view.summary("U_mag_max"));
    }
    require(found, "controls summary missing official T_max, range or existing QoIs");
}
// Read through the production session and write a temporary VTKHDF fixture. Mutations
// use VTK arrays and cell connectivity rather than HDF paths or point ordering.
template <typename Mutate>
void writeFixture(const QDir& root, const QString& path, Mutate mutate) {
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
    vtkNew<vtkHDFWriter> writer;
    writer->SetFileName(path.toUtf8().constData());
    writer->SetInputData(data);
    require(writer->Write() == 1, "write temporary VTKHDF fixture");
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
    run("independent extreme void range", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "void fixture directory");
        writeFixture(root, temp.filePath("step.vtkhdf"), [](vtkUnstructuredGrid* grid) {
            auto kinds = grid->GetCellData()->GetArray("cell_kind");
            auto t = grid->GetPointData()->GetArray("T");
            require(kinds && t, "synthetic fixture requires cell_kind and T");
            vtkNew<vtkIdList> points;
            for (vtkIdType cell = 0; cell < grid->GetNumberOfCells(); ++cell) {
                const double kind = kinds->GetComponent(cell, 0);
                grid->GetCellPoints(cell, points);
                for (vtkIdType i = 0; i < points->GetNumberOfIds(); ++i)
                    t->SetComponent(points->GetId(i), 0, kind == 2 ? 1e12 : kind == 1 ? 300 : 400);
            }
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
        checkUnavailable(view);
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
                checkRange(view, field);
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
            if (entry["step"].toInteger() == 0 && (name == "warpage_pv" || name == "U_mag_max"))
                require(view.summary(name) == QString("%1: %2 %3")
                                                  .arg(name)
                                                  .arg(entry["value"].toDouble(), 0, 'g', 16)
                                                  .arg(entry["unit"].toString()),
                        "official summary regression");
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
        for (auto combo : controls.findChildren<QComboBox*>())
            if (combo != fields && combo->count() > 1 && combo->itemText(0) == "0" &&
                combo->itemText(1) == "1") {
                combo->setCurrentIndex(1);
                checkSummaryLabel(controls, view);
            }
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
        for (auto source : {"mock", "precomputed", "real"}) {
            require(
                view.setProvenance(
                    {{"schema", "chipcae.provenance/0.1"}, {"source", source}, {"degraded", true}}),
                "capture provenance");
            auto props = view.renderWindow()->GetRenderers()->GetFirstRenderer()->GetViewProps();
            vtkTextActor* overlay = nullptr;
            props->InitTraversal();
            while (auto prop = props->GetNextProp())
                if (auto text = vtkTextActor::SafeDownCast(prop))
                    overlay = text;
            const QString expected = QString(source) == "precomputed" ? "PRECOMPUTED"
                                     : QString(source) == "real"      ? "DEGRADED"
                                                                      : "MOCK";
            require(overlay && QString::fromUtf8(overlay->GetInput()) == expected,
                    "viewport watermark text");
            require(overlay->GetTextProperty()->GetOpacity() > 0 &&
                        overlay->GetTextProperty()->GetOpacity() < 1,
                    "watermark opacity");
            require(overlay->GetTextProperty()->GetOrientation() == 30, "diagonal watermark");
            const auto path = temp.filePath(QString(source) + ".png");
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
    failures += acceptance(root);
    return failures ? 1 : 0;
}
