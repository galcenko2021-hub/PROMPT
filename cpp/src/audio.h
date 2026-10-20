// Звуковые эффекты из синтезированных тонов (XAudio2, без внешних файлов).
// Если звуковое устройство недоступно, звук просто отключается.
#pragma once

#include <string>

namespace audio {

bool init();
void shutdown();
void play(const std::string& name);

}  // namespace audio
