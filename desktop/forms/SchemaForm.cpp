#include "SchemaForm.hpp"

#include "DesktopConfig.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <jsoncons_ext/jsonpointer/jsonpointer.hpp>

namespace chipcae::desktop {
namespace {
QString escaped(QString key) {
    return key.replace("~", "~0").replace("/", "~1");
}
QString text(const Json& value) {
    return QString::fromStdString(value.is_string() ? value.as<std::string>() : value.to_string());
}
Json& at(Json& value, const QString& path) {
    return jsoncons::jsonpointer::get(value, path.toStdString());
}
const Json& at(const Json& value, const QString& path) {
    return jsoncons::jsonpointer::get(value, path.toStdString());
}
// Select an object variant by its present const discriminator, even while other fields are invalid.
Json variant(Json schema, const Json& value) {
    if (!schema.is_object() || !schema.contains("oneOf") ||
        !schema.at("oneOf").at(0).contains("properties")) {
        return schema;
    }
    for (const auto& branch : schema.at("oneOf").array_range()) {
        if (!branch.contains("properties") || !value.is_object()) {
            continue;
        }
        for (const auto& property : branch.at("properties").object_range()) {
            if (property.value().contains("const") && value.contains(property.key()) &&
                value.at(property.key()) == property.value().at("const")) {
                return branch;
            }
        }
    }
    for (const auto& branch : schema.at("oneOf").array_range()) {
        if (branch.contains("properties") && branch.contains("required") && value.is_object()) {
            bool matches = true;
            for (const auto& required : branch.at("required").array_range()) {
                matches = matches && value.contains(required.as<std::string>());
            }
            if (matches) {
                return branch;
            }
        }
    }
    return schema.at("oneOf").at(0);
}
} // namespace
SchemaForm::SchemaForm(QWidget* parent) : QWidget(parent), layout_(new QVBoxLayout(this)) {
    messages_ = new QListWidget(this);
    messages_->setObjectName("validationErrors");
    messages_->setMaximumHeight(160);
    layout_->addWidget(messages_);
}
QString SchemaForm::caseSchemaPath(const QString& repository) {
    const auto current = repository + "/schema/case.schema.json";
    return QFileInfo::exists(current) ? current : repository + "/schema/case-1.0.schema.json";
}
QString SchemaForm::resultSchemaPath(const QString& repository) {
    return repository + "/schema/result.schema.json";
}
QString SchemaForm::caseSchemaPath(const QString& repository,
                                   const QStringList& args,
                                   const QProcessEnvironment& env,
                                   const QString& executableDir,
                                   const QString& compileTimeFallback) {
    const auto path = DesktopConfig::locateSchemaFile(
        "case-1.0.schema.json", args, env, executableDir, compileTimeFallback);
    return path.isEmpty() ? caseSchemaPath(repository) : path;
}
QString SchemaForm::resultSchemaPath(const QString& repository,
                                     const QStringList& args,
                                     const QProcessEnvironment& env,
                                     const QString& executableDir,
                                     const QString& compileTimeFallback) {
    const auto path = DesktopConfig::locateSchemaFile(
        "result.schema.json", args, env, executableDir, compileTimeFallback);
    return path.isEmpty() ? resultSchemaPath(repository) : path;
}
bool SchemaForm::load(const QString& schemaFile, const Json& document) {
    validator_.reset();
    document_ = document;
    path_.clear();
    inputErrors_.clear();
    try {
        validator_ = std::make_unique<SchemaValidator>(schemaFile);
        rebuild();
        validate();
        return true;
    } catch (const std::exception& error) {
        validator_.reset();
        if (fields_) {
            delete fields_;
            fields_ = nullptr;
        }
        errors_ = {{"", QString::fromUtf8(error.what())}};
        messages_->clear();
        messages_->addItem("/: " + errors_.front().message);
        emit validationChanged();
        return false;
    }
}
void SchemaForm::setObjectPath(const QString& pointer) {
    if (!validator_) {
        return;
    }
    try {
        at(document_, pointer);
        path_ = pointer;
        rebuild();
    } catch (const std::exception&) {
        // An outline selection can become stale after removing an array item.
        path_.clear();
        rebuild();
    }
}
bool SchemaForm::setValue(const QString& pointer, const Json& value) {
    if (!validator_) {
        return false;
    }
    try {
        at(document_, pointer) = value;
        inputErrors_.erase(pointer);
        validate();
        emit documentChanged();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
void SchemaForm::validate() {
    errors_ = validator_->validate(document_);
    for (const auto& [path, message] : inputErrors_) {
        errors_.append({path, message});
    }
    messages_->clear();
    for (const auto& error : errors_) {
        messages_->addItem((error.path.isEmpty() ? "/" : error.path) + ": " + error.message);
    }
    messages_->setToolTip("JSON pointer paths; values use the schema's stored units (SI / K).");
    emit validationChanged();
}
Json SchemaForm::schemaAt(const QString& path) const {
    auto schema = validator_->schema();
    Json value = document_;
    for (const auto& token : path.split('/', Qt::SkipEmptyParts)) {
        const auto key = QString(token).replace("~1", "/").replace("~0", "~").toStdString();
        schema = variant(validator_->resolved(schema), value);
        Json childSchema;
        Json childValue;
        if (value.is_array()) {
            const auto index = static_cast<std::size_t>(std::stoull(key));
            if (schema.contains("prefixItems") && index < schema.at("prefixItems").size()) {
                childSchema = schema.at("prefixItems").at(index);
            } else {
                childSchema = schema.contains("items") ? schema.at("items") : Json::object();
            }
            childValue = value.at(index);
        } else {
            childSchema = schema.contains("properties") && schema.at("properties").contains(key)
                              ? schema.at("properties").at(key)
                              : Json::object();
            childValue = value.at(key);
        }
        // jsoncons assignments from a descendant must not invalidate their own source.
        schema = std::move(childSchema);
        value = std::move(childValue);
    }
    return schema;
}
Json SchemaForm::seed(const Json& input) const {
    auto schema = validator_->resolved(input);
    if (!schema.is_object()) {
        return Json();
    }
    if (schema.contains("default")) {
        return schema.at("default");
    }
    if (schema.contains("const")) {
        return schema.at("const");
    }
    if (schema.contains("enum")) {
        return schema.at("enum").at(0);
    }
    if (schema.contains("oneOf") && schema.at("oneOf").at(0).contains("properties")) {
        return seed(schema.at("oneOf").at(0));
    }
    const auto type = schema.get_value_or<std::string>("type", "");
    if (type == "object" || schema.contains("properties")) {
        Json value = Json::object();
        if (schema.contains("required") && schema.contains("properties")) {
            for (const auto& key : schema.at("required").array_range()) {
                const auto name = key.as<std::string>();
                value[name] = schema.at("properties").contains(name)
                                  ? seed(schema.at("properties").at(name))
                                  : Json();
            }
        }
        return value;
    }
    if (type == "array") {
        return Json::array();
    }
    if (type == "boolean") {
        return Json(false);
    }
    if (type == "number" || type == "integer") {
        return schema.contains("minimum") ? schema.at("minimum") : Json(0);
    }
    return Json("");
}
void SchemaForm::scheduleRebuild() {
    QTimer::singleShot(0, this, [this] {
        if (validator_) {
            setObjectPath(path_);
        }
    });
}
void SchemaForm::rebuild() {
    inputErrors_.clear();
    delete fields_;
    auto* scroll = new QScrollArea(this);
    fields_ = scroll;
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* rows = new QVBoxLayout(content);
    scroll->setWidget(content);
    layout_->insertWidget(0, scroll, 1);
    render(rows, schemaAt(path_), path_);
    rows->addStretch();
    if (validator_)
        validate();
}
void SchemaForm::render(QVBoxLayout* rows, const Json& input, const QString& path, int depth) {
    if (depth > 32) {
        rows->addWidget(new QLabel(path + ": recursive schema editing is unsupported"));
        return;
    }
    auto schema = validator_->resolved(input);
    const auto value = at(document_, path);
    if (schema.is_object() && schema.contains("oneOf") &&
        schema.at("oneOf").at(0).contains("properties")) {
        auto* choice = new QComboBox;
        choice->setObjectName(path + "/@variant");
        int selected = 0;
        const auto active = variant(schema, value);
        for (std::size_t i = 0; i < schema.at("oneOf").size(); ++i) {
            const auto& branch = schema.at("oneOf").at(i);
            QString label = QString("Variant %1").arg(i + 1);
            for (const auto& property : branch.at("properties").object_range()) {
                if (property.value().contains("const")) {
                    label = text(property.value().at("const"));
                    break;
                }
            }
            choice->addItem(label);
            if (branch == active) {
                selected = static_cast<int>(i);
            }
        }
        choice->setCurrentIndex(selected);
        rows->addWidget(choice);
        connect(choice, &QComboBox::activated, this, [this, schema, path](int index) {
            auto replacement = seed(schema.at("oneOf").at(index));
            const auto& old = at(document_, path);
            if (old.is_object() && replacement.is_object()) {
                for (const auto& property : old.object_range()) {
                    if (property.key().starts_with("x-")) {
                        replacement[property.key()] = property.value();
                    }
                }
            }
            setValue(path, replacement);
            scheduleRebuild();
        });
        schema = active;
    }
    if (value.is_object() && schema.is_object()) {
        auto properties = schema.contains("properties") ? schema.at("properties") : Json::object();
        if (schema.contains("required")) {
            for (const auto& required : schema.at("required").array_range()) {
                const auto key = required.as<std::string>();
                if (!properties.contains(key)) {
                    properties[key] = Json::object();
                }
            }
        }
        for (const auto& item : value.object_range()) {
            if (!properties.contains(item.key())) {
                properties[item.key()] = Json::object();
            }
        }
        for (const auto& property : properties.object_range()) {
            const auto key = property.key();
            const auto name = QString::fromStdString(key);
            if (name.startsWith("x-") &&
                !property.value().get_value_or<bool>("x-chipcae-visible", false)) {
                continue;
            }
            const auto childPath = path + "/" + escaped(name);
            auto* box = new QGroupBox(childPath);
            auto* children = new QVBoxLayout(box);
            auto* present = new QCheckBox("Present");
            present->setObjectName(childPath + "/@present");
            present->setChecked(value.contains(key));
            children->addWidget(present);
            connect(present,
                    &QCheckBox::toggled,
                    this,
                    [this, path, key, childSchema = property.value()](bool enabled) {
                        auto& parent = at(document_, path);
                        if (enabled) {
                            parent[key] = seed(childSchema);
                        } else {
                            parent.erase(key);
                        }
                        inputErrors_.clear();
                        validate();
                        emit documentChanged();
                        scheduleRebuild();
                    });
            if (value.contains(key)) {
                render(children, property.value(), childPath, depth + 1);
            } else if (property.value().contains("default")) {
                children->addWidget(new QLabel("Default: " + text(property.value().at("default"))));
            }
            rows->addWidget(box);
        }
        return;
    }
    if (value.is_array() && schema.is_object()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            auto* box = new QGroupBox(path + "/" + QString::number(i));
            auto* children = new QVBoxLayout(box);
            Json item = schema.contains("items") ? schema.at("items") : Json::object();
            if (schema.contains("prefixItems") && i < schema.at("prefixItems").size()) {
                item = schema.at("prefixItems").at(i);
            }
            render(children, item, path + "/" + QString::number(i), depth + 1);
            auto* remove = new QPushButton("Remove");
            remove->setObjectName(path + "/" + QString::number(i) + "/@remove");
            children->addWidget(remove);
            connect(remove, &QPushButton::clicked, this, [this, path, i] {
                auto& array = at(document_, path);
                array.erase(array.array_range().begin() + i);
                inputErrors_.clear();
                validate();
                emit documentChanged();
                scheduleRebuild();
            });
            rows->addWidget(box);
        }
        auto* add = new QPushButton("Add item");
        add->setObjectName(path + "/@add");
        rows->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this, path, schema] {
            auto& array = at(document_, path);
            auto item = schema.contains("items") ? schema.at("items") : Json::object();
            if (schema.contains("prefixItems") && array.size() < schema.at("prefixItems").size()) {
                item = schema.at("prefixItems").at(array.size());
            }
            array.push_back(seed(item));
            validate();
            emit documentChanged();
            scheduleRebuild();
        });
        return;
    }
    if (schema.is_object() && schema.contains("const")) {
        auto* editor = new QLineEdit(text(value));
        editor->setReadOnly(true);
        editor->setObjectName(path);
        editor->setToolTip("Required constant: " + text(schema.at("const")));
        rows->addWidget(editor);
    } else if (schema.is_object() && schema.contains("enum")) {
        auto* editor = new QComboBox;
        editor->setObjectName(path);
        int selected = -1;
        int i = 0;
        for (const auto& option : schema.at("enum").array_range()) {
            editor->addItem(text(option));
            if (option == value) {
                selected = i;
            }
            ++i;
        }
        if (selected < 0) {
            editor->addItem("Invalid: " + text(value));
            selected = i;
        }
        editor->setCurrentIndex(selected);
        rows->addWidget(editor);
        connect(editor, &QComboBox::activated, this, [this, path, schema](int index) {
            if (static_cast<std::size_t>(index) < schema.at("enum").size()) {
                setValue(path, schema.at("enum").at(index));
            }
        });
    } else {
        auto* editor = new QLineEdit(text(value));
        editor->setObjectName(path);
        if (schema.is_object() && schema.contains("description")) {
            editor->setToolTip(text(schema.at("description")));
        }
        rows->addWidget(editor);
        const auto type = schema.is_object() ? schema.get_value_or<std::string>("type", "") : "";
        connect(
            editor, &QLineEdit::textEdited, this, [this, path, type, value](const QString& input) {
                try {
                    auto parsed = type == "string" || (type.empty() && value.is_string())
                                      ? Json(input.toStdString())
                                      : Json::parse(input.toStdString());
                    setValue(path, parsed);
                } catch (const std::exception& error) {
                    inputErrors_[path] = QString("Invalid JSON value: %1").arg(error.what());
                    validate();
                }
            });
    }
}
} // namespace chipcae::desktop
