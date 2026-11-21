#include "ResultSession.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
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
void copyEventManifest(const QDir& root, const QTemporaryDir& temporary) {
    require(temporary.isValid(), "cannot create temporary event-discovery fixture");
    const auto target = QDir(temporary.path()).filePath("manifest.json");
    require(QFile::copy(root.filePath("default/manifest.json"), target),
            "cannot copy event-discovery manifest to " + target);
}

void copyEventLog(const QDir& root,
                  const QString& source,
                  const QDir& target,
                  const QString& name) {
    require(QFile::copy(root.filePath("events/" + source + ".caelog"), target.filePath(name)),
            "cannot copy event fixture to " + target.filePath(name));
}

void requireSameEventLog(const chipcae::desktop::EventLogResult& actual,
                         const chipcae::desktop::EventLogResult& expected) {
    require(actual.events.size() == expected.events.size(),
            QString("eventLog().events.size(): expected %1, got %2")
                .arg(expected.events.size())
                .arg(actual.events.size()));
    require(actual.validatedBytes == expected.validatedBytes, "event validatedBytes mismatch");
    require(actual.error.toUtf8() == expected.error.toUtf8(), "event error bytes mismatch");
}

void requireEventOpen(ResultSession& session,
                      const QString& input,
                      const QString& explicitLog = {}) {
    const bool opened =
        explicitLog.isEmpty() ? session.open(input) : session.open(input, {}, explicitLog);
    require(opened, "event-discovery open failed: " + session.error());
    require(session.error().isEmpty(), "event discovery must not set a session error");
}

void requireEmptyEventLog(const ResultSession& session, const QStringList& baselineWarnings) {
    require(session.eventLog().events.empty(), "absent event log must have no events");
    require(session.eventLog().validatedBytes == 0, "absent event log must validate zero bytes");
    require(session.eventLog().error.isEmpty(), "absent event log must have no decoder error");
    require(session.error().isEmpty(), "absent event log must have no session error");
    require(session.warnings() == baselineWarnings,
            "absent event log must preserve manifest warnings");
}

