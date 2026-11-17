#include "EventCodec.hpp"

#include <QFile>
#include <QTemporaryFile>
#include <QtEndian>
namespace chipcae::desktop {
namespace {
class Unsupported final : public EventCodec {
    DecodeResult read(const QString&, quint64 offset) override {
        // CCEVLOG/CRC32C and segmented logs await frozen wire and golden fixtures.
        return {{}, offset, "unsupported CCEVLOG/CRC32C (wire not implemented)"};
    }
};
class Legacy final : public EventCodec {
    DecodeResult read(const QString& path, quint64 offset) override {
        DecodeResult result{{}, offset, {}};
        QFile file(path);
        if (!file.exists())
            return result;
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
            result.diagnostic = "cannot open event log";
            return result;
        }
        while (!file.atEnd()) {
            auto header = file.read(16);
            if (header.size() < 16)
                break;
            if (header.first(8) != QByteArray("CAELOG1\0", 8)) {
                result.diagnostic = "invalid CAELOG1 magic";
                break;
            }
            auto length = qFromLittleEndian<quint32>(header.constData() + 8);
            if (length > 64u * 1024u * 1024u) {
                result.diagnostic = "CAELOG1 record exceeds 64 MiB";
                break;
            }
            auto body = file.read(length);
            if (body.size() != length)
                break;
            quint32 crc = 0xffffffffu;
            for (unsigned char byte : body) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
            }
            if ((crc ^ 0xffffffffu) != qFromLittleEndian<quint32>(header.constData() + 12)) {
                result.diagnostic = "CAELOG1 bad CRC-32/IEEE";
                break;
            }
            // Isolate each complete record: the shared reader throws on malformed
            // payloads, and must not lose a preceding valid prefix in that case.
            QTemporaryFile record;
            if (!record.open() || record.write(header + body) != header.size() + body.size() ||
                !record.flush()) {
                result.diagnostic = "cannot stage legacy record";
                break;
            }
            try {
                auto events = io::EventLogReader(record.fileName().toStdString()).read_from(0);
                if (events.size() != 1)
                    throw std::runtime_error("invalid CAELOG1 payload");
                if (auto hello = std::get_if<io::KernelHello>(&events.front().payload);
                    hello && hello->event_log_format != 1)
                    throw std::runtime_error("unsupported Hello event_log_format");
                result.events.push_back(std::move(events.front()));
                result.offset += 16 + length;
            } catch (const std::exception& e) {
                result.diagnostic = QString::fromUtf8(e.what());
                break;
            }
        }
        return result;
    }
};
} // namespace
std::unique_ptr<EventCodec> makeEventCodec(EventFormat format) {
    if (format == EventFormat::LegacyCaelog1)
        return std::make_unique<Legacy>();
    return std::make_unique<Unsupported>();
}
} // namespace chipcae::desktop
