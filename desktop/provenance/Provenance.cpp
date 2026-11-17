#include "Provenance.hpp"

#include "ResultView.hpp"

#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QTimeZone>
#include <QtEndian>
namespace chipcae::desktop {
Provenance Provenance::read(const QString& directory) {
    QFile f(directory + "/provenance.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    auto p = QJsonDocument::fromJson(f.readAll()).object();
    if (p["schema"] != "chipcae.provenance/0.1")
        return {};
    auto date = QDateTime::fromString(p["generated_at"].toString(), Qt::ISODate);
    return {p["source"].toString(),
            date.toTimeZone(QTimeZone("Asia/Shanghai")).toString("yyyy-MM-dd HH:mm:ss"),
            p["degraded"].toString()};
}
QString Provenance::label() const {
    return source == "real"          ? "真实计算"
           : source == "mock"        ? "MOCK 模拟数据"
           : source == "precomputed" ? "预计算结果"
                                     : "来源未知";
}
QString Provenance::watermark() const {
    return source == "real" ? QString{} : source == "precomputed" ? "预计算" : "MOCK";
}
bool writeCapturedPng(ResultView& view, const QString& path, const QString& watermark) {
    if (!view.capture(path))
        return false;
    if (!watermark.isEmpty()) {
        QImage bitmap(path);
        if (bitmap.isNull())
            return false;
        QPainter painter(&bitmap);
        painter.setPen(QColor(200, 130, 0, 150));
        auto font = painter.font();
        font.setPixelSize(qMax(24, bitmap.width() / 12));
        font.setBold(true);
        painter.setFont(font);
        painter.translate(bitmap.width() / 2, bitmap.height() / 2);
        painter.rotate(-25);
        painter.drawText(
            QRect(-bitmap.width() / 2, -50, bitmap.width(), 100), Qt::AlignCenter, watermark);
        painter.end();
        if (!bitmap.save(path, "PNG"))
            return false;
    }
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    auto png = input.readAll();
    input.close();
    if (png.size() < 12)
        return false;
    QByteArray data("chipcae.watermark");
    data.append('\0');
    data.append(watermark.toUtf8());
    QByteArray chunk(4, '\0');
    qToBigEndian<quint32>(data.size(), chunk.data());
    QByteArray payload = QByteArray("tEXt") + data;
    chunk += payload;
    quint32 crc = 0xffffffff;
    for (unsigned char c : payload) {
        crc ^= c;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    char bytes[4];
    qToBigEndian<quint32>(crc ^ 0xffffffff, bytes);
    chunk.append(bytes, 4);
    png.insert(png.size() - 12, chunk);
    QSaveFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(png) == png.size() && out.commit();
}
} // namespace chipcae::desktop
