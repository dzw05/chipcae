#pragma once
#include "chipcae/io/event_log.hpp"

#include <QString>
#include <memory>
namespace chipcae::desktop {
enum class EventFormat { Ccevlog, LegacyCaelog1 };
struct DecodeResult {
    std::vector<io::Event> events;
    quint64 offset = 0;
    QString diagnostic;
};
class EventCodec {
public:
    virtual ~EventCodec() = default;
    virtual DecodeResult read(const QString& path, quint64 offset) = 0;
};
std::unique_ptr<EventCodec> makeEventCodec(EventFormat format);
} // namespace chipcae::desktop
