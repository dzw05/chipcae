#include "CaseDocument.hpp"

#include "SchemaValidator.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <jsoncons_ext/jsonpointer/jsonpointer.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#include <map>
#include <set>
#include <stdexcept>

namespace chipcae::desktop {
namespace {
QByteArray bytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(f.errorString().toStdString());
    auto raw = f.readAll();
    if (f.error() != QFileDevice::NoError)
        throw std::runtime_error(f.errorString().toStdString());
    return raw;
}
Json read(const QString& path) {
    return Json::parse(bytes(path).toStdString());
}
QByteArray digest(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
QByteArray write(const QString& path, const Json& value) {
    const auto text = QByteArray::fromStdString(value.to_string());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
    return text;
}
struct LocatedError : std::runtime_error {
    QString code;
    QString pointer;
    QString source;
    LocatedError(const char* errorCode, const QString& message, QString path, QString document)
        : std::runtime_error(message.toStdString()), code(QString::fromLatin1(errorCode)),
          pointer(std::move(path)), source(std::move(document)) {}
};
void libraryCheck(const Json& value) {
    if (value.at("schema") != "chipcae.material_library/1" || !value.at("materials").is_array())
        throw std::runtime_error("Invalid material library container");
    std::set<std::string> ids;
    for (size_t i = 0; i < value.at("materials").size(); ++i) {
        const auto& item = value.at("materials").at(i);
        if (!ids.insert(item.at("id").as<std::string>()).second)
            throw LocatedError("MATERIAL_ID_DUPLICATE",
                               "MATERIAL_ID_DUPLICATE",
                               QString("/materials/%1/id").arg(i),
                               "materials");
    }
}

bool isContained(const QString& root, const QString& path) {
    return !root.isEmpty() && path.startsWith(root.endsWith('/') ? root : root + '/');
}

QByteArray referenceHash(const Json& ref) {
    return QByteArray::fromStdString(ref.at("sha256").as<std::string>()).toLower();
}

Json reference(const Json& ref,
               const QDir& dir,
               bool parse = true,
               const QString& pointer = {},
               const QString& source = "case") {
    const auto* field = ref.contains("ref") ? "ref" : "path";
    const auto name = QString::fromStdString(ref.at(field).as<std::string>());
    const auto pathPointer = pointer.isEmpty() ? QString() : pointer + '/' + field;
    if (name.isEmpty() || name.contains(QChar::Null) || QDir::isAbsolutePath(name) ||
        name.contains('\\') || name.contains(':') || name.split('/').contains(".."))
        throw LocatedError("INPUT_REF_INVALID", "Invalid reference path", pathPointer, source);
    const QFileInfo file(dir.filePath(name));
    const auto canonical = file.canonicalFilePath();
    if (!canonical.isEmpty() && !isContained(dir.canonicalPath(), canonical))
        throw LocatedError(
            "INPUT_REF_INVALID", "Reference escapes document directory", pathPointer, source);
    if (canonical.isEmpty() || !file.isFile())
        throw LocatedError("INPUT_REF_MISSING",
                           "Reference is missing or is not a regular file: " + name,
                           pathPointer,
                           source);
    QByteArray raw;
    try {
        raw = bytes(canonical);
    } catch (const std::exception& e) {
        throw LocatedError("INPUT_REF_MISSING", QString::fromUtf8(e.what()), pathPointer, source);
    }
    if (ref.contains("sha256") && digest(raw) != referenceHash(ref))
        throw LocatedError("INPUT_HASH_MISMATCH",
                           "INPUT_HASH_MISMATCH",
                           pointer.isEmpty() ? QString() : pointer + "/sha256",
                           source);
    return parse ? Json::parse(raw.toStdString()) : Json::object();
}
DocumentError exceptionError(const std::exception& e) {
    const auto message = QString::fromUtf8(e.what());
    if (const auto* located = dynamic_cast<const LocatedError*>(&e))
        return {located->code, message, located->pointer, located->source};
    return {"DOCUMENT_ERROR", message, {}};
}

QDir stackDirectory(const QString& casePath, const Json& data) {
    const auto root = QFileInfo(casePath).absoluteDir();
    const auto name = QString::fromStdString(data.at("stack").at("ref").as<std::string>());
    return QFileInfo(root.filePath(name)).absoluteDir();
}

void validateCaseReferences(const Json& data, const QDir& root) {
    reference(data.at("stack"), root);
    reference(data.at("materials"), root);
}

QString externalRefPointer(size_t index) {
    return QString("/external_refs/%1").arg(index);
}

struct ExternalAsset {
    QString path;
    QByteArray hash;
    std::vector<size_t> indices;
};

std::vector<ExternalAsset> externalAssets(const Json& stack) {
    std::vector<ExternalAsset> assets;
    if (!stack.contains("external_refs"))
        return assets;
    std::map<std::string, size_t> paths;
    const auto& refs = stack.at("external_refs");
    for (size_t i = 0; i < refs.size(); ++i) {
        const auto& ref = refs.at(i);
        const auto path = ref.at("path").as<std::string>();
        const auto hash = referenceHash(ref);
        const auto [entry, inserted] = paths.try_emplace(path, assets.size());
        if (inserted) {
            assets.push_back({QString::fromStdString(path), hash, {i}});
        } else {
            auto& asset = assets.at(entry->second);
            if (asset.hash != hash)
                throw LocatedError("INPUT_REF_CONFLICT",
                                   "Conflicting hashes for external reference: " + asset.path,
                                   externalRefPointer(i) + "/sha256",
                                   "stack");
            asset.indices.push_back(i);
        }
    }
    return assets;
}

void validateExternalAssets(const Json& stack,
                            const QDir& root,
                            const std::vector<ExternalAsset>& assets) {
    for (const auto& asset : assets) {
        const auto index = asset.indices.front();
        reference(
            stack.at("external_refs").at(index), root, false, externalRefPointer(index), "stack");
    }
}

QString uniqueName(const QString& preferred, const auto& available) {
    auto name = preferred;
    for (int n = 1; !available(name); ++n)
        name = QStringLiteral("case-%1-%2").arg(n).arg(preferred);
    return name;
}

struct CreatedAssets {
    QStringList files;
    QStringList directories;

    ~CreatedAssets() {
        for (const auto& file : files)
            QFile::remove(file);
        for (auto it = directories.crbegin(); it != directories.crend(); ++it)
            QDir().rmdir(*it);
    }

    void keep() {
        files.clear();
        directories.clear();
    }
};

QString destinationParent(const QString& root, const QString& name, CreatedAssets& created) {
    auto segments = QDir::cleanPath(name).split('/', Qt::SkipEmptyParts);
    segments.removeLast();
    auto parent = root;
    for (const auto& segment : segments) {
        const QDir dir(parent);
        QFileInfo next(dir.filePath(segment));
        if (!next.exists() && !next.isSymLink() && dir.mkdir(segment)) {
            created.directories.append(next.absoluteFilePath());
            next.refresh();
        }
        const auto canonical = next.canonicalFilePath();
        if (!next.isDir() || (canonical != root && !isContained(root, canonical)))
            throw std::runtime_error("Cannot use external reference destination directory");
        parent = canonical;
    }
    return parent;
}

void relocateExternalAssets(Json& stack,
                            const QDir& source,
                            const QDir& destination,
                            const std::vector<ExternalAsset>& assets,
                            CreatedAssets& created) {
    const auto root = destination.canonicalPath();
    for (const auto& asset : assets) {
        const auto pointer = externalRefPointer(asset.indices.front());
        try {
            if (root.isEmpty())
                throw std::runtime_error("Missing external reference destination directory");
            bool reuse = false;
            const auto name = uniqueName(asset.path, [&](const QString& candidate) {
                const QFileInfo file(destination.filePath(candidate));
                if (!file.exists() && !file.isSymLink())
                    return true;
                const auto canonical = file.canonicalFilePath();
                if (file.isFile() && isContained(root, canonical)) {
                    try {
                        reuse = digest(bytes(canonical)) == asset.hash;
                    } catch (const std::runtime_error&) {
                        // An unreadable destination is occupied, just like a different digest.
                    }
                }
                return reuse;
            });
            const auto parent = destinationParent(root, name, created);
            if (!reuse) {
                const auto path = QDir(parent).filePath(QFileInfo(name).fileName());
                QFile sourceFile(source.filePath(asset.path));
                if (!sourceFile.copy(path))
                    throw std::runtime_error(sourceFile.errorString().toStdString());
                created.files.append(path);
                auto relocated = stack.at("external_refs").at(asset.indices.front());
                relocated["path"] = name.toStdString();
                reference(relocated, destination, false, pointer, "stack");
            }
            if (name != asset.path)
                for (const auto index : asset.indices)
                    stack["external_refs"][index]["path"] = name.toStdString();
        } catch (const std::exception& e) {
            throw LocatedError("INPUT_REF_COPY_FAILED",
                               "Cannot relocate " + asset.path + ": " + QString::fromUtf8(e.what()),
                               pointer + "/path",
                               "stack");
        }
    }
}
} // namespace

struct CaseDocument::StackImport {
    DocumentErrors issues;
    QString sourceRoot;
    std::vector<ExternalAsset> assets;
};
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
CaseDocument::StackImport CaseDocument::preflightImportedStack(const Json& stack,
                                                               const QString& sourceDir) const {
    StackImport result;
    try {
        if (stack.empty())
            throw LocatedError("INPUT_STACK_EMPTY", "Imported stack is empty", {}, "stack");
        const bool needsSource = stack.contains("external_refs") &&
                                 stack.at("external_refs").is_array() &&
                                 !stack.at("external_refs").empty();
        if (!sourceDir.isEmpty()) {
            const QFileInfo source(QDir::current().absoluteFilePath(sourceDir));
            if (source.isDir())
                result.sourceRoot = source.canonicalFilePath();
        }
        if (result.sourceRoot.isEmpty() && (needsSource || !sourceDir.isEmpty()))
            throw LocatedError("INPUT_REF_SOURCE_MISSING",
                               "sourceDir must identify an existing directory for external_refs",
                               "/external_refs",
                               "stack");

        CaseDocument candidate;
        candidate.data_ = data_;
        candidate.materials_ = materials_;
        candidate.stack_ = stack;
        candidate.validateSchemas(result.issues);
        if (result.issues.empty()) {
            result.assets = externalAssets(stack);
            validateExternalAssets(stack, QDir(result.sourceRoot), result.assets);
            candidate.validateContents(result.issues);
        }
    } catch (const std::exception& e) {
        result.issues.push_back(exceptionError(e));
    }
    return result;
}

bool CaseDocument::checkImportedStack(const Json& stack, const QString& sourceDir) const {
    return importedStackIssues(stack, sourceDir).empty();
}
bool CaseDocument::replaceImportedStack(const Json& stack, const QString& sourceDir) {
    auto import = preflightImportedStack(stack, sourceDir);
    errors_ = std::move(import.issues);
    if (!errors_.empty())
        return false;

    CreatedAssets created;
    try {
        auto staged = stack;
        if (!path_.isEmpty() && !import.assets.empty()) {
            validateCaseReferences(data_, QFileInfo(path_).absoluteDir());
            relocateExternalAssets(staged,
                                   QDir(import.sourceRoot),
                                   stackDirectory(path_, data_),
                                   import.assets,
                                   created);
        }
        for (const auto& asset : import.assets) {
            const auto hash = asset.hash.toStdString();
            for (const auto index : asset.indices)
                staged["external_refs"][index]["sha256"] = hash;
        }
        stack_ = std::move(staged);
        importedStackSourceRoot_ = path_.isEmpty() ? std::move(import.sourceRoot) : QString();
        created.keep();
    } catch (const std::exception& e) {
        errors_ = {exceptionError(e)};
        return false;
    }
    markDirty();
    return true;
}

DocumentErrors CaseDocument::importedStackIssues(const Json& stack,
                                                 const QString& sourceDir) const {
    return preflightImportedStack(stack, sourceDir).issues;
}
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
                    const auto stackDir = stackDirectory(path_, value);
                    for (const auto& ref : stack.at("external_refs").array_range())
                        reference(ref, stackDir, false);
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
void CaseDocument::applyStackJson(const Json& value) {
    if (stack_ != value) {
        stack_ = value;
        markDirty();
    }
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
        validateSchemas(result);
        if (result.empty())
            validateContents(result);
    } catch (const std::exception& e) {
        result.push_back(exceptionError(e));
    }
    return result;
}

