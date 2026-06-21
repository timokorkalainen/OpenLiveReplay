#include "recorder_engine/ingest/colorvui.h"

namespace {

class RbspReader {
public:
    RbspReader(const quint8* data, int size, int startByte)
        : m_data(data), m_size(size), m_byte(startByte) {}

    int bit() {
        if (m_byte >= m_size) {
            m_overrun = true;
            return 0;
        }
        if (m_zeroes >= 2 && m_data[m_byte] == 0x03 && m_bit == 0) {
            ++m_byte;
            m_zeroes = 0;
            if (m_byte >= m_size) {
                m_overrun = true;
                return 0;
            }
        }
        const int b = (m_data[m_byte] >> (7 - m_bit)) & 1;
        if (m_bit == 0) {
            if (m_data[m_byte] == 0x00)
                ++m_zeroes;
            else
                m_zeroes = 0;
        }
        if (++m_bit == 8) {
            m_bit = 0;
            ++m_byte;
        }
        return b;
    }

    unsigned bits(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | unsigned(bit());
        return v;
    }

    unsigned ue() {
        int zeros = 0;
        while (!m_overrun && bit() == 0 && zeros < 32)
            ++zeros;
        if (zeros >= 32) {
            m_overrun = true;
            return 0;
        }
        return (1u << zeros) - 1u + bits(zeros);
    }

    int se() {
        const unsigned k = ue();
        return (k & 1u) ? int((k + 1u) / 2u) : -int(k / 2u);
    }

    bool overrun() const { return m_overrun; }

private:
    const quint8* m_data = nullptr;
    int m_size = 0;
    int m_byte = 0;
    int m_bit = 0;
    int m_zeroes = 0;
    bool m_overrun = false;
};

ColorPrimaries mapPrimaries(unsigned code) {
    switch (code) {
    case 1:
        return ColorPrimaries::Bt709;
    case 5:
    case 6:
        return ColorPrimaries::Bt601;
    case 9:
        return ColorPrimaries::Bt2020;
    default:
        return ColorPrimaries::Unspecified;
    }
}

ColorTransfer mapTransfer(unsigned code) {
    switch (code) {
    case 1:
    case 6:
    case 14:
    case 15:
        return ColorTransfer::Bt709;
    case 5:
    case 8:
        return ColorTransfer::Bt601;
    case 16:
        return ColorTransfer::Bt2020;
    default:
        return ColorTransfer::Unspecified;
    }
}

ColorMatrix mapMatrix(unsigned code) {
    switch (code) {
    case 5:
    case 6:
        return ColorMatrix::Bt601;
    case 9:
    case 10:
        return ColorMatrix::Bt2020;
    default:
        return ColorMatrix::Bt709;
    }
}

bool hasHighProfileFields(unsigned profileIdc) {
    switch (profileIdc) {
    case 44:
    case 83:
    case 86:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 134:
    case 138:
    case 139:
    case 244:
        return true;
    default:
        return false;
    }
}

} // namespace

VuiColorInfo parseSpsColorVui(NativeVideoCodec codec, const QByteArray& nal) {
    VuiColorInfo out;
    if (codec != NativeVideoCodec::H264 || nal.size() < 4) return out;

    const auto* data = reinterpret_cast<const quint8*>(nal.constData());
    const int nalSize = static_cast<int>(nal.size());
    if ((data[0] & 0x1f) != 7) return out;

    RbspReader r(data, nalSize, 1);
    const unsigned profileIdc = r.bits(8);
    r.bits(8);
    r.bits(8);
    r.ue();

    if (hasHighProfileFields(profileIdc)) {
        const unsigned chromaFormatIdc = r.ue();
        if (chromaFormatIdc == 3) r.bit();
        r.ue();
        r.ue();
        r.bit();
        if (r.bit()) return out;
    }

    r.ue();
    const unsigned picOrderCntType = r.ue();
    if (picOrderCntType == 0) {
        r.ue();
    } else if (picOrderCntType == 1) {
        r.bit();
        r.se();
        r.se();
        const unsigned n = r.ue();
        for (unsigned i = 0; i < n && !r.overrun(); ++i)
            r.se();
    }
    r.ue();
    r.bit();
    r.ue();
    r.ue();
    const unsigned frameMbsOnly = r.bit();
    if (!frameMbsOnly) r.bit();
    r.bit();
    if (r.bit()) {
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    if (!r.bit() || r.overrun()) return out;

    if (r.bit()) {
        const unsigned aspect = r.bits(8);
        if (aspect == 255) {
            r.bits(16);
            r.bits(16);
        }
    }
    if (r.bit()) r.bit();

    if (r.bit()) {
        r.bits(3);
        out.range = r.bit() ? ColorRange::Full : ColorRange::Video;
        if (r.bit()) {
            const unsigned primaries = r.bits(8);
            const unsigned transfer = r.bits(8);
            const unsigned matrix = r.bits(8);
            if (!r.overrun()) {
                out.present = true;
                out.primaries = mapPrimaries(primaries);
                out.transfer = mapTransfer(transfer);
                out.matrix = mapMatrix(matrix);
            }
        }
    }
    return out;
}
