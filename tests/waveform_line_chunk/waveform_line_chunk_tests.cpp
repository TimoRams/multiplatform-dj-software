#include "waveform/WaveformLineChunk.h"

#include <iostream>

int main()
{
    constexpr std::uint32_t chunkSize = 4096;
    auto lines = std::make_shared<std::vector<WaveformLine>>(17);
    WaveformLineChunk valid{3, 2, 8192, 17, 8209, std::move(lines)};
    if (!valid.isWellFormed(chunkSize)) {
        std::cerr << "FAIL: final partial immutable chunk rejected\n";
        return 1;
    }
    valid.lineCount = 16;
    if (valid.isWellFormed(chunkSize)) {
        std::cerr << "FAIL: count mismatch accepted\n";
        return 1;
    }
    return 0;
}
