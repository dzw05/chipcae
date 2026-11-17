#pragma once
#include "EventCodec.hpp"
namespace chipcae::desktop {
class EventTailer {
public:
    EventTailer(QString path, EventFormat format);
    DecodeResult poll();
    quint64 offset() const { return offset_; }
    quint64 lastSequence() const { return sequence_; }

private:
    QString path_;
    std::unique_ptr<EventCodec> codec_;
    quint64 offset_ = 0, sequence_ = 0;
    quint64 device_ = 0, inode_ = 0;
    bool delivered_ = false;
};
} // namespace chipcae::desktop
