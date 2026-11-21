#include "ResultControls.hpp"

#include "ResultView.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
namespace chipcae::desktop {
ResultControls::ResultControls(ResultView& view, ResultSession& session, QWidget* parent)
    : QWidget(parent) {
    auto layout = new QFormLayout(this);
    auto field = new QComboBox(this);
    field->addItems({QStringLiteral("温度"),
                     QStringLiteral("位移幅值"),
                     QStringLiteral("翘曲"),
                     QStringLiteral("叠层"),
                     QStringLiteral("材料")});
    field->setCurrentIndex(static_cast<int>(view.colorField()));
    layout->addRow(QStringLiteral("字段"), field);
    connect(field, &QComboBox::currentIndexChanged, &view, [&view](int index) {
        view.setColorField(static_cast<ResultColorField>(index));
    });
    connect(&view, &ResultView::displayChanged, field, [&view, field] {
        const QSignalBlocker blocker(field);
        field->setCurrentIndex(static_cast<int>(view.colorField()));
    });
    auto picked = new QLabel(QStringLiteral("未知"), this);
    layout->addRow("elem_gid", picked);
    connect(&view, &ResultView::elementPicked, picked, &QLabel::setText);
    auto scale = new QDoubleSpinBox(this);
    scale->setRange(-1e9, 1e9);
    scale->setDecimals(6);
    scale->setValue(1);
    layout->addRow(QStringLiteral("翘曲放大系数"), scale);
    connect(scale, &QDoubleSpinBox::valueChanged, &view, &ResultView::setWarpageScale);
    auto layer = new QComboBox(this);
    layer->addItem(QStringLiteral("全部层"), 0);
    const auto names = session.layerNames();
    for (int i = 0; i < names.size(); ++i)
        layer->addItem(session.layerName(i + 1), i + 1);
    layout->addRow(QStringLiteral("叠层"), layer);
    connect(layer, &QComboBox::currentIndexChanged, &view, [&view, layer] {
        view.setLayer(layer->currentData().toInt());
    });
    for (auto name : {"hex", "cut", "eb_surface", "particles"}) {
        auto check = new QCheckBox(name, this);
        check->setChecked(true);
        layout->addRow(check);
        connect(check, &QCheckBox::toggled, &view, [&view, name](bool on) {
            view.setBlockVisible(name, on);
        });
    }
    auto voids = new QCheckBox("Show void cells", this);
    layout->addRow(voids);
    connect(voids, &QCheckBox::toggled, &view, &ResultView::setShowVoid);
    auto clip = new QCheckBox("Clip (positive half-space)", this);
    layout->addRow(clip);
    std::array<QDoubleSpinBox*, 6> plane{};
    for (int i = 0; i < 6; ++i) {
        plane[i] = new QDoubleSpinBox(this);
        plane[i]->setDecimals(9);
        plane[i]->setRange(-1e9, 1e9);
        plane[i]->setValue(i == 3 ? 1 : 0);
        layout->addRow(QString("%1 %2").arg(i < 3 ? "Origin" : "Normal").arg("xyz"[i % 3]),
                       plane[i]);
    }
    auto updateClip = [&view, clip, plane] {
        view.setClip(clip->isChecked(),
                     {plane[0]->value(), plane[1]->value(), plane[2]->value()},
                     {plane[3]->value(), plane[4]->value(), plane[5]->value()});
    };
    connect(clip, &QCheckBox::toggled, &view, updateClip);
    for (auto spin : plane)
        connect(spin, &QDoubleSpinBox::valueChanged, &view, updateClip);
    auto steps = new QComboBox(this);
    for (auto s : session.steps()) {
        auto o = s.toObject();
        steps->addItem(QString::number(o["n"].toInteger()), o["n"].toInteger());
    }
    layout->addRow("Step", steps);
    auto summary = new QLabel(this);
    summary->setWordWrap(true);
    layout->addRow(summary);
    auto refresh = [&view, summary] {
        const auto range = view.colorRange();
        const auto fieldText = range.valid ? QString("%1: min=%2 max=%3")
                                                 .arg(view.colorBarTitle())
                                                 .arg(range.min, 0, 'g', 16)
                                                 .arg(range.max, 0, 'g', 16)
                               : view.colorField() == ResultColorField::Temperature
                                   ? QStringLiteral("无温度场")
                                   : QString("Field: unavailable");
        summary->setText(view.tmaxText() + "\n" + fieldText + "\n" + view.summary("warpage_pv") +
                         "\n" + view.summary("U_mag_max"));
    };
    connect(steps, &QComboBox::currentIndexChanged, &view, [&view, &session, steps, refresh] {
        view.loadStep(session, steps->currentData().toLongLong());
        refresh();
    });
    connect(&view, &ResultView::displayChanged, summary, refresh);
    refresh();
    auto capture = new QPushButton("Export PNG", this);
    layout->addRow(capture);
    connect(capture, &QPushButton::clicked, &view, [&view, this] {
        const auto path =
            QFileDialog::getSaveFileName(this, "Export screenshot", {}, "PNG (*.png)");
        if (!path.isEmpty())
            view.capture(path);
    });
}
} // namespace chipcae::desktop
