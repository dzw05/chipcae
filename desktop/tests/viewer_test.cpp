#include "ResultControls.hpp"
#include "ResultView.hpp"

#include <QApplication>
#include <QDir>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vtkCellData.h>
#include <vtkDataSet.h>
#include <vtkDoubleArray.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
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
int main(int argc, char** argv) {
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
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
        QDir root(argv[1]);
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
        std::cout << "viewer checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