QStringList eventLogDiscovery(const QDir& root) {
    // Pin the decoder fixtures independently of open's filename selection.
    const auto valid = ResultSession::readEventLog(root.filePath("events/valid.caelog"));
    const auto half = ResultSession::readEventLog(root.filePath("events/half.caelog"));
    const auto badCrc = ResultSession::readEventLog(root.filePath("events/bad-crc.caelog"));
    require(valid.events.size() == 2 && valid.error.isEmpty(), "invalid valid-event reference");
    require(half.events.size() == 1 && !half.error.isEmpty(), "invalid half-event reference");
    require(badCrc.events.size() == 1 && !badCrc.error.isEmpty(), "invalid CRC-event reference");

    // Copy only the manifest: default's generated solver log must not leak into these cases.
    QTemporaryDir manifestOnly;
    copyEventManifest(root, manifestOnly);
    const QDir empty(manifestOnly.path());
    require(!QFileInfo::exists(empty.filePath("events.bin")) &&
                !QFileInfo::exists(empty.filePath("events.caelog")),
            "manifest-only fixture unexpectedly contains a default log");
    ResultSession baseline;
    requireEventOpen(baseline, manifestOnly.path());
    const auto baselineWarnings = baseline.warnings();

    QStringList failures;
    const auto run = [&](const QString& name, const std::function<void()>& test) {
        try {
            test();
            std::cout << "event discovery PASS: " << name.toStdString() << '\n';
        } catch (const std::exception& error) {
            const auto failure = name + ": " + QString::fromUtf8(error.what());
            failures << failure;
            std::cerr << "event discovery FAIL: " << failure.toStdString() << '\n';
        }
    };

    for (const bool manifestInput : {false, true}) {
        run(manifestInput ? "bin only, manifest input" : "bin only, directory input", [&] {
            QTemporaryDir temporary;
            copyEventManifest(root, temporary);
            const QDir directory(temporary.path());
            copyEventLog(root, "valid", directory, "events.bin");
            require(!QFileInfo::exists(directory.filePath("events.caelog")),
                    "bin-only fixture contains events.caelog");
            ResultSession session;
            requireEventOpen(
                session, manifestInput ? directory.filePath("manifest.json") : temporary.path());
            require(session.eventLog().error.isEmpty(), "valid bin must have no decoder error");
            requireSameEventLog(session.eventLog(), valid);
            require(session.warnings() == baselineWarnings, "valid bin must not add warnings");
        });
    }
    run("caelog only", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        const QDir directory(temporary.path());
        copyEventLog(root, "valid", directory, "events.caelog");
        require(!QFileInfo::exists(directory.filePath("events.bin")),
                "caelog-only fixture contains events.bin");
        ResultSession session;
        requireEventOpen(session, temporary.path());
        requireSameEventLog(session.eventLog(), valid);
        require(session.warnings() == baselineWarnings, "valid caelog must not add warnings");
    });
    run("both present, bin wins", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        const QDir directory(temporary.path());
        copyEventLog(root, "valid", directory, "events.bin");
        copyEventLog(root, "half", directory, "events.caelog");
        ResultSession session;
        requireEventOpen(session, temporary.path());
        requireSameEventLog(session.eventLog(), valid);
        require(!session.warnings().contains(half.error), "unselected half log must not warn");
        require(session.warnings() == baselineWarnings, "valid selected bin must not add warnings");
    });
    run("explicit valid path overrides bin", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        copyEventLog(root, "half", QDir(temporary.path()), "events.bin");
        ResultSession session;
        requireEventOpen(session,
                         QDir(temporary.path()).filePath("manifest.json"),
                         root.filePath("events/valid.caelog"));
        requireSameEventLog(session.eventLog(), valid);
        require(session.warnings() == baselineWarnings, "explicit valid log must not add warnings");
    });
    run("explicit corrupt path does not fall back", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        copyEventLog(root, "valid", QDir(temporary.path()), "events.bin");
        ResultSession session;
        requireEventOpen(session,
                         QDir(temporary.path()).filePath("manifest.json"),
                         root.filePath("events/bad-crc.caelog"));
        requireSameEventLog(session.eventLog(), badCrc);
        require(session.warnings().contains(badCrc.error),
                "explicit decoder error must warn verbatim");
    });
    run("explicit missing path does not fall back", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        const QDir directory(temporary.path());
        copyEventLog(root, "valid", directory, "events.bin");
        const auto missing = directory.filePath("missing.caelog");
        require(!QFileInfo::exists(missing), "explicit missing log unexpectedly exists");
        ResultSession session;
        requireEventOpen(session, directory.filePath("manifest.json"), missing);
        requireEmptyEventLog(session, baselineWarnings);
    });
    run("neither default exists", [&] { requireEmptyEventLog(baseline, baselineWarnings); });
    run("default filenames are case sensitive", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        const QDir directory(temporary.path());
        copyEventLog(root, "valid", directory, "Events.bin");
        copyEventLog(root, "valid", directory, "EVENTS.CAELOG");
        require(!QFileInfo::exists(directory.filePath("events.bin")) &&
                    !QFileInfo::exists(directory.filePath("events.caelog")),
                "case-sensitive fixture requires a case-sensitive filesystem");
        ResultSession session;
        requireEventOpen(session, temporary.path());
        requireEmptyEventLog(session, baselineWarnings);
    });

    QTemporaryDir repeatedDirectory;
    copyEventManifest(root, repeatedDirectory);
    copyEventLog(root, "valid", QDir(repeatedDirectory.path()), "events.bin");
    ResultSession repeated;
    run("repeated bin open does not accumulate events", [&] {
        requireEventOpen(repeated, repeatedDirectory.path());
        const auto firstLog = repeated.eventLog();
        requireEventOpen(repeated, repeatedDirectory.path());
        requireSameEventLog(firstLog, valid);
        requireSameEventLog(repeated.eventLog(), valid);
        requireSameEventLog(repeated.eventLog(), firstLog);
    });
    run("manifest-only reopen clears events and decoder error", [&] {
        requireEventOpen(repeated, manifestOnly.path());
        requireEmptyEventLog(repeated, baselineWarnings);
        // Seed a real decoder error as well, so clearing error is not a vacuous assertion.
        requireEventOpen(
            repeated, repeatedDirectory.path(), root.filePath("events/bad-crc.caelog"));
        requireSameEventLog(repeated.eventLog(), badCrc);
        require(repeated.warnings().contains(badCrc.error), "seeded decoder error must warn");
        requireEventOpen(repeated, manifestOnly.path());
        requireEmptyEventLog(repeated, baselineWarnings);
    });
    run("selected corrupt bin does not fall back to valid caelog", [&] {
        QTemporaryDir temporary;
        copyEventManifest(root, temporary);
        const QDir directory(temporary.path());
        copyEventLog(root, "bad-crc", directory, "events.bin");
        copyEventLog(root, "valid", directory, "events.caelog");
        ResultSession session;
        requireEventOpen(session, temporary.path());
        requireSameEventLog(session.eventLog(), badCrc);
        require(session.warnings().contains(badCrc.error),
                "selected bin decoder error must warn verbatim");
    });
    return failures;
}

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
namespace {
void stackWrite(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open stack fixture " + path);
    require(file.write(bytes) == bytes.size(), "write stack fixture " + path);
}
void copyStackPack(const QDir& root, const QString& target) {
    const QDir source(root.filePath("default"));
    QDirIterator it(source.path(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        const auto output = QDir(target).filePath(source.relativeFilePath(path));
        require(QDir().mkpath(QFileInfo(output).absolutePath()), "create stack pack directory");
        require(QFile::copy(path, output), "copy stack pack file");
    }
    // Modified synthetic HDF files no longer have the original checksum.
    QFile file(QDir(target).filePath("manifest.json"));
    require(file.open(QIODevice::ReadOnly), "read stack pack manifest");
    auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonArray steps;
    for (auto value : manifest["steps"].toArray()) {
        auto step = value.toObject();
        step.remove("checksum");
        steps.append(step);
    }
    manifest["steps"] = steps;
    stackWrite(file.fileName(), QJsonDocument(manifest).toJson());
}
const QByteArray fallbackStack = R"({"id":"fallback","unit":"m","outline":{"box":[0,0,1,1]},
    "symmetry":"none","package_type":"FCBGA","layers":[
    {"name":" Base ","material":"Mixed_CASE","thickness":0.001,"kind":"Homogenized","n_elem_min":1,"grading":1},
    {"name":" Base ","thickness":0.002,"kind":"Explicit","n_elem_min":1,"grading":1,
     "regions":[{"name":"ignored_region","material":"not_a_fourth_material","footprint":{"box":[0,0,1,1]}}]},
    {"name":"TOP","material":"","thickness":0.003,"kind":"Homogenized","n_elem_min":1,"grading":1}]})";
