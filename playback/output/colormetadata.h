#ifndef COLORMETADATA_H
#define COLORMETADATA_H

enum class ColorMatrix {
    Bt601,
    Bt709,
    Bt2020,
};

enum class ColorRange {
    Video,
    Full,
};

enum class ColorPrimaries {
    Bt601,
    Bt709,
    Bt2020,
    Unspecified,
};

enum class ColorTransfer {
    Bt601,
    Bt709,
    Bt2020,
    Unspecified,
};

enum class ChromaFormat {
    Yuv420,
    Yuv422,
    Yuv444,
    Rgb,
};

struct ColorMetadata {
    ColorMatrix matrix = ColorMatrix::Bt709;
    ColorRange range = ColorRange::Video;
    ColorPrimaries primaries = ColorPrimaries::Bt709;
    ColorTransfer transfer = ColorTransfer::Bt709;
    ChromaFormat chromaFormat = ChromaFormat::Yuv420;
    int bitDepth = 8;

    bool operator==(const ColorMetadata& other) const {
        return matrix == other.matrix && range == other.range && primaries == other.primaries &&
               transfer == other.transfer && chromaFormat == other.chromaFormat &&
               bitDepth == other.bitDepth;
    }
    bool operator!=(const ColorMetadata& other) const { return !(*this == other); }
};

#endif // COLORMETADATA_H
