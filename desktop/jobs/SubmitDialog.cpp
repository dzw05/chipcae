#include "SubmitDialog.hpp"

#include "CaseDocument.hpp"

#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QSaveFile>
#include <QSpinBox>
namespace chipcae::desktop {
SubmitDialog::SubmitDialog(QWidget* parent) : QDialog(parent) {
    profile_ = {{"schema", "chipcae.run_profile/1.0"},
                {"ranks", 1},
                {"backend", "cpu"},
                {"gpus", 0},
                {"threads_per_rank", 1},
                {"launcher", "direct"}};
    auto* layout = new QFormLayout(this);
    auto spin = [this](int value) {
        auto* s = new QSpinBox(this);
        s->setRange(1, 1000000);
        s->setValue(value);
        return s;
    };
    ranks_ = spin(1);
    threads_ = spin(1);
    nx_ = spin(4);
    layout->addRow(tr("Ranks"), ranks_);
    layout->addRow(tr("Threads per rank"), threads_);
    layout->addRow(tr("Mesh nx"), nx_);
    layout->addRow(new QLabel(tr("CPU · 0 GPUs · CUDA unavailable"), this));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
QJsonObject SubmitDialog::profile() const {
    auto p = profile_;
    p["ranks"] = ranks_->value();
    p["threads_per_rank"] = threads_->value();
    p["backend"] = "cpu";
    p["gpus"] = 0;
    if (ranks_->value() > 1)
        p["launcher"] = "mpirun";
    return p;
}
void SubmitDialog::setProfile(const QJsonObject& p) {
    profile_ = {};
    for (auto it = p.begin(); it != p.end(); ++it)
        if (QStringList{"schema",
                        "ranks",
                        "backend",
                        "gpus",
                        "threads_per_rank",
                        "launcher",
                        "mpi_args",
                        "output_interval"}
                .contains(it.key()) ||
            it.key().startsWith("x-"))
            profile_.insert(it.key(), it.value());
    ranks_->setValue(p["ranks"].toInt(1));
    threads_->setValue(p["threads_per_rank"].toInt(1));
}
int SubmitDialog::meshNx() const {
    return nx_->value();
}
void SubmitDialog::setMeshNx(int nx) {
    nx_->setValue(nx);
}
// TEST-FIRST stubs: keep existing behavior until preview mesh locking is implemented.
void SubmitDialog::setMeshNxLocked(bool) {}
bool SubmitDialog::meshNxLocked() const {
    return false;
}
bool SubmitDialog::loadProfile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    auto doc = QJsonDocument::fromJson(file.readAll());
    CaseDocument validator;
    try {
        if (!doc.isObject() || !validator.applyRunProfile(Json::parse(doc.toJson().toStdString())))
            return false;
    } catch (...) {
        return false;
    }
    setProfile(doc.object());
    return true;
}
bool SubmitDialog::saveProfile(const QString& path) const {
    QSaveFile file(path);
    auto bytes = QJsonDocument(profile()).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
} // namespace chipcae::desktop
