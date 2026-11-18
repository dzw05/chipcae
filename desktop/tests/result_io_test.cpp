#include "ResultSession.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vtkActor.h>
#include <vtkCell.h>
#include <vtkCellData.h>
#include <vtkDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkEGLRenderWindow.h>
#include <vtkFieldData.h>
#include <vtkNew.h>
#include <vtkPartitionedDataSet.h>
#include <vtkPartitionedDataSetCollection.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkStringArray.h>
#include <vtkVersion.h>
#include <vtkXOpenGLRenderWindow.h>
using chipcae::desktop::MeshSha256Compare;
using chipcae::desktop::ResultSession;
void require(bool ok, const QString& message) {
    if (!ok)
        throw std::runtime_error(message.toStdString());
}
namespace {
void copyResultFixture(const QDir& source, const QDir& destination) {
    require(source.exists(), "digest fixture directory missing");
    require(QDir().mkpath(destination.absolutePath()), "cannot create fixture copy directory");
    for (const auto& entry :
         source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const auto target = destination.filePath(entry.fileName());
        if (entry.isDir())
            copyResultFixture(QDir(entry.absoluteFilePath()), QDir(target));
        else
            require(QFile::copy(entry.absoluteFilePath(), target), "cannot copy " + target);
    }
}

void withDigestManifest(const QDir& source,
                        const std::function<void(QJsonObject&)>& edit,
                        const std::function<void(const ResultSession&)>& verify) {
    QTemporaryDir temporary;
    require(temporary.isValid(), "cannot create temporary digest fixture");
    const QDir copy(QDir(temporary.path()).filePath("digest"));
    copyResultFixture(source, copy);
    QFile manifest(copy.filePath("manifest.json"));
    require(manifest.open(QIODevice::ReadOnly), "cannot read copied manifest");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parse);
    require(manifest.error() == QFileDevice::NoError, "cannot read complete copied manifest");
    require(parse.error == QJsonParseError::NoError && document.isObject(),
            "copied manifest is not a JSON object");
    manifest.close();
    auto object = document.object();
    edit(object);
    const auto bytes = QJsonDocument(object).toJson();
    require(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "cannot rewrite copied manifest");
    require(manifest.write(bytes) == bytes.size(), "incomplete copied manifest write");
    require(manifest.flush(), "cannot flush copied manifest");
    manifest.close();
    ResultSession session;
    const bool opened = session.open(copy.absolutePath());
    require(opened, "modified digest fixture must open: " + session.error());
    verify(session);
}

void requireComparison(const ResultSession& session,
                       const QString& preview,
                       MeshSha256Compare expected,
                       const QString& manifestSha = {}) {
    QString detail = "old detail must be overwritten";
    const auto actual = session.comparePreviewMeshSha256(preview, &detail);
    require(actual == expected,
            QString("unexpected mesh SHA-256 comparison status: expected %1, got %2")
                .arg(static_cast<int>(expected))
                .arg(static_cast<int>(actual)));
    if (expected == MeshSha256Compare::Match) {
        require(detail.isEmpty(), "matching digest must clear detail");
    } else {
        require(!detail.isEmpty(), "non-match digest must provide detail");
        if (expected == MeshSha256Compare::PreviewMissing)
            require(detail.contains("preview"), "missing preview detail must contain preview");
        else if (expected == MeshSha256Compare::ManifestMissing)
            require(detail.contains("manifest"), "missing manifest detail must contain manifest");
        else
            require(detail.contains(preview.trimmed()) && detail.contains(manifestSha),
                    "mismatch detail must contain both trimmed hashes");
    }
    QString repeated = "different stale detail";
    require(session.comparePreviewMeshSha256(preview, &repeated) == expected && repeated == detail,
            "repeated comparison must preserve status and overwrite detail deterministically");
    require(session.comparePreviewMeshSha256(preview, nullptr) == expected,
            "comparison with nullptr detail must preserve status");
    require(session.comparePreviewMeshSha256(preview) == expected,
            "comparison with omitted detail must preserve status");
}

