#include "MaterialMapMissingDialog.hpp"

#include "ImportSourceDialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
namespace chipcae::desktop {
MaterialMapMissingDialog::MaterialMapMissingDialog(QWidget* parent) : QDialog(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(messageText(), this));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* reselect =
        buttons->addButton(QStringLiteral("重新选择文件"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(reselect, &QPushButton::clicked, this, [this] {
        selected_ = ImportSourceDialog::getMaterialMapPath(this);
        if (!selected_.isEmpty()) {
            accept();
        }
    });
}
QString MaterialMapMissingDialog::messageText() const {
    return QStringLiteral("缺少材料映射，请选择映射文件后重试");
}
QString MaterialMapMissingDialog::prompt(QWidget* parent) {
    MaterialMapMissingDialog dialog(parent);
    return dialog.exec() == QDialog::Accepted ? dialog.selected_ : QString{};
}
} // namespace chipcae::desktop
