#include "ImportSourceDialog.hpp"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QPushButton>
#include <QSortFilterProxyModel>
namespace chipcae::desktop {
namespace {
class SourceModel : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        auto* files = qobject_cast<QFileSystemModel*>(sourceModel());
        if (files &&
            !ImportSourceDialog::isSupportedImportPath(files->filePath(mapToSource(index))))
            return QSortFilterProxyModel::flags(index) &
                   ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        return QSortFilterProxyModel::flags(index);
    }
};
} // namespace
QStringList ImportSourceDialog::nameFilters() {
    return {"IPC-2581 / ODB++ (*.xml *.tgz *.zip)",
            "IPC-2581 (*.xml)",
            "ODB++ (*.tgz *.zip)",
            "STEP (*.step *.stp) — B 档 (disabled)"};
}
bool ImportSourceDialog::isSupportedImportPath(const QString& path) {
    if (path.isEmpty())
        return false;
    const QFileInfo file(path);
    if (file.isDir())
        return true;
    const auto suffix = file.suffix().toLower();
    return file.isFile() && (suffix == "xml" || suffix == "tgz" || suffix == "zip");
}
QString ImportSourceDialog::getImportPath(QWidget* parent,
                                          const QString& caption,
                                          const QString& startDir) {
    QFileDialog dialog(
        parent, caption.isEmpty() ? QStringLiteral("Import ECAD") : caption, startDir);
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters(nameFilters());
    dialog.setProxyModel(new SourceModel(&dialog));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    auto* directory = buttons->addButton(QStringLiteral("Select current ODB++ directory"),
                                         QDialogButtonBox::ActionRole);
    QString chosenDirectory;
    QObject::connect(directory, &QPushButton::clicked, &dialog, [&] {
        chosenDirectory = dialog.directory().absolutePath();
        dialog.QDialog::done(QDialog::Accepted);
    });
    if (dialog.exec() != QDialog::Accepted)
        return {};
    if (!chosenDirectory.isEmpty())
        return chosenDirectory;
    const auto selected = dialog.selectedFiles();
    return !selected.isEmpty() && isSupportedImportPath(selected.first()) ? selected.first()
                                                                          : QString{};
}
QString ImportSourceDialog::getMaterialMapPath(QWidget* parent, const QString& startDir) {
    QFileDialog dialog(parent, QStringLiteral("选择材料映射"), startDir);
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilters({"JSON (*.json)"});
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return {};
    const auto path = dialog.selectedFiles().first();
    const QFileInfo file(path);
    return file.isFile() && file.suffix().compare("json", Qt::CaseInsensitive) == 0 ? path
                                                                                    : QString{};
}
} // namespace chipcae::desktop
