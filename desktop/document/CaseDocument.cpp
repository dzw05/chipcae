#include "CaseDocument.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <jsoncons_ext/jsonpointer/jsonpointer.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <set>
#include <stdexcept>

namespace chipcae::desktop {
namespace {
QByteArray bytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(f.errorString().toStdString());
    return f.readAll();
}
Json read(const QString& path) {
    return Json::parse(bytes(path).toStdString());
}
QByteArray digest(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
void write(const QString& path, const Json& value) {
    const auto text = QByteArray::fromStdString(value.to_string());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
}
void libraryCheck(const Json& value) {
    if (value.at("schema") != "chipcae.material_library/1" || !value.at("materials").is_array())
        throw std::runtime_error("Invalid material library container");
    std::set<std::string> ids;
    for (const auto& item : value.at("materials").array_range()) {
        if (!ids.insert(item.at("id").as<std::string>()).second)
            throw std::runtime_error("MATERIAL_ID_DUPLICATE");
    }
}
Json reference(const Json& ref, const QDir& dir, bool parse = true) {
    auto name =
        QString::fromStdString(ref.at(ref.contains("ref") ? "ref" : "path").as<std::string>());
    if (QDir::isAbsolutePath(name) || name.contains('\\') || name.contains(":") ||
        name.split('/').contains(".."))
        throw std::runtime_error("Invalid reference path");
    auto path = dir.filePath(name);
    auto canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.startsWith(dir.canonicalPath() + '/'))
        throw std::runtime_error("Reference escapes document directory");
    auto raw = bytes(path);
    if (ref.contains("sha256") &&
        digest(raw).toLower() !=
            QByteArray::fromStdString(ref.at("sha256").as<std::string>()).toLower())
        throw std::runtime_error("INPUT_HASH_MISMATCH");
    return parse ? Json::parse(raw.toStdString()) : Json::object();
}
DocumentError exceptionError(const std::exception& e) {
    QString message = QString::fromUtf8(e.what());
    return {message == "MATERIAL_ID_DUPLICATE" || message == "INPUT_HASH_MISMATCH"
                ? message
                : "DOCUMENT_ERROR",
            message};
}
} // namespace
MeshDigest MeshDigest::fromJson(const Json& v) {
    auto integer = [&](const char* key) {
        if (!v.at(key).is_int64())
            throw std::runtime_error("Digest field must be int64");
        return v.at(key).as<int64_t>();
    };
    MeshDigest d{integer("nx0"),
                 integer("ny0"),
                 integer("L"),
                 integer("padded_cells"),
                 integer("nz"),
                 {},
                 {},
                 v.at("mesh_sha256").as<std::string>()};
    if (v.contains("npoints"))
        d.npoints = integer("npoints");
    if (v.contains("ncells"))
        d.ncells = integer("ncells");
    if (!QRegularExpression("^[0-9a-f]{64}$")
             .match(QString::fromStdString(d.mesh_sha256))
             .hasMatch())
        throw std::runtime_error("Invalid mesh_sha256");
    return d;
}
CaseDocument::CaseDocument(QObject* parent) : QObject(parent) {}
void CaseDocument::markDirty() {
    if (!dirty_) {
        dirty_ = true;
        emit dirtyChanged(true);
    }
    emit changed();
}
void CaseDocument::beginEdit() {
    emit editStarted();
}
void CaseDocument::applyJson(const Json& value) {
    if (data_ == value)
        return;
    Json materials = materials_;
    Json stack = stack_;
    const bool refsChanged =
        (value.contains("stack") &&
         (!data_.contains("stack") || value.at("stack") != data_.at("stack"))) ||
        (value.contains("materials") &&
         (!data_.contains("materials") || value.at("materials") != data_.at("materials")));
    if (refsChanged) {
        try {
            if (path_.isEmpty())
                throw std::runtime_error("Cannot change case references before load");
            const auto dir = QFileInfo(path_).absoluteDir();
            if (value.contains("materials")) {
                materials = reference(value.at("materials"), dir);
                libraryCheck(materials);
            }
            if (value.contains("stack")) {
                stack = reference(value.at("stack"), dir);
                if (stack.contains("external_refs")) {
                    const auto stackPath = dir.filePath(
                        QString::fromStdString(value.at("stack").at("ref").as<std::string>()));
                    for (const auto& ref : stack.at("external_refs").array_range())
                        reference(ref, QFileInfo(stackPath).absoluteDir(), false);
                }
            }
        } catch (const std::exception& e) {
            errors_ = {exceptionError(e)};
            return;
        }
    }
    data_ = value;
    materials_ = std::move(materials);
    stack_ = std::move(stack);
    markDirty();
}
void CaseDocument::setValue(const std::string& pointer, const Json& value) {
    auto copy = data_;
    jsoncons::jsonpointer::replace(copy, pointer, value);
    applyJson(copy);
}
MeshOptions CaseDocument::meshOptions() const {
    MeshOptions o;
    const auto& m = data_.at("mesh");
    if (m.contains("nx"))
        o.nx = m.at("nx").as<int64_t>();
    if (m.contains("ny"))
        o.ny = m.at("ny").as<int64_t>();
    o.maxRatio = m.get_value_or<double>("max_ratio", 2);
    o.levels = m.get_value_or<int64_t>("levels", 0);
    o.cellBudget = m.get_value_or<int64_t>("cell_budget", 50000);
    return o;
}
void CaseDocument::setMeshOptions(const MeshOptions& o) {
    auto copy = data_;
    auto& m = copy.at("mesh");
    if (o.nx)
        m["nx"] = *o.nx;
    else
        m.erase("nx");
    if (o.ny)
        m["ny"] = *o.ny;
    else
        m.erase("ny");
    m["max_ratio"] = o.maxRatio;
    m["levels"] = o.levels;
    m["cell_budget"] = o.cellBudget;
    applyJson(copy);
}
DocumentErrors CaseDocument::validate() const {
    DocumentErrors result;
    try {
        static const auto validator = jsoncons::jsonschema::make_json_schema(
            read(QStringLiteral(CHIPCAE_SCHEMA_DIR "/case-1.0.schema.json")),
            [](const jsoncons::uri&) -> Json {
                return read(QStringLiteral(CHIPCAE_SCHEMA_DIR "/parameters.schema.json"));
            });
        if (!validator.is_valid(data_))
            result.push_back({"CASE_SCHEMA_INVALID", "case.v1 schema validation failed"});
        if (!path_.isEmpty()) {
            const auto root = QFileInfo(path_).absoluteDir();
            reference(data_.at("stack"), root);
            reference(data_.at("materials"), root);
            if (stack_.contains("external_refs")) {
                const auto stackDir = QFileInfo(root.filePath(QString::fromStdString(
                                                    data_.at("stack").at("ref").as<std::string>())))
                                          .absoluteDir();
                for (const auto& ref : stack_.at("external_refs").array_range())
                    reference(ref, stackDir, false);
            }
        }
        std::set<std::string> stableIds;
        auto checkIds = [&](auto&& self, const Json& node) -> void {
            if (node.is_object()) {
                if (node.contains("id") &&
                    !stableIds.insert(node.at("id").as<std::string>()).second)
                    result.push_back({"CASE_ID_DUPLICATE", "Duplicate case item id"});
                for (const auto& member : node.object_range())
                    if (member.key().substr(0, 2) != "x-")
                        self(self, member.value());
            } else if (node.is_array())
                for (const auto& child : node.array_range())
                    self(self, child);
        };
        if (data_.contains("loads"))
            checkIds(checkIds, data_.at("loads"));
        if (data_.contains("constraints"))
            checkIds(checkIds, data_.at("constraints"));
        if (!stack_.empty()) {
            static const auto stackValidator = jsoncons::jsonschema::make_json_schema(
                read(QStringLiteral(CHIPCAE_SCHEMA_DIR "/stack-0.2.schema.json")));
            if (!stackValidator.is_valid(stack_))
                result.push_back({"STACK_SCHEMA_INVALID", "stack0.2 validation failed"});
            std::set<std::string> layers;
            for (const auto& layer : stack_.at("layers").array_range())
                if (!layers.insert(layer.at("name").as<std::string>()).second)
                    result.push_back({"LAYER_NAME_DUPLICATE", "Duplicate stack layer name"});
            auto targets = [&](auto&& self, const Json& node) -> void {
                if (node.is_object()) {
                    if (node.contains("target")) {
                        const auto target = node.at("target").as<std::string>();
                        if (target.starts_with("layer:") && !layers.contains(target.substr(6)))
                            result.push_back({"TARGET_LAYER_INVALID", "Unknown target layer"});
                    }
                    for (const auto& member : node.object_range())
                        if (member.key().substr(0, 2) != "x-")
                            self(self, member.value());
                } else if (node.is_array())
                    for (const auto& child : node.array_range())
                        self(self, child);
            };
            targets(targets, data_);
        }
        if (!materials_.empty())
            libraryCheck(materials_);
        if (!stack_.empty() && !materials_.empty()) {
            std::set<std::string> ids;
            for (const auto& m : materials_.at("materials").array_range())
                ids.insert(m.at("id").as<std::string>());
            for (const auto& layer : stack_.at("layers").array_range()) {
                if (layer.contains("material") &&
                    !ids.contains(layer.at("material").as<std::string>()))
                    result.push_back({"MATERIAL_REFERENCE_INVALID", "Unknown layer material id"});
                if (layer.contains("regions"))
                    for (const auto& region : layer.at("regions").array_range())
                        if (!ids.contains(region.at("material").as<std::string>()))
                            result.push_back(
                                {"MATERIAL_REFERENCE_INVALID", "Unknown region material id"});
            }
        }
    } catch (const std::exception& e) {
        result.push_back(exceptionError(e));
    }
    return result;
}
bool CaseDocument::load(const QString& path) {
    try {
        CaseDocument candidate;
        auto raw = bytes(path);
        candidate.data_ = Json::parse(raw.toStdString());
        const auto dir = QFileInfo(path).absoluteDir();
        candidate.materials_ = reference(candidate.data_.at("materials"), dir);
        libraryCheck(candidate.materials_);
        candidate.stack_ = reference(candidate.data_.at("stack"), dir);
        if (candidate.stack_.contains("external_refs")) {
            const auto stackPath = dir.filePath(
                QString::fromStdString(candidate.data_.at("stack").at("ref").as<std::string>()));
            for (const auto& ref : candidate.stack_.at("external_refs").array_range())
                reference(ref, QFileInfo(stackPath).absoluteDir(), false);
        }
        errors_ = candidate.validate();
        if (!errors_.empty())
            return false;
        data_ = candidate.data_;
        materials_ = candidate.materials_;
        stack_ = candidate.stack_;
        path_ = QFileInfo(path).absoluteFilePath();
        hash_ = digest(raw);
        dirty_ = false;
        emit changed();
        emit dirtyChanged(false);
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::save(const QString& path) {
    errors_ = validate();
    if (!errors_.empty())
        return false;
    try {
        auto copy = data_;
        if (!stack_.empty() && !materials_.empty()) {
            const auto targetDir = QFileInfo(path).absoluteDir();
            const auto bundle = "case-assets-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!targetDir.mkpath(bundle))
                throw std::runtime_error("Cannot create reference directory");
            const QDir assetDir(targetDir.filePath(bundle));
            if (stack_.contains("external_refs")) {
                if (path_.isEmpty())
                    throw std::runtime_error("Missing original stack location");
                const auto sourceDir =
                    QFileInfo(QFileInfo(path_).absoluteDir().filePath(QString::fromStdString(
                                  data_.at("stack").at("ref").as<std::string>())))
                        .absoluteDir();
                for (const auto& ref : stack_.at("external_refs").array_range()) {
                    reference(ref, sourceDir, false);
                    const auto name = QString::fromStdString(
                        ref.at(ref.contains("ref") ? "ref" : "path").as<std::string>());
                    if (!QDir().mkpath(QFileInfo(assetDir.filePath(name)).absolutePath()) ||
                        !QFile::copy(sourceDir.filePath(name), assetDir.filePath(name)))
                        throw std::runtime_error("Cannot copy stack external reference");
                }
            }
            auto uniqueName = [&](const QString& preferred) {
                QString name = preferred;
                int n = 0;
                while (assetDir.exists(name))
                    name = QStringLiteral("case-%1-%2").arg(++n).arg(preferred);
                return name;
            };
            const auto stackName = uniqueName(QStringLiteral("stack.json"));
            const auto materialsName = uniqueName(QStringLiteral("materials.json"));
            write(assetDir.filePath(stackName), stack_);
            write(assetDir.filePath(materialsName), materials_);
            const QString names[] = {stackName, materialsName};
            int i = 0;
            for (const auto* key : {"stack", "materials"}) {
                const auto& name = names[i++];
                copy[key]["ref"] = (bundle + '/' + name).toStdString();
                if (copy[key].contains("sha256"))
                    copy[key]["sha256"] = digest(bytes(assetDir.filePath(name))).toStdString();
            }
        }
        auto& m = copy.at("mesh");
        if (!m.contains("max_ratio"))
            m["max_ratio"] = 2;
        if (!m.contains("cell_budget"))
            m["cell_budget"] = 50000;
        write(path, copy);
        data_ = copy;
        path_ = QFileInfo(path).absoluteFilePath();
        hash_ = digest(bytes(path));
        dirty_ = false;
        emit changed();
        emit dirtyChanged(false);
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::finalizeMeshForSubmit() {
    errors_ = validate();
    try {
        auto o = meshOptions();
        if (o.levels < 1)
            errors_.push_back(
                {"MESH_LEVELS_REQUIRED", "GUI submission requires explicit levels >= 1"});
        if (!errors_.empty())
            return false;
        setMeshOptions(o);
        return true;
    } catch (const std::exception& e) {
        errors_.push_back(exceptionError(e));
        return false;
    }
}
bool CaseDocument::setStackJson(const Json& value) {
    try {
        static const auto validator = jsoncons::jsonschema::make_json_schema(
            read(QStringLiteral(CHIPCAE_SCHEMA_DIR "/stack-0.2.schema.json")));
        if (!validator.is_valid(value))
            throw std::runtime_error("STACK_SCHEMA_INVALID");
        if (stack_ != value) {
            stack_ = value;
            markDirty();
        }
        errors_.clear();
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::setMaterialLibrary(const Json& value) {
    try {
        libraryCheck(value);
        errors_.clear();
        if (materials_ != value) {
            materials_ = value;
            markDirty();
        }
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::loadMaterialLibrary(const QString& path) {
    try {
        return setMaterialLibrary(read(path));
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::saveMaterialLibrary(const QString& path) {
    try {
        libraryCheck(materials_);
        write(path, materials_);
        errors_.clear();
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::applyRunProfile(const Json& value) {
    try {
        static const auto validator = jsoncons::jsonschema::make_json_schema(
            read(QStringLiteral(CHIPCAE_SCHEMA_DIR "/run-profile-1.0.schema.json")));
        if (!validator.is_valid(value))
            throw std::runtime_error("Invalid run profile");
        profile_ = value;
        errors_.clear();
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::loadRunProfile(const QString& path) {
    try {
        return applyRunProfile(read(path));
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::saveRunProfile(const QString& path) {
    try {
        if (!applyRunProfile(profile_))
            return false;
        write(path, profile_);
        return true;
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
}
bool CaseDocument::renameLayer(const std::string& oldName, const std::string& newName) {
    if (newName.empty() || stack_.empty())
        return false;
    auto copy = stack_;
    bool found = false;
    for (auto& layer : copy.at("layers").array_range()) {
        if (layer.at("name") == newName)
            return false;
        if (layer.at("name") == oldName) {
            layer["name"] = newName;
            found = true;
        }
    }
    if (!found)
        return false;
    auto update = [&](auto&& self, Json& node) -> void {
        if (node.is_string() && node == "layer:" + oldName)
            node = "layer:" + newName;
        else if (node.is_array())
            for (auto& child : node.array_range())
                self(self, child);
        else if (node.is_object())
            for (auto& member : node.object_range())
                if (member.key().substr(0, 2) != "x-")
                    self(self, member.value());
    };
    update(update, data_);
    update(update, copy);
    stack_ = copy;
    markDirty();
    return true;
}
} // namespace chipcae::desktop
