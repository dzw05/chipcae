#include "ResultSession.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>
#include <vtkCallbackCommand.h>
#include <vtkCellData.h>
#include <vtkCommand.h>
#include <vtkCompositeDataIterator.h>
#include <vtkCompositeDataSet.h>
#include <vtkDataSet.h>
#include <vtkFieldData.h>
#include <vtkHDFReader.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkStringArray.h>
#include <vtk_hdf5.h>

namespace chipcae::desktop {
namespace {
bool validMeshSha256(const QString& sha) {
    if (sha.size() != 64)
        return false;
    for (const QChar ch : sha)
        if (!((ch >= QChar('0') && ch <= QChar('9')) || (ch >= QChar('a') && ch <= QChar('f'))))
            return false;
    return true;
}
struct Handle {
    hid_t id;
    herr_t (*close)(hid_t);
    ~Handle() {
        if (id >= 0)
            close(id);
    }
};
QByteArray bytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("cannot open " + path.toStdString());
    return f.readAll();
}
QString canonicalBlock(QString name) {
    if (name == "cut_poly")
        return "cut";
    if (name == "embedded_boundary")
        return "eb_surface";
    return name;
}
struct Scan {
    QString error;
    QMap<QString, QStringList> names;
};
herr_t visit(hid_t file, const char* path, const H5O_info_t* info, void* context) {
    auto& scan = *static_cast<Scan*>(context);
    if (info->type != H5O_TYPE_DATASET)
        return 0;
    const QString name = QString::fromUtf8(path);
    const bool faces = name.endsWith("/PolyhedronToFaces");
    const bool table = name.endsWith("/layer_names") || name.endsWith("/material_names");
    if (!faces && !table)
        return 0;
    Handle ds{H5Dopen2(file, path, H5P_DEFAULT), H5Dclose};
    Handle space{H5Dget_space(ds.id), H5Sclose};
    auto count = H5Sget_simple_extent_npoints(space.id);
    if (count < 0) {
        scan.error = "invalid HDF dataspace";
        return 1;
    }
    if (faces) {
        // Read in bounded chunks, including every temporal step and partition.
        for (hsize_t start = 0; start < static_cast<hsize_t>(count); start += 4096) {
            hsize_t size = std::min<hsize_t>(4096, count - start);
            Handle memory{H5Screate_simple(1, &size, nullptr), H5Sclose};
            long long values[4096];
            if (H5Sget_simple_extent_ndims(space.id) != 1 ||
                H5Sselect_hyperslab(space.id, H5S_SELECT_SET, &start, nullptr, &size, nullptr) <
                    0 ||
                H5Dread(ds.id, H5T_NATIVE_LLONG, memory.id, space.id, H5P_DEFAULT, values) < 0) {
                scan.error = "cannot inspect PolyhedronToFaces";
                return 1;
            }
            for (hsize_t i = 0; i < size; ++i)
                if (values[i] < 0) {
                    scan.error = "unsupported negative PolyhedronToFaces reference (before VTK)";
                    return 1;
                }
        }
    } else {
        Handle type{H5Dget_type(ds.id), H5Tclose};
        if (H5Tget_class(type.id) == H5T_STRING && H5Tis_variable_str(type.id) == 0) {
            const auto width = H5Tget_size(type.id);
            if (width == 0 || count > 1048576 / width) {
                scan.error = "oversized name table";
                return 1;
            }
            QByteArray raw(static_cast<qsizetype>(count * width), '\0');
            if (H5Dread(ds.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw.data()) < 0) {
                scan.error = "cannot read name table";
                return 1;
            }
            QStringList names;
            for (hssize_t i = 0; i < count; ++i) {
                QByteArray item(raw.constData() + i * width, width);
                auto end = item.indexOf('\0');
                names << QString::fromUtf8(end < 0 ? item : item.left(end));
            }
            scan.names.insert(name, names);
        } else if (H5Tget_class(type.id) == H5T_STRING && H5Tis_variable_str(type.id) != 0) {
            // Official S1b / O1b writer uses H5T_VARIABLE strings; vtkHDFReader
            // does not expose these FieldData arrays on the partitions it builds.
            if (count > 1048576) {
                scan.error = "oversized name table";
                return 1;
            }
            std::vector<char*> buffer(static_cast<size_t>(count), nullptr);
            if (H5Dread(ds.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer.data()) < 0) {
                scan.error = "cannot read name table";
                return 1;
            }
            QStringList names;
            for (hssize_t i = 0; i < count; ++i)
                names << QString::fromUtf8(
                    buffer[static_cast<size_t>(i)] ? buffer[static_cast<size_t>(i)] : "");
            scan.names.insert(name, names);
            H5Dvlen_reclaim(type.id, space.id, H5P_DEFAULT, buffer.data());
        }
    }
    return 0;
}

QStringList tableNamed(const QMap<QString, QStringList>& tables, const char* suffix) {
    QStringList chosen;
    const QString ending = QString::fromLatin1(suffix);
    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        if (!it.key().endsWith(ending))
            continue;
        if (it.key().contains("/hex/"))
            return it.value();
        if (chosen.isEmpty())
            chosen = it.value();
    }
    return chosen;
}
QString idName(const QStringList& names, int id) {
    if (id < 1 || id > names.size())
        return {};
    return names[id - 1];
}
bool validStackNames(const QJsonObject& stack) {
    if (!stack["layers"].isArray() || stack["layers"].toArray().isEmpty())
        return false;
    for (auto value : stack["layers"].toArray()) {
        if (!value.isObject())
            return false;
        const auto layer = value.toObject();
        if (!layer["name"].isString() ||
            (layer.contains("material") && !layer["material"].isString()))
            return false;
    }
    return true;
}
bool validStackRows(const QJsonObject& stack) {
    if (!validStackNames(stack))
        return false;
    for (auto value : stack["layers"].toArray()) {
        const auto layer = value.toObject();
        if (!layer["thickness"].isDouble() || !std::isfinite(layer["thickness"].toDouble()))
            return false;
        if (layer.contains("copper")) {
            if (!layer["copper"].isObject())
                return false;
            const auto copper = layer["copper"].toObject();
            if (copper.contains("fraction") &&
                (!copper["fraction"].isDouble() || !std::isfinite(copper["fraction"].toDouble())))
                return false;
            if (copper.contains("polygons")) {
                const auto polygons = copper["polygons"].toObject();
                if (!polygons["file"].isString() || !polygons["layer"].isString())
                    return false;
            }
        }
    }
    if (stack.contains("external_refs")) {
        if (!stack["external_refs"].isArray())
            return false;
        for (auto value : stack["external_refs"].toArray()) {
            const auto ref = value.toObject();
            if (!ref["path"].isString() || !ref["sha256"].isString())
                return false;
        }
    }
    return true;
}
void attachNameTables(vtkDataObject* object,
                      const QStringList& layers,
                      const QStringList& materials) {
    if (!object)
        return;
    auto add = [&](const char* name, const QStringList& values) {
        if (auto* existing = object->GetFieldData()->GetAbstractArray(name))
            if (existing->GetNumberOfValues() > 0)
                return;
        vtkNew<vtkStringArray> array;
        array->SetName(name);
        array->SetNumberOfValues(values.size());
        for (int i = 0; i < values.size(); ++i)
            array->SetValue(i, values[i].toUtf8().constData());
        object->GetFieldData()->AddArray(array);
    };
    add("layer_names", layers);
    add("material_names", materials);
}
} // namespace
QString ResultSession::checkedPath(const QString& relative) const {
    if (QDir::isAbsolutePath(relative))
        throw std::runtime_error("absolute result path forbidden");
    const auto p = std::filesystem::weakly_canonical(std::filesystem::path(root_.toStdString()) /
                                                     relative.toStdString());
    auto rel = p.lexically_relative(root_.toStdString());
    if (rel.empty() || *rel.begin() == "..")
        throw std::runtime_error("result path escapes run directory");
    return QString::fromStdString(p.string());
}
bool ResultSession::open(const QString& input, const QString& sidecar, const QString& eventPath) {
    manifest_ = {};
    warnings_.clear();
    error_.clear();
    names_.clear();
    resultsStackJson_.clear();
    stackLayerNames_.clear();
    stackMaterialNames_.clear();
    fallbackLayerNames_.clear();
    fallbackMaterialNames_.clear();
    log_ = {};
    direct_.clear();
    try {
        QFileInfo info(input);
        QString manifestPath;
        if (info.isDir())
            manifestPath = QDir(input).filePath("manifest.json");
        else if (input.endsWith(".json"))
            manifestPath = input;
        else {
            direct_ = info.canonicalFilePath();
            if (direct_.isEmpty())
                throw std::runtime_error("VTKHDF file missing");
            manifestPath = sidecar;
            if (manifestPath.isEmpty())
                manifestPath = info.dir().filePath("manifest.json");
        }
        root_ = QFileInfo(manifestPath).absoluteDir().canonicalPath();
        if (root_.isEmpty())
            throw std::runtime_error("run directory missing");
        if (QFileInfo::exists(manifestPath)) {
            QJsonParseError parse;
            auto doc = QJsonDocument::fromJson(bytes(manifestPath), &parse);
            if (parse.error != QJsonParseError::NoError || !doc.isObject())
                throw std::runtime_error("invalid manifest JSON");
            manifest_ = doc.object();
            if (!manifest_["steps"].isArray() || !manifest_["fields"].isArray())
                throw std::runtime_error("manifest requires steps and fields arrays");
            if (manifest_["schema_version"].toString() != "chipcae.results/1.0")
                warnings_ << "unknown manifest schema_version";
            auto normalizedSteps = steps();
            for (qsizetype i = 0; i < normalizedSteps.size(); ++i) {
                auto step = normalizedSteps[i].toObject();
                if (!step.contains("n") && step.contains("step")) {
                    step["n"] = step["step"];
                    warnings_ << "legacy step mapped to n";
                }
                normalizedSteps[i] = step;
            }
            manifest_["steps"] = normalizedSteps;
            auto normalizedFields = fields();
            for (qsizetype i = 0; i < normalizedFields.size(); ++i) {
                auto field = normalizedFields[i].toObject();
                for (auto pair :
                     {std::pair{"location", "association"}, std::pair{"components", "ncomp"}}) {
                    if (!field.contains(pair.first) && field.contains(pair.second)) {
                        field[pair.first] = field[pair.second];
                        warnings_ << QString("legacy manifest %1 mapped to %2")
                                         .arg(pair.second, pair.first);
                    }
                }
                if (!field["name"].isString() || !field["location"].isString() ||
                    field["components"].toInt() < 1)
                    throw std::runtime_error("invalid manifest field");
                normalizedFields[i] = field;
            }
            manifest_["fields"] = normalizedFields;
            std::set<qint64> seen;
            for (auto v : steps()) {
                auto s = v.toObject();
                if (!s.contains("n") || !s.contains("time") || !s["file"].isString())
                    throw std::runtime_error("invalid manifest step");
                if (!s["n"].isDouble() || s["n"].toInteger(-1) < 0 || !s["time"].isDouble() ||
                    !seen.insert(s["n"].toInteger()).second)
                    throw std::runtime_error("invalid or duplicate manifest step n/time");
                checkedPath(s["file"].toString());
            }
        } else if (!direct_.isEmpty()) {
            manifest_ = QJsonObject{{"steps", QJsonArray{QJsonObject{{"n", 0}, {"time", 0.0}}}},
                                    {"fields", QJsonArray{}}};
            warnings_ << "standalone VTKHDF without manifest";
        } else
            throw std::runtime_error("manifest missing");
        const int format = 1;
        const auto declaredFormat = manifest_.value("event_log_format");
        if (!declaredFormat.isUndefined() &&
            (!declaredFormat.isDouble() || declaredFormat.toDouble() != 1.0))
            throw std::runtime_error("unsupported event_log_format (only 1 supported)");
        for (auto v : manifest_["summary"].toArray())
            if (v.toObject()["name"] == "U_abs_max")
                warnings_ << "legacy U_abs_max mapped to U_mag_max";
        for (auto v : manifest_["blocks"].toArray()) {
            auto name = v.toObject()["name"].toString();
            if (canonicalBlock(name) != name)
                warnings_ << "legacy block name: " + name;
        }
        QString logPath = eventPath;
        if (logPath.isEmpty()) {
            for (const auto* name : {"events.bin", "events.caelog"}) {
                const auto candidate = QDir(root_).filePath(name);
                if (QFileInfo::exists(candidate)) {
                    logPath = candidate;
                    break;
                }
            }
        }
        if (QFileInfo::exists(logPath)) {
            log_ = readEventLog(logPath, format);
            if (!log_.error.isEmpty())
                warnings_ << log_.error;
        }
        cacheResultsStack();
        return true;
    } catch (const std::exception& e) {
        error_ = QString::fromUtf8(e.what());
        return false;
    }
}
void ResultSession::cacheResultsStack() {
    try {
        const auto declared = manifest_.value("stackup");
        if (!declared.isUndefined() && (!declared.isString() || declared.toString().isEmpty()))
            throw std::runtime_error("invalid stackup path");
        const auto path = checkedPath(declared.isUndefined() ? QStringLiteral("stackup.json")
                                                             : declared.toString());
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("cannot open stackup file");
        const auto raw = file.readAll();
        if (file.error() != QFileDevice::NoError)
            throw std::runtime_error("cannot read stackup file");
        QJsonParseError error;
        const auto stack = QJsonDocument::fromJson(raw, &error);
        if (error.error != QJsonParseError::NoError || !stack.isObject() ||
            !validStackNames(stack.object()))
            throw std::runtime_error("invalid stackup JSON or name fields");
        for (auto value : stack.object()["layers"].toArray()) {
            const auto layer = value.toObject();
            stackLayerNames_ << layer["name"].toString();
            stackMaterialNames_ << layer["material"].toString();
        }
        // Name fallback remains usable even when the stack cannot safely populate the table.
        if (!validStackRows(stack.object()))
            throw std::runtime_error("invalid stackup display fields");
        resultsStackJson_ = raw;
    } catch (const std::exception& e) {
        warnings_ << QStringLiteral("result stackup ignored: %1").arg(QString::fromUtf8(e.what()));
    }
}
QJsonArray ResultSession::steps() const {
    return manifest_["steps"].toArray();
}
QJsonArray ResultSession::fields() const {
    return manifest_["fields"].toArray();
}
QJsonArray ResultSession::blocks() const {
    QJsonArray out;
    for (auto v : manifest_["blocks"].toArray()) {
        auto o = v.toObject();
        auto n = o["name"].toString();
        o["name"] = canonicalBlock(n);
        out.append(o);
    }
    return out;
}
QJsonArray ResultSession::summary() const {
    auto out = manifest_["summary"].toArray();
    for (qsizetype i = 0; i < out.size(); ++i) {
        auto o = out[i].toObject();
        if (o["name"] == "U_abs_max") {
            bool preferred = false;
            for (auto v : out) {
                auto s = v.toObject();
                if (s["name"] == "U_mag_max" && s["step"] == o["step"])
                    preferred = true;
            }
            if (preferred) {
                out.removeAt(i--);
                continue;
            }
            o["name"] = "U_mag_max";
            out[i] = o;
        }
    }
    return out;
}
QJsonObject ResultSession::meshDigest() const {
    QJsonObject out = manifest_["mesh"].toObject();
    for (auto key : {"nx0", "ny0", "L", "padded_cells", "nz", "mesh_sha256"})
        if (!out.contains(key) && manifest_.contains(key))
            out[key] = manifest_[key];
    return out;
}
MeshSha256Compare ResultSession::comparePreviewMeshSha256(const QString& previewSha256,
                                                          QString* detail) const {
    const QString preview = previewSha256.trimmed();
    if (!validMeshSha256(preview)) {
        if (detail)
            *detail = "preview mesh SHA-256 is missing or invalid";
        return MeshSha256Compare::PreviewMissing;
    }
    const QString manifestSha = meshDigest().value("mesh_sha256").toString().trimmed();
    if (!validMeshSha256(manifestSha)) {
        if (detail)
            *detail = "manifest mesh SHA-256 is missing or invalid";
        return MeshSha256Compare::ManifestMissing;
    }
    if (preview == manifestSha) {
        if (detail)
            detail->clear();
        return MeshSha256Compare::Match;
    }
    if (detail)
        *detail = QString("preview mesh SHA-256 %1 differs from manifest mesh SHA-256 %2")
                      .arg(preview, manifestSha);
    return MeshSha256Compare::Mismatch;
}
bool ResultSession::preflight(const QString& path) {
    Handle file{H5Fopen(path.toUtf8().constData(), H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose};
    if (file.id < 0) {
        error_ = "cannot open VTKHDF";
        return false;
    }
    Handle group{H5Gopen2(file.id, "/VTKHDF", H5P_DEFAULT), H5Gclose};
    if (group.id < 0) {
        error_ = "missing VTKHDF root";
        return false;
    }
    Handle attr{H5Aopen(group.id, "Version", H5P_DEFAULT), H5Aclose};
    int version[2] = {};
    Handle space{attr.id < 0 ? -1 : H5Aget_space(attr.id), H5Sclose};
    if (space.id < 0 || H5Sget_simple_extent_npoints(space.id) != 2 ||
        H5Aread(attr.id, H5T_NATIVE_INT, version) < 0 || version[0] != 2 ||
        (version[1] < 8 && version[1] != 0)) {
        error_ = "unsupported VTKHDF Version";
        return false;
    }
    if (version[1] != 8)
        warnings_ << QString("VTKHDF Version 2.%1 compatibility read").arg(version[1]);
    Handle typeAttr{H5Aopen(group.id, "Type", H5P_DEFAULT), H5Aclose};
    Handle type{typeAttr.id < 0 ? -1 : H5Aget_type(typeAttr.id), H5Tclose};
    Handle typeSpace{typeAttr.id < 0 ? -1 : H5Aget_space(typeAttr.id), H5Sclose};
    if (type.id < 0 || typeSpace.id < 0 || H5Tget_class(type.id) != H5T_STRING ||
        H5Tis_variable_str(type.id) != 0 || H5Tget_size(type.id) > 256 ||
        H5Sget_simple_extent_npoints(typeSpace.id) != 1) {
        error_ = "invalid VTKHDF Type";
        return false;
    }
    QByteArray layout(static_cast<qsizetype>(H5Tget_size(type.id)) + 1, '\0');
    if (H5Aread(typeAttr.id, type.id, layout.data()) < 0) {
        error_ = "cannot read VTKHDF Type";
        return false;
    }
    const QString kind = QString::fromUtf8(layout.constData());
    if (version[1] == 0 && kind != "UnstructuredGrid") {
        error_ = "legacy 2.0 requires single UnstructuredGrid";
        return false;
    }
    if (kind == "PartitionedDataSetCollection" &&
        H5Lexists(group.id, "Assembly", H5P_DEFAULT) <= 0) {
        error_ = "PDC missing required Assembly";
        return false;
    }
    Scan scan;
    auto status = H5Ovisit(file.id, H5_INDEX_NAME, H5_ITER_NATIVE, visit, &scan);
    if (status < 0 && scan.error.isEmpty())
        scan.error = "VTKHDF preflight traversal failed";
    names_ = scan.names;
    error_ = scan.error;
    return error_.isEmpty();
}
vtkSmartPointer<vtkDataObject> ResultSession::loadStep(qint64 n, const QString& field) {
    error_.clear();
    names_.clear();
    fallbackLayerNames_.clear();
    fallbackMaterialNames_.clear();
    try {
        QJsonObject step;
        for (auto v : steps())
            if (v.toObject()["n"].toInteger() == n)
                step = v.toObject();
        if (step.isEmpty())
            throw std::runtime_error("unknown step");
        if (!step.value("complete").toBool(true)) {
            warnings_ << "step incomplete; waiting for publication";
            return nullptr;
        }
        QString path = step["file"].isString() ? checkedPath(step["file"].toString()) : direct_;
        if (!QFileInfo::exists(path)) {
            warnings_ << "step file missing; waiting for publication";
            return nullptr;
        }
        auto checksum = step["checksum"].toString();
        if (!checksum.isEmpty()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("cannot open step for checksum");
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&file);
            if (hash.result().toHex() != checksum.toLatin1())
                throw std::runtime_error("step checksum mismatch");
        }
        if (!preflight(path))
            return nullptr;
        if (tableNamed(names_, "/layer_names").isEmpty())
            fallbackLayerNames_ = stackLayerNames_;
        if (tableNamed(names_, "/material_names").isEmpty())
            fallbackMaterialNames_ = stackMaterialNames_;
        vtkNew<vtkHDFReader> reader;
        bool readerError = false;
        vtkNew<vtkCallbackCommand> errors;
        errors->SetClientData(&readerError);
        errors->SetCallback([](vtkObject*, unsigned long, void* context, void*) {
            *static_cast<bool*>(context) = true;
        });
        reader->AddObserver(vtkCommand::ErrorEvent, errors);
        reader->SetFileName(path.toUtf8().constData());
        reader->UpdateInformation();
        reader->UpdateTimeStep(step["time"].toDouble());
        if (readerError || reader->GetErrorCode())
            throw std::runtime_error("VTKHDF reader failed");
        vtkSmartPointer<vtkDataObject> result = reader->GetOutputDataObject(0);
        if (!result)
            throw std::runtime_error("VTKHDF produced no data");
        bool found = field.isEmpty();
        auto inspect = [&](vtkDataObject* object) {
            auto ds = vtkDataSet::SafeDownCast(object);
            if (!ds)
                return;
            for (auto pair :
                 {std::pair{"layer_id", "layer"}, std::pair{"material_id", "material"}}) {
                auto data = ds->GetCellData();
                auto old = data->GetArray(pair.second);
                if (old && !data->GetArray(pair.first)) {
                    warnings_
                        << QString("legacy field %1 mapped to %2").arg(pair.second, pair.first);
                    vtkSmartPointer<vtkDataArray> alias;
                    alias.TakeReference(old->NewInstance());
                    alias->DeepCopy(old);
                    alias->SetName(pair.first);
                    data->AddArray(alias);
                } else if (old)
                    warnings_ << QString("field conflict: %1 preferred over %2")
                                     .arg(pair.first, pair.second);
            }
            if (!field.isEmpty()) {
                if (ds->GetPointData()->HasArray(field.toUtf8())) {
                    ds->GetPointData()->SetActiveScalars(field.toUtf8());
                    found = true;
                }
                if (ds->GetCellData()->HasArray(field.toUtf8())) {
                    ds->GetCellData()->SetActiveScalars(field.toUtf8());
                    found = true;
                }
            }
        };
        if (auto composite = vtkCompositeDataSet::SafeDownCast(result)) {
            vtkSmartPointer<vtkCompositeDataIterator> it;
            it.TakeReference(composite->NewIterator());
            for (it->InitTraversal(); !it->IsDoneWithTraversal(); it->GoToNextItem())
                inspect(it->GetCurrentDataObject());
        } else
            inspect(result);
        if (!found)
            warnings_ << "selected field absent (empty or mesh-only result)";
        const auto layers = layerNames();
        const auto materials = materialNames();
        attachNameTables(result, layers, materials);
        if (auto composite = vtkCompositeDataSet::SafeDownCast(result)) {
            vtkSmartPointer<vtkCompositeDataIterator> it;
            it.TakeReference(composite->NewIterator());
            for (it->InitTraversal(); !it->IsDoneWithTraversal(); it->GoToNextItem())
                attachNameTables(it->GetCurrentDataObject(), layers, materials);
        }
        return result;
    } catch (const std::exception& e) {
        error_ = QString::fromUtf8(e.what());
        return nullptr;
    }
}
EventLogResult ResultSession::readEventLog(const QString& path, int format) {
    EventLogResult out;
    if (format != 1) {
        out.error = "unsupported event_log_format (only 1 supported)";
        return out;
    }
    try {
        auto data = bytes(path);
        auto u32 = [&](qsizetype p) {
            quint32 v = 0;
            for (int i = 0; i < 4; ++i)
                v |= quint32(quint8(data[p + i])) << (8 * i);
            return v;
        };
        qsizetype p = 0;
        while (p < data.size()) {
            if (data.size() - p < 16) {
                out.error = "truncated CAELOG1 header";
                break;
            }
            if (std::memcmp(data.constData() + p, "CAELOG1\0", 8)) {
                out.error = "invalid CAELOG1 magic";
                break;
            }
            quint32 length = u32(p + 8), expected = u32(p + 12);
            if (length > quint64(data.size() - p - 16)) {
                out.error = "truncated CAELOG1 record";
                break;
            }
            quint32 crc = 0xffffffffu;
            for (quint32 i = 0; i < length; ++i) {
                crc ^= quint8(data[p + 16 + i]);
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
            }
            if ((crc ^ 0xffffffffu) != expected) {
                out.error = "CAELOG1 bad CRC-32/IEEE";
                break;
            }
            p += 16 + length;
            out.validatedBytes = p;
        }
        std::uint64_t offset = 0;
        out.events = chipcae::io::EventLogReader(path.toStdString()).read_from(0, &offset);
        if (offset != out.validatedBytes && out.error.isEmpty())
            out.error = "invalid CAELOG1 payload";
    } catch (const std::exception& e) {
        out.error = QString::fromUtf8(e.what());
    }
    return out;
}
QStringList ResultSession::layerNames() const {
    const auto names = tableNamed(names_, "/layer_names");
    return names.isEmpty() ? fallbackLayerNames_ : names;
}
QStringList ResultSession::materialNames() const {
    const auto names = tableNamed(names_, "/material_names");
    return names.isEmpty() ? fallbackMaterialNames_ : names;
}
QString ResultSession::layerName(int id) const {
    return idName(layerNames(), id);
}
QString ResultSession::materialName(int id) const {
    return idName(materialNames(), id);
}
} // namespace chipcae::desktop
