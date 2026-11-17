#include "SchemaValidator.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QUrl>
#include <fstream>
#include <jsoncons_ext/jsonpointer/jsonpointer.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace chipcae::desktop {
namespace {
jsoncons::uri schemaUri(const std::string& value) {
    const QUrl url(QString::fromStdString(value));
    // jsoncons emits file:/ during RFC resolution, but parses file:/// as a local file.
    // Canonicalize with Qt before using a file URI as a registry key.
    return jsoncons::uri(url.scheme() == "file"
                             ? QUrl::fromLocalFile(url.toLocalFile()).toString().toStdString()
                             : value);
}
} // namespace
SchemaValidator::~SchemaValidator() = default;
Json SchemaValidator::read(const QString& file) {
    std::ifstream stream(file.toStdString());
    if (!stream) {
        throw std::runtime_error("Schema/document file unavailable: " + file.toStdString());
    }
    return Json::parse(stream);
}
SchemaValidator::SchemaValidator(const QString& schemaFile) {
    const auto directory = QFileInfo(schemaFile).absolutePath();
    QStringList files{QFileInfo(schemaFile).absoluteFilePath()};
    for (const auto& folder : QStringList{directory, directory + "/../material/schema"}) {
        QDirIterator entries(folder, {"*.schema.json"}, QDir::Files);
        while (entries.hasNext()) {
            const auto file = entries.next();
            if (!files.contains(file)) {
                files.append(file);
            }
        }
    }
    for (const auto& file : files) {
        auto document = read(file);
        const auto retrieval = QUrl::fromLocalFile(file).toString().toStdString();
        const auto base = document.get_value_or<std::string>("$id", retrieval);
        // Give renderer references the same document context as the validator.
        std::function<void(Json&, const std::string&)> normalize = [&](Json& node,
                                                                       const std::string& context) {
            if (node.is_object()) {
                const auto local = node.get_value_or<std::string>("$id", context);
                if (node.contains("$ref")) {
                    const auto reference = node.at("$ref").as<std::string>();
                    node["$ref"] =
                        reference.starts_with("#")
                            ? local + reference
                            : jsoncons::uri(local).resolve(jsoncons::uri(reference)).string();
                }
                for (auto& property : node.object_range()) {
                    normalize(property.value(), local);
                }
            } else if (node.is_array()) {
                for (auto& child : node.array_range()) {
                    normalize(child, context);
                }
            }
        };
        normalize(document, base);
        documents_[schemaUri(retrieval)] = document;
        documents_[schemaUri(base)] = document;
        if (file == files.front()) {
            schema_ = document;
        }
    }
    auto resolver = [this](const jsoncons::uri& uri) -> Json {
        const auto reference = uri.string();
        const auto found = documents_.find(schemaUri(reference.substr(0, reference.find('#'))));
        if (found == documents_.end()) {
            throw std::runtime_error("Unresolved local schema: " + uri.string());
        }
        return found->second;
    };
    validator_ = std::make_unique<jsoncons::jsonschema::json_schema<Json>>(
        jsoncons::jsonschema::make_json_schema(
            schema_,
            QUrl::fromLocalFile(QFileInfo(schemaFile).absoluteFilePath()).toString().toStdString(),
            resolver));
}
QList<FieldError> SchemaValidator::validate(const Json& value) const {
    QList<FieldError> errors;
    std::function<void(const jsoncons::jsonschema::validation_message&)> append;
    append = [&](const auto& message) {
        errors.append({QString::fromStdString(message.instance_location().string()),
                       QString::fromStdString(message.message())});
        for (const auto& detail : message.details()) {
            append(detail);
        }
    };
    validator_->validate(value, [&](const auto& message) {
        append(message);
        return jsoncons::jsonschema::walk_result::advance;
    });
    return errors;
}
Json SchemaValidator::resolved(const Json& schema) const {
    auto result = schema;
    for (int depth = 0; result.is_object() && result.contains("$ref"); ++depth) {
        if (depth == 32) {
            throw std::runtime_error("Recursive schema cannot be rendered");
        }
        const auto ref = result.at("$ref").as<std::string>();
        const auto split = ref.find('#');
        const auto base = ref.substr(0, split);
        const auto fragment = split == std::string::npos ? "" : ref.substr(split + 1);
        const Json* document = &schema_;
        if (!base.empty()) {
            auto found = documents_.find(schemaUri(base));
            if (found == documents_.end()) {
                throw std::runtime_error("Unsupported editor reference: " + ref);
            }
            document = &found->second;
        }
        result = fragment.empty() ? *document : jsoncons::jsonpointer::get(*document, fragment);
    }
    return result;
}
} // namespace chipcae::desktop
