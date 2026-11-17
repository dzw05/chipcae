#include "ResultSession.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cmath>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vtkActor.h>
#include <vtkCell.h>
#include <vtkDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkEGLRenderWindow.h>
#include <vtkNew.h>
#include <vtkPartitionedDataSet.h>
#include <vtkPartitionedDataSetCollection.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkVersion.h>
#include <vtkXOpenGLRenderWindow.h>
using chipcae::desktop::ResultSession;
void require(bool ok, const QString& message) {
    if (!ok)
        throw std::runtime_error(message.toStdString());
}
vtkDataSet* first(vtkDataObject* object) {
    if (auto pdc = vtkPartitionedDataSetCollection::SafeDownCast(object))
        return pdc->GetPartitionedDataSet(0)->GetPartition(0);
    if (auto pd = vtkPartitionedDataSet::SafeDownCast(object))
        return pd->GetPartition(0);
    return vtkDataSet::SafeDownCast(object);
}
void topology(vtkDataSet* data) {
    require(data != nullptr, "missing cut data");
    for (vtkIdType c = 0; c < data->GetNumberOfCells(); ++c) {
        auto cell = data->GetCell(c);
        if (cell->GetCellType() != VTK_POLYHEDRON)
            continue;
        double center[3] = {};
        for (vtkIdType i = 0; i < cell->GetNumberOfPoints(); ++i) {
            double p[3];
            cell->GetPoints()->GetPoint(i, p);
            for (int j = 0; j < 3; ++j)
                center[j] += p[j] / cell->GetNumberOfPoints();
        }
        std::map<std::pair<vtkIdType, vtkIdType>, int> edges;
        for (int f = 0; f < cell->GetNumberOfFaces(); ++f) {
            auto face = cell->GetFace(f);
            require(face->GetNumberOfPoints() >= 3, "short face");
            double a[3], b[3], d[3], fc[3] = {};
            face->GetPoints()->GetPoint(0, a);
            face->GetPoints()->GetPoint(1, b);
            face->GetPoints()->GetPoint(2, d);
            for (vtkIdType i = 0; i < face->GetNumberOfPoints(); ++i) {
                double p[3];
                face->GetPoints()->GetPoint(i, p);
                for (int j = 0; j < 3; ++j)
                    fc[j] += p[j] / face->GetNumberOfPoints();
                edges[{face->GetPointId(i),
                       face->GetPointId((i + 1) % face->GetNumberOfPoints())}]++;
            }
            double ab[3], ad[3];
            for (int j = 0; j < 3; ++j) {
                ab[j] = b[j] - a[j];
                ad[j] = d[j] - a[j];
            }
            double normal[3] = {ab[1] * ad[2] - ab[2] * ad[1],
                                ab[2] * ad[0] - ab[0] * ad[2],
                                ab[0] * ad[1] - ab[1] * ad[0]};
            double dot = 0;
            for (int j = 0; j < 3; ++j)
                dot += normal[j] * (fc[j] - center[j]);
            require(dot > 0, "face must point outward");
        }
        for (auto [edge, count] : edges)
            require(count == 1 && edges[{edge.second, edge.first}] == 1, "face rings not closed");
    }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QDir root(argv[1]);
        ResultSession s;
        for (auto variant : {"default",
                             "hex_fallback",
                             "old_names",
                             "s1_compat",
                             "variable_parts",
                             "hex_only",
                             "particles_only"}) {
            require(s.open(root.filePath(variant)), s.error());
            double initial = 0;
            for (int n : {0, 1, 2, 0}) {
                auto object = s.loadStep(n, "T");
                require(object != nullptr, s.error());
                auto ds = first(object);
                require(ds != nullptr, "missing partition");
                if (ds->GetNumberOfPoints()) {
                    auto value = ds->GetPointData()->GetArray("T")->GetComponent(0, 0);
                    if (n == 0) {
                        if (initial)
                            require(value == initial, "failed return to step zero");
                        initial = value;
                    } else
                        require(value != initial, "stale field cache");
                }
                if (QString(variant) == "default") {
                    auto pdc = vtkPartitionedDataSetCollection::SafeDownCast(object);
                    topology(pdc->GetPartitionedDataSet(1)->GetPartition(0));
                    require(!s.nameTables().isEmpty(), "fixed width names absent");
                }
            }
        }
        require(s.open(root.filePath("default/out/step-0.vtkhdf"),
                       root.filePath("default/manifest.json")),
                s.error());
        auto selected = s.loadStep(1, "T");
        require(selected != nullptr, s.error());
        require(first(selected)->GetPointData()->GetArray("T")->GetComponent(0, 0) > 200,
                "sidecar step selection ignored");
        require(s.open(root.filePath("empty")), s.error());
        require(s.steps().isEmpty(), "empty steps");
        require(!s.loadStep(0), "empty must not produce dataset");
        require(s.open(root.filePath("mid_step")), s.error());
        require(!s.loadStep(0) && s.error().isEmpty(), "mid step should wait");
        require(s.open(root.filePath("reject_negative.vtkhdf")), s.error());
        require(!s.loadStep(0) && s.error().contains("negative PolyhedronToFaces"),
                "negative preflight failed");
        require(!s.open(root.filePath("unsupported")) &&
                    s.error().contains("unsupported event_log_format"),
                "format accepted");
        require(!s.open(root.filePath("escape")) && s.error().contains("escapes"),
                "path escape accepted");
        require(s.open(root.filePath("legacy_summary")), s.error());
        require(s.warnings().join(' ').contains("U_abs_max"), "missing alias warning");
        for (auto v : s.summary())
            require(v.toObject()["name"] != "U_abs_max", "legacy summary not normalized");
        require(s.open(root.filePath("preferred_summary")), s.error());
        for (auto v : s.summary())
            require(v.toObject()["value"].toDouble() != -1, "legacy overrode official");
        require(s.open(root.filePath("default")), s.error());
        require(!s.warnings().join(' ').contains("U_mag_max"), "official key warned");
        for (auto file : {"valid", "bad-crc", "half"}) {
            auto log =
                ResultSession::readEventLog(root.filePath(QString("events/%1.caelog").arg(file)));
            require(log.events.size() == (QString(file) == "valid" ? 2 : 1), "event prefix count");
            require(log.error.isEmpty() == (QString(file) == "valid"),
                    "bad tail silently accepted");
        }
        require(
            !ResultSession::readEventLog(root.filePath("events/valid.caelog"), 2).error.isEmpty(),
            "format accepted");
        require(s.open(root.filePath("temporal_ug.vtkhdf")), s.error());
        require(s.loadStep(0) != nullptr, s.error());
        require(s.open(root.filePath("bad_checksum")), s.error());
        require(!s.loadStep(0) && s.error().contains("checksum"), "checksum accepted");
        require(s.open(root.filePath("digest")), s.error());
        require(s.meshDigest()["nx0"].toInt() == 4 &&
                    s.meshDigest()["mesh_sha256"].toString().size() == 64,
                "mesh digest lost");
        require(s.open(root.filePath("temporal.json")), s.error());
        double base = 0;
        for (int n : {0, 1, 2, 0}) {
            auto data = s.loadStep(n);
            require(data != nullptr, s.error());
            auto value = first(data)->GetPointData()->GetArray("T")->GetComponent(0, 0);
            if (n == 0) {
                if (base)
                    require(base == value, "temporal return failed");
                base = value;
            } else
                require(value != base, "temporal offsets ignored");
        }
        for (auto name : {"legacy", "future", "wrong_major"}) {
            require(s.open(root.filePath(QString(name) + ".vtkhdf")), s.error());
            auto data = s.loadStep(0);
            if (QString(name) == "wrong_major")
                require(!data && s.error().contains("Version"), "major version accepted");
            else
                require(data != nullptr && !s.warnings().isEmpty(), s.error());
        }
        require(s.open(root.filePath("type_multi.vtkhdf")), s.error());
        require(!s.loadStep(0) && s.error().contains("Type"), "multi-value Type accepted");
        QString s1;
        for (const auto& candidate : {QString::fromUtf8(qgetenv("CHIPCAE_S1")),
                                      QStringLiteral("/workspace/samples/s1"),
                                      root.filePath("s1")}) {
            if (!candidate.isEmpty() && QFileInfo(candidate).exists()) {
                s1 = candidate;
                break;
            }
        }
        if (s1.isEmpty()) {
            std::cout << "S1 fixture not present; skipping official S1 readback\n";
            return 0;
        }
        require(s.open(s1), s.error());
        auto object = s.loadStep(0, "T");
        require(object != nullptr, s.error());
        auto pdc = vtkPartitionedDataSetCollection::SafeDownCast(object);
        require(pdc != nullptr, "S1 PDC missing");
        topology(pdc->GetPartitionedDataSet(1)->GetPartition(0));
        auto ds = first(object);
        require(ds && ds->GetNumberOfCells() > 0, "S1 empty");
        vtkSmartPointer<vtkRenderWindow> window;
        if (qEnvironmentVariableIsSet("DISPLAY") && !qgetenv("DISPLAY").isEmpty())
            window = vtkSmartPointer<vtkXOpenGLRenderWindow>::New();
        else
            window = vtkSmartPointer<vtkEGLRenderWindow>::New();
        vtkNew<vtkDataSetMapper> mapper;
        mapper->SetInputData(ds);
        vtkNew<vtkActor> actor;
        actor->SetMapper(mapper);
        vtkNew<vtkRenderer> renderer;
        renderer->AddActor(actor);
        renderer->ResetCamera();
        window->SetOffScreenRendering(1);
        window->SetSize(128, 128);
        window->AddRenderer(renderer);
        if (!window->SupportsOpenGL()) {
            std::cout << "S1 IO OK; OpenGL unavailable, skip render\n";
            return 0;
        }
        window->Render();
        require(window->GetNeverRendered() == 0, "S1 render failed");
        std::cout << "VTK " << vtkVersion::GetVTKVersion() << " S1 cells=" << ds->GetNumberOfCells()
                  << " points=" << ds->GetNumberOfPoints() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
