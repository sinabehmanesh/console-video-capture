#include "audio_passthrough.h"

#include <iostream>
#include <string>

int video_main();

namespace {

int run_audio_only_mode() {
    AudioPassthrough audio;
    audio.start();

    std::cout << "\nAudio-only passthrough mode active.\n"
              << "Video is expected to use the capture card's HDMI passthrough directly.\n"
              << "Only capture-card audio is routed to the current Windows default output.\n"
              << "Press Enter to stop.\n";

    std::string line;
    std::getline(std::cin, line);

    audio.stop();
    return 0;
}

} // namespace

int main() {
    std::cout << "Console video capture mode:\n"
              << "  [1] Video + audio capture - render USB video and pass capture-card audio\n"
              << "  [2] Audio passthrough only - use HDMI passthrough for video, Windows for audio\n"
              << "Select mode [1]: ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice == "2") {
        return run_audio_only_mode();
    }

    return video_main();
}
