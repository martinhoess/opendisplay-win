#pragma once

#include <string>

namespace od {

// Writes one log line, prefixed with the local time and the device it belongs
// to (the iPad's address). Several senders share one log file, so a line
// without its device — and without a timestamp to order it against the other
// senders' lines — can't be attributed afterwards.
void Logf(const std::string& tag, const char* fmt, ...);

} // namespace od
