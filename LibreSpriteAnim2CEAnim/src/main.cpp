#include "json.hpp"
#include "tdf.hpp"
#include <string>
#include <fstream>
#include <iostream>

int main(int argc, char* argv[]) {
    constexpr uint8_t kTdfVersion = 0x11;

    if (argc < 3) {
        std::cerr << "Usage: "<< argv[0] << " <input.json> <output.tdf> [override_image]\n";
        return 1;
    }

    std::string inputPath  = argv[1];
    std::string outputPath = argv[2];
    std::string overrideImage = (argc >= 4) ? argv[3] : "";

    std::ifstream file(inputPath);
    if (!file.is_open()) {
        std::cerr << "Failed to open input\n";
        return 1;
    }

    nlohmann::json j;
    file >> j;

    TDFFile out;

    std::string imagePath = j["meta"]["image"];
    if (!overrideImage.empty())
        imagePath = overrideImage;

    out.set("SourceImagePath", TDFFile::makeString(imagePath));

    auto& frames = j["frames"];
    out.set("FrameCount", TDFFile::makeUInt(frames.size()));

    std::vector<TDFFile> frameList;

    for (auto& [name, f] : frames.items()) {
        TDFFile frame;

        frame.set("X", TDFFile::makeUInt(f["frame"]["x"]));
        frame.set("Y", TDFFile::makeUInt(f["frame"]["y"]));
        frame.set("Width", TDFFile::makeUInt(f["frame"]["w"]));
        frame.set("Height", TDFFile::makeUInt(f["frame"]["h"]));
        frame.set("Duration", TDFFile::makeUInt(f["duration"]));

        frameList.push_back(frame);
    }

    out.set("Frames", TDFFile::makeObjectArray(frameList, kTdfVersion));

    out.save(outputPath, kTdfVersion);

    return 0;
}
