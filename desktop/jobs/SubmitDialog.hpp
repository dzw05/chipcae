#pragma once
#include <QDialog>
#include <QJsonObject>
class QSpinBox;
namespace chipcae::desktop {
class SubmitDialog : public QDialog {
public:
    explicit SubmitDialog(QWidget* parent = nullptr);
    bool loadProfile(const QString& path);
    bool saveProfile(const QString& path) const;
    QJsonObject profile() const;
    void setProfile(const QJsonObject& value);
    int meshNx() const;
    void setMeshNx(int nx);
    void setMeshNxLocked(bool locked);
    bool meshNxLocked() const;

private:
    QJsonObject profile_;
    QSpinBox *ranks_, *threads_, *nx_;
};
} // namespace chipcae::desktop
