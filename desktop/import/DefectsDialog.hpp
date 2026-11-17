#pragma once
#include <QDialog>
class QPlainTextEdit;
class QPushButton;
namespace chipcae::desktop {
class ImportController;
class DefectsDialog : public QDialog {
    Q_OBJECT
public:
    explicit DefectsDialog(ImportController& controller, QWidget* parent = nullptr);

private:
    QPlainTextEdit* text_;
    QPushButton* confirm_;
};
} // namespace chipcae::desktop
