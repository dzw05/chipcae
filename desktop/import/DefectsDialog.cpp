#include "DefectsDialog.hpp"

#include "ImportController.hpp"

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace chipcae::desktop {
DefectsDialog::DefectsDialog(ImportController& controller, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Import defects"));
    resize(640, 420);
    auto* layout = new QVBoxLayout(this);
    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    layout->addWidget(text_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    confirm_ = buttons->addButton(tr("确认并继续"), QDialogButtonBox::ActionRole);
    confirm_->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(confirm_, &QPushButton::clicked, &controller, &ImportController::confirmDefects);
    connect(confirm_, &QPushButton::clicked, this, [this] { confirm_->setEnabled(false); });
    connect(&controller, &ImportController::defectsReady, this, [this, &controller] {
        text_->setPlainText(tr("Exit code: %1\n%2\n\n%3\n\n%4")
                                .arg(controller.exitCode())
                                .arg(controller.error(),
                                     QString::fromUtf8(controller.reportBytes()),
                                     QString::fromUtf8(controller.stderrText())));
        confirm_->setEnabled(controller.outcome() == ImportOutcome::NeedsConfirmation);
        if (controller.outcome() != ImportOutcome::Accepted || !controller.error().isEmpty() ||
            controller.exitCode() != 0)
            open();
    });
}
} // namespace chipcae::desktop