void CaseDocument::validateSchemas(DocumentErrors& result) const {
    static const SchemaValidator validator(
        QStringLiteral(CHIPCAE_SCHEMA_DIR "/case-1.0.schema.json"));
    for (const auto& error : validator.validate(data_))
        result.push_back({"CASE_SCHEMA_INVALID", error.message, error.path});
    if (!stack_.empty()) {
        static const SchemaValidator stackValidator(
            QStringLiteral(CHIPCAE_SCHEMA_DIR "/stack-0.2.schema.json"));
        for (const auto& error : stackValidator.validate(stack_))
            result.push_back({"STACK_SCHEMA_INVALID", error.message, error.path, "stack"});
    }
}

void CaseDocument::validateContents(DocumentErrors& result) const {
    if (!path_.isEmpty()) {
        validateCaseReferences(data_, QFileInfo(path_).absoluteDir());
        if (stack_.contains("external_refs")) {
            const auto stackDir = stackDirectory(path_, data_);
            for (const auto& ref : stack_.at("external_refs").array_range())
                reference(ref, stackDir, false);
        }
    }
    std::set<std::string> stableIds;
    auto checkIds = [&](auto&& self, const Json& node, const std::string& pointer) -> void {
        if (node.is_object()) {
            if (node.contains("id") && node.at("id").is_string() &&
                !stableIds.insert(node.at("id").as<std::string>()).second)
                result.push_back({"CASE_ID_DUPLICATE",
                                  "Duplicate case item id",
                                  QString::fromStdString(pointer + "/id")});
            for (const auto& member : node.object_range())
                if (member.key().substr(0, 2) != "x-")
                    self(self, member.value(), pointer + "/" + member.key());
        } else if (node.is_array())
            for (size_t i = 0; i < node.size(); ++i)
                self(self, node.at(i), pointer + "/" + std::to_string(i));
    };
    if (data_.contains("loads"))
        checkIds(checkIds, data_.at("loads"), "/loads");
    if (data_.contains("constraints"))
        checkIds(checkIds, data_.at("constraints"), "/constraints");
    if (!stack_.empty()) {
        std::set<std::string> layers;
        for (size_t i = 0; i < stack_.at("layers").size(); ++i)
            if (!layers.insert(stack_.at("layers").at(i).at("name").as<std::string>()).second)
                result.push_back({"LAYER_NAME_DUPLICATE",
                                  "Duplicate stack layer name",
                                  QString("/layers/%1/name").arg(i),
                                  "stack"});
        auto targets = [&](auto&& self, const Json& node, const std::string& pointer) -> void {
            if (node.is_object()) {
                if (node.contains("target")) {
                    const auto target = node.at("target").as<std::string>();
                    if (target.starts_with("layer:") && !layers.contains(target.substr(6)))
                        result.push_back({"TARGET_LAYER_INVALID",
                                          "Unknown target layer",
                                          QString::fromStdString(pointer + "/target")});
                }
                for (const auto& member : node.object_range())
                    if (member.key().substr(0, 2) != "x-")
                        self(self, member.value(), pointer + "/" + member.key());
            } else if (node.is_array())
                for (size_t i = 0; i < node.size(); ++i)
                    self(self, node.at(i), pointer + "/" + std::to_string(i));
        };
        targets(targets, data_, "");
    }
    if (!materials_.empty())
        libraryCheck(materials_);
    if (!stack_.empty() && !materials_.empty()) {
        std::set<std::string> ids;
        for (const auto& m : materials_.at("materials").array_range())
            ids.insert(m.at("id").as<std::string>());
        for (size_t i = 0; i < stack_.at("layers").size(); ++i) {
            const auto& layer = stack_.at("layers").at(i);
            if (layer.contains("material") && !ids.contains(layer.at("material").as<std::string>()))
                result.push_back({"MATERIAL_REFERENCE_INVALID",
                                  "Unknown layer material id",
                                  QString("/layers/%1/material").arg(i),
                                  "stack"});
            if (layer.contains("regions"))
                for (size_t j = 0; j < layer.at("regions").size(); ++j)
                    if (!ids.contains(layer.at("regions").at(j).at("material").as<std::string>()))
                        result.push_back({"MATERIAL_REFERENCE_INVALID",
                                          "Unknown region material id",
                                          QString("/layers/%1/regions/%2/material").arg(i).arg(j),
                                          "stack"});
        }
    }
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
            const auto stackDir = stackDirectory(path, candidate.data_);
            for (const auto& ref : candidate.stack_.at("external_refs").array_range())
                reference(ref, stackDir, false);
        }
        errors_ = candidate.validate();
        if (!errors_.empty())
            return false;
        data_ = candidate.data_;
        materials_ = candidate.materials_;
        stack_ = candidate.stack_;
        path_ = QFileInfo(path).absoluteFilePath();
        importedStackSourceRoot_.clear();
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
        auto stagedStack = stack_;
        auto savedPath = QFileInfo(path).absoluteFilePath();
        CreatedAssets created;
        if (!stack_.empty() && !materials_.empty()) {
            const auto targetDir = QFileInfo(path).absoluteDir();
            const auto bundle = "case-assets-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!targetDir.mkpath(".") || !targetDir.mkdir(bundle))
                throw std::runtime_error("Cannot create reference directory");
            const QDir assetDir(targetDir.filePath(bundle));
            created.directories.append(assetDir.absolutePath());
            const auto assets = externalAssets(stack_);
            if (!assets.empty()) {
                if (path_.isEmpty() && importedStackSourceRoot_.isEmpty())
                    throw std::runtime_error("Missing original stack location");
                const auto sourceDir =
                    path_.isEmpty() ? QDir(importedStackSourceRoot_) : stackDirectory(path_, data_);
                if (path_.isEmpty())
                    validateExternalAssets(stack_, sourceDir, assets);
                relocateExternalAssets(stagedStack, sourceDir, assetDir, assets, created);
            }
            const auto available = [&](const QString& name) { return !assetDir.exists(name); };
            const auto writeReference = [&](const char* key, const Json& value) {
                const auto name = uniqueName(QString::fromLatin1(key) + ".json", available);
                const auto assetPath = assetDir.filePath(name);
                const auto raw = write(assetPath, value);
                created.files.append(assetPath);
                copy[key]["ref"] = (bundle + '/' + name).toStdString();
                if (copy[key].contains("sha256"))
                    copy[key]["sha256"] = digest(raw).toStdString();
            };
            writeReference("stack", stagedStack);
            writeReference("materials", materials_);
        }
        auto& m = copy.at("mesh");
        if (!m.contains("max_ratio"))
            m["max_ratio"] = 2;
        if (!m.contains("cell_budget"))
            m["cell_budget"] = 50000;
        auto savedHash = digest(write(savedPath, copy));
        created.keep();
        data_ = std::move(copy);
        stack_ = std::move(stagedStack);
        path_ = std::move(savedPath);
        hash_ = std::move(savedHash);
        importedStackSourceRoot_.clear();
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
            errors_.push_back({"MESH_LEVELS_REQUIRED",
                               "GUI submission requires explicit levels >= 1",
                               "/mesh/levels"});
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
        static const SchemaValidator validator(
            QStringLiteral(CHIPCAE_SCHEMA_DIR "/run-profile-1.0.schema.json"));
        errors_.clear();
        for (const auto& error : validator.validate(value))
            errors_.push_back({"DOCUMENT_ERROR", error.message, error.path, "run-profile"});
        if (!errors_.empty())
            return false;
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
        else if (node.is_array()) {
            for (auto& child : node.array_range())
                self(self, child);
        } else if (node.is_object())
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
