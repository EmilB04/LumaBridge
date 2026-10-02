#pragma once

#include "azoth_oled_music.h"
#include <memory>

namespace luma::app::azoth {

// Used only on the Azoth worker. Poll never waits for a WinRT async operation.
class MusicSource {
public:
    MusicSource();
    ~MusicSource();
    MusicSnapshot Poll(OledContent content);
    void Reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::vector<uint8_t> RenderSong(const MusicSnapshot& song);

} // namespace luma::app::azoth
