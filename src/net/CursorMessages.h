#pragma once

#include "display/DesktopDuplication.h"

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// PNG encode of a straight-alpha BGRA sprite (WIC). Empty on failure.
std::vector<uint8_t> EncodeBgraPng(const uint8_t* bgra, int width, int height);

std::string Base64Encode(const uint8_t* data, size_t size);

// OpenDisplay control JSON (no NULs, starts with '{', under 32 KiB).
std::string MakeCursorMessage(bool visible, double x, double y, uint64_t seq = 0);
std::string MakeCursorImgMessage(const PointerShapeBgra& shape, uint32_t displayW, uint32_t displayH);

} // namespace od
