#pragma once
#include <QString>
#include <QStringList>
#include <jsoncons/json.hpp>
#include <jsoncons/utility/uri.hpp>
namespace jsoncons::jsonschema {
template <typename Json>
class json_schema;
}
#include <map>
#include <memory>

namespace chipcae::desktop {
using Json = jsoncons::json;
struct FieldError {
    QString path;
    QString message;
};
class SchemaValidator {
public:
    explicit SchemaValidator(const QString& schemaFile);
    ~SchemaValidator();
    QList<FieldError> validate(const Json& value) const;
    const Json& schema() const { return schema_; }
    Json resolved(const Json& schema) const;
    static Json read(const QString& file);

private:
    Json schema_;
    std::map<jsoncons::uri, Json> documents_;
    std::unique_ptr<jsoncons::jsonschema::json_schema<Json>> validator_;
};
} // namespace chipcae::desktop
