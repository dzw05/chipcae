#pragma once
#include <QObject>
#include <QString>
#include <jsoncons/json.hpp>
#include <optional>
#include <vector>

namespace chipcae::desktop {
using Json = jsoncons::json;
struct DocumentError {
    QString code;
    QString message;
};
using DocumentErrors = std::vector<DocumentError>;
struct MeshOptions {
    std::optional<int64_t> nx, ny;
    double maxRatio = 2;
    int64_t levels = 0;
    int64_t cellBudget = 50000;
};
// D10 supplies digest extraction from preview/VTKHDF; no alternate hash algorithm here.
struct MeshDigest {
    int64_t nx0, ny0, L, padded_cells, nz;
    std::optional<int64_t> npoints, ncells;
    std::string mesh_sha256;
    bool operator==(const MeshDigest&) const = default;
    static MeshDigest fromJson(const Json& value);
};
class CaseDocument : public QObject {
    Q_OBJECT
public:
    explicit CaseDocument(QObject* parent = nullptr);
    bool load(const QString& path);
    bool checkImportedStack(const Json& stack) const;
    bool replaceImportedStack(const Json& stack);
    bool save(const QString& path);
    DocumentErrors validate() const;
    bool isDirty() const { return dirty_; }
    QByteArray contentHash() const { return hash_; }
    const DocumentErrors& errors() const { return errors_; }
    Json toJson() const { return data_; }
    void applyJson(const Json& value);
    // JSON Pointer mutation and grouping hook for D05 / D11; no undo stack.
    void setValue(const std::string& pointer, const Json& value);
    void beginEdit();
    MeshOptions meshOptions() const;
    void setMeshOptions(const MeshOptions& options);
    bool finalizeMeshForSubmit();
    bool loadMaterialLibrary(const QString& path);
    bool saveMaterialLibrary(const QString& path);
    bool setMaterialLibrary(const Json& value);
    Json materialLibrary() const { return materials_; }
    Json stackJson() const { return stack_; }
    bool loadRunProfile(const QString& path);
    bool saveRunProfile(const QString& path);
    Json runProfile() const { return profile_; }
    bool applyRunProfile(const Json& value);
    bool renameLayer(const std::string& oldName, const std::string& newName);
signals:
    void changed();
    void dirtyChanged(bool dirty);
    void editStarted();

private:
    void markDirty();
    Json data_ = Json::object(), materials_ = Json::object(), stack_ = Json::object();
    Json profile_ = Json::object();
    QString path_;
    QByteArray hash_;
    bool dirty_ = false;
    DocumentErrors errors_;
};
} // namespace chipcae::desktop
