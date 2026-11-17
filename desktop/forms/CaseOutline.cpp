#include "CaseOutline.hpp"

#include <QStandardItemModel>

namespace chipcae::desktop {
CaseOutline::CaseOutline(SchemaForm* form, QWidget* parent)
    : QTreeView(parent), form_(form), model_(new QStandardItemModel(this)) {
    setModel(model_);
    setHeaderHidden(true);
    connect(selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& index) {
                if (!index.isValid() || selecting_) {
                    return;
                }
                selecting_ = true;
                const int source = index.data(Qt::UserRole + 1).toInt();
                const auto schema = source == 0   ? caseSchema_
                                    : source == 1 ? stackSchema_
                                                  : resultSchema_;
                const auto& value = source == 0 ? case_ : source == 1 ? stack_ : result_;
                if (source != active_) {
                    form_->load(schema, value);
                }
                active_ = source;
                form_->setObjectPath(index.data(Qt::UserRole).toString());
                selecting_ = false;
            });
    connect(form_, &SchemaForm::documentChanged, this, [this] {
        if (selecting_) {
            return;
        }
        // SchemaForm owns only the in-memory edit value, never persistence.
        if (active_ == 0) {
            case_ = form_->document();
        } else if (active_ == 1) {
            stack_ = form_->document();
        } else {
            result_ = form_->document();
        }
        refresh();
    });
}
bool CaseOutline::setCaseDocument(const QString& schemaFile, const Json& document) {
    caseSchema_ = schemaFile;
    case_ = document;
    active_ = 0;
    const bool loaded = form_->load(schemaFile, document);
    refresh();
    return loaded;
}
void CaseOutline::setStackDocument(const QString& schemaFile, const Json& document) {
    stackSchema_ = schemaFile;
    stack_ = document;
    if (active_ == 1) {
        form_->load(schemaFile, document);
    }
    refresh();
}
void CaseOutline::setResultDocument(const QString& schemaFile, const Json& document) {
    resultSchema_ = schemaFile;
    result_ = document;
    if (active_ == 2) {
        form_->load(schemaFile, document);
    }
    refresh();
}
void CaseOutline::append(QStandardItem* parent,
                         const Json& value,
                         const QString& path,
                         int source) {
    auto add = [&](const QString& name, const Json& child, const QString& childPath) {
        auto* item = new QStandardItem(name);
        item->setEditable(false);
        item->setData(childPath, Qt::UserRole);
        const auto identity = child.is_object() && child.contains("id")
                                  ? QString::fromStdString(child.at("id").to_string())
                                  : childPath;
        item->setData(QString::number(source) + ":" + identity, Qt::UserRole + 2);
        item->setData(source, Qt::UserRole + 1);
        parent->appendRow(item);
        append(item, child, childPath, source);
    };
    if (value.is_object()) {
        for (const auto& property : value.object_range()) {
            const auto name = QString::fromStdString(property.key());
            if ((property.value().is_object() || property.value().is_array()) &&
                !name.startsWith("x-")) {
                add(name,
                    property.value(),
                    path + "/" + QString(name).replace("~", "~0").replace("/", "~1"));
            }
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            const auto& child = value.at(i);
            auto name = QString::number(i);
            if (child.is_object()) {
                if (child.contains("id")) {
                    name = QString::fromStdString(child.at("id").to_string());
                } else if (child.contains("name")) {
                    name = QString::fromStdString(child.at("name").to_string());
                }
            }
            add(name, child, path + "/" + QString::number(i));
        }
    }
}
void CaseOutline::refresh() {
    const auto identity = currentIndex().data(Qt::UserRole + 2);
    selecting_ = true;
    model_->clear();
    auto root = [&](const QString& title, const Json& document, int source) {
        auto* item = new QStandardItem(title);
        item->setEditable(false);
        item->setData(QString(), Qt::UserRole);
        item->setData(QString::number(source) + ":root", Qt::UserRole + 2);
        item->setData(source, Qt::UserRole + 1);
        model_->appendRow(item);
        append(item, document, "", source);
        return item;
    };
    root("case", case_, 0);
    if (!stackSchema_.isEmpty()) {
        root("stack / layers", stack_, 1);
    }
    auto* result = root(
        resultSchema_.isEmpty() ? "result (schema/document not loaded)" : "result", result_, 2);
    result->setEnabled(!resultSchema_.isEmpty());
    if (identity.isValid()) {
        const auto matches = model_->match(model_->index(0, 0),
                                           Qt::UserRole + 2,
                                           identity,
                                           1,
                                           Qt::MatchExactly | Qt::MatchRecursive);
        if (!matches.isEmpty()) {
            setCurrentIndex(matches.front());
        }
    }
    expandToDepth(1);
    selecting_ = false;
}
} // namespace chipcae::desktop
