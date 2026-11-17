#include "EventTailer.hpp"

#include <algorithm>
#include <sys/stat.h>
namespace chipcae::desktop {
EventTailer::EventTailer(QString path, EventFormat format)
    : path_(std::move(path)), codec_(makeEventCodec(format)) {}
DecodeResult EventTailer::poll() {
    struct stat info{};
    if (::stat(path_.toLocal8Bit().constData(), &info) == 0) {
        if (device_ != quint64(info.st_dev) || inode_ != quint64(info.st_ino) ||
            quint64(info.st_size) < offset_)
            offset_ = 0;
        device_ = info.st_dev;
        inode_ = info.st_ino;
    }
    auto result = codec_->read(path_, offset_);
    offset_ = result.offset;
    std::erase_if(result.events, [this](const io::Event& event) {
        if (delivered_ && event.seq <= sequence_)
            return true;
        delivered_ = true;
        sequence_ = event.seq;
        return false;
    });
    return result;
}
} // namespace chipcae::desktop
