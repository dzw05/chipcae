#include "DefectsDialog.hpp"

#include "ImportController.hpp"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
namespace chipcae::desktop {
DefectsDialog::DefectsDialog(ImportController& controller, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Import defects"));
    resize(640, 420);
    auto* layout = new QVBoxLayout(this);
    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    layout->addWidget(text_);
    auto* table = new QTableWidget(this);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        {tr("defect_id"), tr("tier"), tr("kind"), tr("说明"), tr("大小"), tr("单位")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    confirm_ = buttons->addButton(tr("确认并继续"), QDialogButtonBox::ActionRole);
    confirm_->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(confirm_, &QPushButton::clicked, this, [this, &controller] {
        confirm_->setEnabled(false);
        controller.confirmDefects();
    });
    connect(&controller, &ImportController::defectsReady, this, [this, &controller, table] {
        text_->setPlainText(tr("Exit code: %1\n%2\n\n%3\n\n%4")
                                .arg(controller.exitCode())
                                .arg(controller.error(),
                                     QString::fromUtf8(controller.reportBytes()),
                                     QString::fromUtf8(controller.stderrText())));
        table->setRowCount(0);
        const auto report = controller.importReport();
        if (report.empty()) {
            text_->appendPlainText(tr("无旁路缺陷明细"));
        } else {
            const auto& defects = report.at("defects");
            if (defects.empty())
                text_->appendPlainText(tr("无缺陷"));
            // Stable grouping: iterate the original array within each severity.
            for (const auto* tier : {"C", "B", "A", "Weld"}) {
                for (const auto& defect : defects.array_range()) {
                    if (defect.at("tier") != tier)
                        continue;
                    auto string = [&](const char* key) {
                        return QString::fromStdString(defect.at(key).as<std::string>());
                    };
                    auto description = string("suggestion");
                    const auto rejection = string("rejection_reason");
                    if (!rejection.isEmpty())
                        description += "\n" + rejection;
                    const auto& size = defect.at("size");
                    const auto sizeText = size.is_null()
                                              ? QStringLiteral("—")
                                              : QString::number(size.as<double>(), 'g', 17);
                    const auto unit = string("size_unit");
                    const auto unitText = unit == "1"    ? tr("计数")
                                          : unit == "m2" ? QStringLiteral("m²")
                                          : unit == "m3" ? QStringLiteral("m³")
                                                         : unit;
                    const QStringList cells{string("defect_id"),
                                            string("tier"),
                                            string("kind"),
                                            description,
                                            sizeText,
                                            unitText};
                    const int row = table->rowCount();
                    table->insertRow(row);
                    for (int column = 0; column < cells.size(); ++column) {
                        auto* item = new QTableWidgetItem(cells[column]);
                        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                        table->setItem(row, column, item);
                    }
                }
            }
            if (report.contains("parts")) {
                const auto& parts = report.at("parts");
                for (const auto* key : {"split_layers", "rewrite_targets"})
                    text_->appendPlainText(QString::fromLatin1(key) + ": " +
                                           QString::fromStdString(parts.at(key).to_string()));
                text_->appendPlainText(
                    "welded_interfaces: " +
                    QString::fromStdString(report.at("welded_interfaces").to_string()));
            }
        }
        confirm_->setEnabled(controller.outcome() == ImportOutcome::NeedsConfirmation);
        if (controller.outcome() != ImportOutcome::Accepted || !controller.error().isEmpty() ||
            controller.exitCode() != 0)
            open();
    });
}
} // namespace chipcae::desktop