const QStringList fallbackLayers{" Base ", " Base ", "TOP"};
const QStringList fallbackMaterials{"Mixed_CASE", "", ""};
void removeFieldNames(const QString& target,
                      const QString& table,
                      bool empty,
                      bool alternating = false) {
    // Mutate only copied synthetic fixtures, using the same h5py dependency as result_fixtures.py.
    const QString script = R"(
import h5py, pathlib, sys
root, table, empty, alternating = sys.argv[1:]
for path in pathlib.Path(root, 'out').glob('*.vtkhdf'):
    if alternating == '1' and path.name == 'step-1.vtkhdf':
        continue
    with h5py.File(path, 'r+') as f:
        def change(name, group):
            if not isinstance(group, h5py.Group) or name.split('/')[-1] != 'FieldData':
                return
            for key in ('layer_names', 'material_names') if table == 'both' else (table,):
                if key in group:
                    del group[key]
                if empty == '1':
                    group.create_dataset(key, shape=(0,), dtype='S32')
        f.visititems(change)
)";
    QProcess process;
    const auto python = qEnvironmentVariable("CHIPCAE_FIXTURE_PYTHON", "python3");
    process.start(python,
                  {"-c", script, target, table, empty ? "1" : "0", alternating ? "1" : "0"});
    require(process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit &&
                process.exitCode() == 0,
            "h5py fixture mutation failed: " + QString::fromUtf8(process.readAllStandardError()));
}
void requireNames(const ResultSession& session,
                  const QStringList& layers,
                  const QStringList& materials) {
    require(session.layerNames() == layers,
            "layer fallback list: expected [" + layers.join(" | ") + "] got [" +
                session.layerNames().join(" | ") + "]");
    require(session.materialNames() == materials,
            "material fallback list: expected [" + materials.join(" | ") + "] got [" +
                session.materialNames().join(" | ") + "]");
    for (int i = 0; i < layers.size(); ++i)
        require(session.layerName(i + 1) == layers[i], "1-based layer name lookup");
    for (int i = 0; i < materials.size(); ++i)
        require(session.materialName(i + 1) == materials[i], "1-based material name lookup");
    for (int id : {-1, 0, 99})
        require(session.layerName(id).isEmpty() && session.materialName(id).isEmpty(),
                "invalid IDs remain unmapped");
    require(session.layerName(static_cast<int>(layers.size()) + 1).isEmpty() &&
                session.materialName(static_cast<int>(materials.size()) + 1).isEmpty(),
            "first out-of-range ID unmapped");
}
int stackNameAcceptance(const QDir& root) {
    int failures = 0;
    const auto run = [&](const QString& name, auto test) {
        try {
            test();
            std::cout << "PASS stack names: " << name.toStdString() << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL stack names: " << name.toStdString() << ": " << e.what() << '\n';
        }
    };
    ResultSession baseline;
    require(baseline.open(root.filePath("default")) && baseline.loadStep(0) != nullptr,
            "load FieldData reference");
    const auto fieldLayers = baseline.layerNames();
    const auto fieldMaterials = baseline.materialNames();
    require(fieldLayers == QStringList{"substrate", "bump_eq", "die_layer"} &&
                fieldMaterials ==
                    QStringList{"organic_eq", "SAC305_ball_eq", "underfill", "silicon"},
            "synthetic FieldData reference names");
    for (const auto& mode :
         {"both-missing", "both-empty", "layer-only", "material-only", "neither"}) {
        run(mode, [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "stack names directory");
            copyStackPack(root, temp.path());
            const QString variant(mode);
            if (variant != "neither")
                removeFieldNames(temp.path(),
                                 variant == "layer-only"      ? "layer_names"
                                 : variant == "material-only" ? "material_names"
                                                              : "both",
                                 variant == "both-empty");
            // Control verifies the intended empty table(s) before any fallback file exists.
            require(QFile::remove(temp.filePath("stackup.json")),
                    "remove copied stack before control read");
            ResultSession control;
            require(control.open(temp.path()) && control.loadStep(0) != nullptr,
                    "load no-stack control");
            const bool fallbackLayer =
                variant == "both-missing" || variant == "both-empty" || variant == "layer-only";
            const bool fallbackMaterial =
                variant == "both-missing" || variant == "both-empty" || variant == "material-only";
            require(control.layerNames() == (fallbackLayer ? QStringList{} : fieldLayers) &&
                        control.materialNames() ==
                            (fallbackMaterial ? QStringList{} : fieldMaterials),
                    "fixture has exactly the intended missing FieldData tables");
            stackWrite(temp.filePath("stackup.json"), fallbackStack);
            ResultSession session;
            require(session.open(temp.path()), session.error());
            for (int step : {0, 1, 2, 0}) {
                auto data = session.loadStep(step);
                require(data != nullptr, session.error());
                const auto layers = fallbackLayer ? fallbackLayers : fieldLayers;
                const auto materials = fallbackMaterial ? fallbackMaterials : fieldMaterials;
                requireNames(session, layers, materials);
                auto* ds = first(data);
                require(ds && ds->GetFieldData(), "loaded partition has FieldData");
                for (const auto& table :
                     {qMakePair("layer_names", layers), qMakePair("material_names", materials)}) {
                    auto* array = vtkStringArray::SafeDownCast(
                        ds->GetFieldData()->GetAbstractArray(table.first));
                    require(array && array->GetNumberOfValues() == table.second.size(),
                            "effective names attached to VTK FieldData");
                    for (int i = 0; i < table.second.size(); ++i)
                        require(QString::fromStdString(array->GetValue(i)) == table.second[i],
                                "attached name order and content");
                }
            }
        });
    }
    run("each step reselects names and session reopen clears fallback", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "alternating names directory");
        copyStackPack(root, temp.path());
        removeFieldNames(temp.path(), "both", false, true);
        stackWrite(temp.filePath("stackup.json"), fallbackStack);
        ResultSession session;
        require(session.open(temp.path()), session.error());
        for (int step : {0, 1, 2, 1, 0}) {
            require(session.loadStep(step) != nullptr, session.error());
            requireNames(session,
                         step == 1 ? fieldLayers : fallbackLayers,
                         step == 1 ? fieldMaterials : fallbackMaterials);
        }
        require(session.open(root.filePath("viewer_mixed")) && session.loadStep(0) != nullptr,
                "reopen unnamed directory without stack");
        requireNames(session, {}, {});
        require(session.open(root.filePath("default")) && session.loadStep(0) != nullptr,
                "reopen named directory");
        requireNames(session, fieldLayers, fieldMaterials);
    });
    for (const auto& fields : {QByteArray{},
                               QByteArray(R"(,"thickness":null)"),
                               QByteArray(R"(,"thickness":"invalid")"),
                               QByteArray(R"(,"thickness":[])")}) {
        run("name fallback ignores display-only fields: " + QString::fromUtf8(fields), [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "names-only stack directory");
            copyStackPack(root, temp.path());
            removeFieldNames(temp.path(), "both", false);
            stackWrite(temp.filePath("stackup.json"),
                       R"({"layers":[{"name":" Base ","material":"Mixed_CASE")" + fields +
                           R"(},{"name":" Base "},{"name":"TOP","material":""}]})");
            ResultSession session;
            require(session.open(temp.path()) && session.loadStep(0) != nullptr,
                    "names-only stack must not prevent result loading");
            requireNames(session, fallbackLayers, fallbackMaterials);
            require(session.resultsStackJson().isEmpty(),
                    "names-only stack is not eligible for row display");
        });
    }
    run("open caches stack bytes for later step loading", [&] {
        QTemporaryDir temp;
        require(temp.isValid(), "cached stack directory");
        copyStackPack(root, temp.path());
        removeFieldNames(temp.path(), "both", false);
        stackWrite(temp.filePath("stackup.json"), fallbackStack);
        ResultSession session;
        require(session.open(temp.path()), session.error());
        require(QFile::remove(temp.filePath("stackup.json")), "delete stack after open");
        for (int step : {0, 1, 0}) {
            require(session.loadStep(step) != nullptr, session.error());
            requireNames(session, fallbackLayers, fallbackMaterials);
        }
    });
    for (const auto& bytes :
         {QByteArray{},
          QByteArray("{"),
          QByteArray("[]"),
          QByteArray("{}"),
          QByteArray("{\"layers\":[]}"),
          QByteArray("{\"layers\":true}"),
          QByteArray("{\"layers\":[{\"name\":{},\"thickness\":0.1}]}"),
          QByteArray("{\"layers\":[{\"name\":\"x\",\"thickness\":0.1,\"material\":[]}]}"),
          QByteArray("missing")}) {
        run("invalid stack: " + QString::fromUtf8(bytes), [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "invalid stack directory");
            copyStackPack(root, temp.path());
            removeFieldNames(temp.path(), "both", false);
            if (bytes == "missing")
                require(QFile::remove(temp.filePath("stackup.json")), "remove stack fixture");
            else
                stackWrite(temp.filePath("stackup.json"), bytes);
            ResultSession session;
            require(session.open(temp.path()) && session.loadStep(0) != nullptr,
                    "invalid stack must not prevent load");
            requireNames(session, {}, {});
        });
    }
    for (const auto& mode : {"relative", "absolute", "parent", "symlink", "missing"}) {
        run(QString("manifest stackup path: ") + mode, [&] {
            QTemporaryDir temp;
            require(temp.isValid(), "stackup path directory");
            const auto pack = temp.filePath("pack");
            copyStackPack(root, pack);
            removeFieldNames(pack, "both", false);
            stackWrite(temp.filePath("outside.json"), fallbackStack);
            stackWrite(pack + "/stackup.json", fallbackStack);
            QFile file(pack + "/manifest.json");
            require(file.open(QIODevice::ReadOnly), "read path manifest");
            auto manifest = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            if (QString(mode) == "relative") {
                require(QDir().mkpath(pack + "/Nested"), "create relative path");
                stackWrite(pack + "/Nested/StackUP.json", fallbackStack);
                stackWrite(pack + "/stackup.json", "{}");
                manifest["stackup"] = "Nested/StackUP.json";
            } else if (QString(mode) == "absolute")
                manifest["stackup"] = temp.filePath("outside.json");
            else if (QString(mode) == "parent")
                manifest["stackup"] = "../outside.json";
            else if (QString(mode) == "missing")
                manifest["stackup"] = "absent.json";
            else {
                require(QFile::link(temp.filePath("outside.json"), pack + "/link.json"),
                        "create escaping stack link");
                manifest["stackup"] = "link.json";
            }
            stackWrite(file.fileName(), QJsonDocument(manifest).toJson());
            ResultSession session;
            require(session.open(pack) && session.loadStep(0) != nullptr,
                    "stack path does not prevent result loading");
            requireNames(session,
                         QString(mode) == "relative" ? fallbackLayers : QStringList{},
                         QString(mode) == "relative" ? fallbackMaterials : QStringList{});
        });
    }
    const auto s1 = qEnvironmentVariable("CHIPCAE_S1");
    if (s1.isEmpty() || !QFileInfo::exists(s1))
        std::cout << "SKIP stack name fallback: CHIPCAE_S1 is unavailable\n";
    else
        run("official s1 fallback", [&] {
            ResultSession session;
            require(session.open(s1) && session.loadStep(0) != nullptr, "open official s1");
            const QStringList expected{"substrate", "die_attach", "Si"};
            require(session.layerNames() == expected, "official s1 fallback names");
            for (int i = 0; i < expected.size(); ++i)
                require(session.layerName(i + 1) == expected[i], "official s1 1-based names");
        });
    return failures;
}
} // namespace

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
        const int stackNameStatus = stackNameAcceptance(root) == 0 ? 0 : 1;
        const int eventDiscoveryStatus =
            (eventLogDiscovery(root).isEmpty() ? 0 : 1) | stackNameStatus;
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
            return eventDiscoveryStatus;
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
            return eventDiscoveryStatus;
        }
        window->Render();
        require(window->GetNeverRendered() == 0, "S1 render failed");
        std::cout << "VTK " << vtkVersion::GetVTKVersion() << " S1 cells=" << ds->GetNumberOfCells()
                  << " points=" << ds->GetNumberOfPoints() << "\n";
        return eventDiscoveryStatus;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