void meshSha256Comparison(const ResultSession& session, const QDir& fixture) {
    const QString sha = session.meshDigest().value("mesh_sha256").toString();
    require(sha.size() == 64, "digest fixture must contain a 64-character SHA-256");
    requireComparison(session, sha, MeshSha256Compare::Match);
    requireComparison(session, sha + " ", MeshSha256Compare::Match);
    requireComparison(session, " \t" + sha + "\n", MeshSha256Compare::Match);

    QString mutated = sha;
    mutated[0] = sha[0] == QChar('a') ? QChar('b') : QChar('a');
    requireComparison(session, mutated, MeshSha256Compare::Mismatch, sha);
    requireComparison(session, " \t" + mutated + "\n", MeshSha256Compare::Mismatch, sha);

    QString internalSpace(64, 'a');
    internalSpace[31] = ' ';
    const QStringList invalidPreviews{"",
                                      " \t\n",
                                      QString(63, 'a'),
                                      QString(65, 'a'),
                                      QString(64, 'A'),
                                      QString(64, 'g'),
                                      "0x" + sha,
                                      internalSpace};
    for (const auto& preview : invalidPreviews)
        requireComparison(session, preview, MeshSha256Compare::PreviewMissing);

    ResultSession unopened;
    requireComparison(unopened, sha, MeshSha256Compare::ManifestMissing);
    requireComparison(unopened, "", MeshSha256Compare::PreviewMissing);

    withDigestManifest(
        fixture,
        [](QJsonObject& manifest) {
            auto mesh = manifest.value("mesh").toObject();
            mesh.remove("mesh_sha256");
            manifest["mesh"] = mesh;
            manifest.remove("mesh_sha256");
        },
        [&](const ResultSession& missing) {
            requireComparison(missing, sha, MeshSha256Compare::ManifestMissing);
            for (const auto& preview : invalidPreviews)
                requireComparison(missing, preview, MeshSha256Compare::PreviewMissing);
        });

    const QJsonArray invalidManifestHashes{"",
                                           " \t\n",
                                           QString(63, 'a'),
                                           QString(65, 'a'),
                                           QString(64, 'A'),
                                           QString(64, 'g'),
                                           "0x" + sha,
                                           internalSpace,
                                           QJsonValue(QJsonValue::Null),
                                           42};
    for (const auto& invalid : invalidManifestHashes) {
        withDigestManifest(
            fixture,
            [&](QJsonObject& manifest) {
                auto mesh = manifest.value("mesh").toObject();
                mesh["mesh_sha256"] = invalid;
                manifest["mesh"] = mesh;
                manifest.remove("mesh_sha256");
            },
            [&](const ResultSession& invalidManifest) {
                requireComparison(invalidManifest, sha, MeshSha256Compare::ManifestMissing);
            });
    }

    // The existing merge falls back only for absent nested keys, including non-string values.
    for (const auto& invalid : invalidManifestHashes) {
        withDigestManifest(
            fixture,
            [&](QJsonObject& manifest) {
                auto mesh = manifest.value("mesh").toObject();
                mesh["mesh_sha256"] = invalid;
                manifest["mesh"] = mesh;
                manifest["mesh_sha256"] = sha;
            },
            [&](const ResultSession& invalidNested) {
                requireComparison(invalidNested, sha, MeshSha256Compare::ManifestMissing);
            });
    }

    for (const auto& fallback : {sha, " \t" + sha + "\n"}) {
        withDigestManifest(
            fixture,
            [&](QJsonObject& manifest) {
                auto mesh = manifest.value("mesh").toObject();
                mesh.remove("mesh_sha256");
                manifest["mesh"] = mesh;
                manifest["mesh_sha256"] = fallback;
            },
            [&](const ResultSession& topLevel) {
                requireComparison(topLevel, sha, MeshSha256Compare::Match);
            });
    }
    withDigestManifest(
        fixture,
        [&](QJsonObject& manifest) {
            auto mesh = manifest.value("mesh").toObject();
            mesh["mesh_sha256"] = " \t" + sha + "\n";
            manifest["mesh"] = mesh;
        },
        [&](const ResultSession& padded) {
            requireComparison(padded, sha, MeshSha256Compare::Match);
        });
    withDigestManifest(
        fixture,
        [&](QJsonObject& manifest) {
            auto mesh = manifest.value("mesh").toObject();
            mesh["mesh_sha256"] = mutated;
            manifest["mesh"] = mesh;
            manifest["mesh_sha256"] = sha;
        },
        [&](const ResultSession& nested) {
            requireComparison(nested, mutated, MeshSha256Compare::Match);
            requireComparison(nested, sha, MeshSha256Compare::Mismatch, mutated);
        });
    withDigestManifest(
        fixture,
        [](QJsonObject& manifest) {
            auto mesh = manifest.value("mesh").toObject();
            for (const auto* key : {"nx0", "ny0", "L", "padded_cells", "nz"})
                mesh[key] = mesh.value(key).toInt() + 1;
            manifest["mesh"] = mesh;
        },
        [&](const ResultSession& dimensionsChanged) {
            requireComparison(dimensionsChanged, sha, MeshSha256Compare::Match);
        });

    QString missingDetail = "previous detail";
    require(session.comparePreviewMeshSha256("", &missingDetail) ==
                MeshSha256Compare::PreviewMissing,
            "missing preview must report PreviewMissing");
    QString detail = "stale detail";
    require(session.comparePreviewMeshSha256(mutated, &detail) == MeshSha256Compare::Mismatch &&
                !detail.isEmpty() && detail.contains(sha) && detail.contains(mutated),
            "mismatch must overwrite detail with both hashes");
    require(session.comparePreviewMeshSha256(sha, &detail) == MeshSha256Compare::Match &&
                detail.isEmpty(),
            "match after mismatch must clear detail");
    require(session.comparePreviewMeshSha256("", &detail) == MeshSha256Compare::PreviewMissing &&
                !detail.isEmpty() && detail.contains("preview") && detail == missingDetail,
            "missing preview after match must replace detail rather than append");
}
} // namespace
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
                    require(s.layerName(1) == "substrate" && s.layerName(2) == "bump_eq" &&
                                s.layerName(3) == "die_layer",
                            "fixed-width layer id map");
                    require(s.materialName(1) == "organic_eq" && s.materialName(4) == "silicon",
                            "fixed-width material id map");
                    require(s.layerName(0).isEmpty() && s.layerName(99).isEmpty(),
                            "out-of-range id");
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
        meshSha256Comparison(s, QDir(root.filePath("digest")));
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
        require(s.open(root.filePath("name_vlen.vtkhdf")), s.error());
        auto vlen = s.loadStep(0);
        require(vlen != nullptr, s.error());
        require(s.layerName(1) == "substrate" && s.layerName(2) == "die_attach" &&
                    s.layerName(3) == "Si",
                "variable-length layer id map");
        require(s.materialName(1) == "substrate" && s.materialName(3) == "Si",
                "variable-length material id map");
        require(first(vlen)->GetFieldData() &&
                    first(vlen)->GetFieldData()->GetAbstractArray("layer_names"),
                "name tables attached to partition FieldData");
        QString s1b;
        for (const auto& candidate : {QString::fromUtf8(qgetenv("CHIPCAE_S1B")),
                                      QStringLiteral("/workspace/samples/s1b")}) {
            if (!candidate.isEmpty() && QFileInfo(candidate).exists()) {
                s1b = candidate;
                break;
            }
        }
        if (s1b.isEmpty()) {
            std::cout << "s1b fixture not present; skipped official variable-length name tables\n";
        } else {
            require(s.open(s1b), s.error());
            auto official = s.loadStep(0, "T");
            require(official != nullptr, s.error());
            require(s.layerName(1) == "substrate" && s.layerName(2) == "die_attach" &&
                        s.layerName(3) == "Si",
                    "s1b layer id map");
            require(s.materialName(1) == "substrate" && s.materialName(2) == "die_attach" &&
                        s.materialName(3) == "Si",
                    "s1b material id map");
            auto hex = first(official);
            require(hex && hex->GetCellData() && hex->GetCellData()->GetArray("layer_id"),
                    "s1b layer_id missing");
            auto* ids = hex->GetCellData()->GetArray("layer_id");
            require(s.layerName(static_cast<int>(ids->GetComponent(0, 0))) ==
                        s.layerName(static_cast<int>(ids->GetComponent(0, 0))),
                    "s1b cell id maps to a name");
            const auto firstId = static_cast<int>(ids->GetComponent(0, 0));
            require(!s.layerName(firstId).isEmpty(), "s1b first cell layer id has no name");
            require(hex->GetFieldData() && hex->GetFieldData()->GetAbstractArray("layer_names"),
                    "s1b names attached after VTK read");
            std::cout << "s1b names: " << s.layerName(1).toStdString() << ","
                      << s.layerName(2).toStdString() << "," << s.layerName(3).toStdString()
                      << "\n";
        }
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
