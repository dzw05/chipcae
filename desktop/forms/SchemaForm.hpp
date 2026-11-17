#pragma once
#include "SchemaValidator.hpp"

#include <QWidget>
class QVBoxLayout;
class QListWidget;
#include <map>
namespace chipcae::desktop {
// Owns an in-memory value only; persistence and document-level rules belong to CaseDocument.
class SchemaForm : public QWidget {
    Q_OBJECT
public:
    explicit SchemaForm(QWidget* parent = nullptr);
    bool load(const QString& schemaFile, const Json& document);
    void setObjectPath(const QString& pointer);
    const Json& document() const { return document_; }
    const QList<FieldError>& errors() const { return errors_; }
    QString objectPath() const { return path_; }
    bool setValue(const QString& pointer, const Json& value);
    static QString caseSchemaPath(const QString& repository);
    static QString resultSchemaPath(const QString& repository);
signals:
    void documentChanged();
    void validationChanged();

private:
    std::unique_ptr<SchemaValidator> validator_;
    Json document_;
    QString path_;
    QList<FieldError> errors_;
    std::map<QString, QString> inputErrors_;
    QVBoxLayout* layout_;
    QWidget* fields_ = nullptr;
    QListWidget* messages_;
    void rebuild();
    void validate();
    void scheduleRebuild();
    void render(QVBoxLayout* layout, const Json& schema, const QString& path, int depth = 0);
    Json schemaAt(const QString& path) const;
    Json seed(const Json& schema) const;
};
} // namespace chipcae::desktop
