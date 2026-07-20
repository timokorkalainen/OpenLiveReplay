#include "recorder_engine/ingest/spsframerate.h"

namespace {

// Exp-Golomb / bit reader over an RBSP with H.264 emulation-prevention (0x03)
// unescaping and hard overrun tracking. Mirrors the reader in colorvui.cpp; kept
// local so extending timing recovery cannot perturb the working colour parser.
// Every past-end read returns 0 and latches overrun(), so a truncated or hostile
// NAL can never spin or read out of bounds.
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

    // Read up to 32 bits MSB-first. Used for the two 32-bit timing fields.
    uint32_t bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | uint32_t(bit());
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

int32_t gcd32(int32_t a, int32_t b) {
    while (b != 0) {
        const int32_t t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

} // namespace

SpsFrameRate parseSpsFrameRate(NativeVideoCodec codec, const QByteArray& nal) {
    SpsFrameRate out;
    if (codec != NativeVideoCodec::H264 || nal.size() < 4) return out;

    const auto* data = reinterpret_cast<const quint8*>(nal.constData());
    const int nalSize = static_cast<int>(nal.size());
    if ((data[0] & 0x1f) != 7) return out; // not an SPS NAL

    RbspReader r(data, nalSize, 1);
    const unsigned profileIdc = r.bits(8);
    r.bits(8); // constraint flags + reserved
    r.bits(8); // level_idc
    r.ue();    // seq_parameter_set_id

    if (hasHighProfileFields(profileIdc)) {
        const unsigned chromaFormatIdc = r.ue();
        if (chromaFormatIdc == 3) r.bit(); // separate_colour_plane_flag
        r.ue();                            // bit_depth_luma_minus8
        r.ue();                            // bit_depth_chroma_minus8
        r.bit();                           // qpprime_y_zero_transform_bypass_flag
        if (r.bit()) return out;           // seq_scaling_matrix_present -> bail (rare)
    }

    r.ue(); // log2_max_frame_num_minus4
    const unsigned picOrderCntType = r.ue();
    if (picOrderCntType == 0) {
        r.ue(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (picOrderCntType == 1) {
        r.bit();                   // delta_pic_order_always_zero_flag
        r.se();                    // offset_for_non_ref_pic
        r.se();                    // offset_for_top_to_bottom_field
        const unsigned n = r.ue(); // num_ref_frames_in_pic_order_cnt_cycle
        for (unsigned i = 0; i < n && !r.overrun(); ++i)
            r.se();
    }
    r.ue();                                // max_num_ref_frames
    r.bit();                               // gaps_in_frame_num_value_allowed_flag
    r.ue();                                // pic_width_in_mbs_minus1
    r.ue();                                // pic_height_in_map_units_minus1
    const unsigned frameMbsOnly = r.bit(); // frame_mbs_only_flag
    if (!frameMbsOnly) r.bit();            // mb_adaptive_frame_field_flag
    r.bit();                               // direct_8x8_inference_flag
    if (r.bit()) {                         // frame_cropping_flag
        r.ue();
        r.ue();
        r.ue();
        r.ue();
    }
    if (!r.bit() || r.overrun()) return out; // vui_parameters_present_flag

    // --- VUI, walked to timing_info ---
    if (r.bit()) { // aspect_ratio_info_present_flag
        const unsigned aspect = r.bits(8);
        if (aspect == 255) { // Extended_SAR
            r.bits(16);
            r.bits(16);
        }
    }
    if (r.bit()) r.bit(); // overscan_info_present -> overscan_appropriate
    if (r.bit()) {        // video_signal_type_present_flag
        r.bits(3);        // video_format
        r.bit();          // video_full_range_flag
        if (r.bit()) {    // colour_description_present_flag
            r.bits(8);    // colour_primaries
            r.bits(8);    // transfer_characteristics
            r.bits(8);    // matrix_coefficients
        }
    }
    if (r.bit()) { // chroma_loc_info_present_flag
        r.ue();    // chroma_sample_loc_type_top_field
        r.ue();    // chroma_sample_loc_type_bottom_field
    }
    if (!r.bit() || r.overrun()) return out; // timing_info_present_flag

    const uint32_t numUnitsInTick = r.bits(32);
    const uint32_t timeScale = r.bits(32);
    // (fixed_frame_rate_flag follows but is not needed.)
    if (r.overrun() || numUnitsInTick == 0 || timeScale == 0) return out;

    // fps = time_scale / (2 * num_units_in_tick). Guard the doubling against the
    // 32-bit edge, then reduce and range-check to a plausible broadcast rate so a
    // mis-parse cannot inject a bogus "exact" rate (it downgrades to Incomparable).
    const int64_t den64 = int64_t(numUnitsInTick) * 2;
    const int64_t num64 = int64_t(timeScale);
    if (num64 > 2'000'000'000 || den64 > 2'000'000'000) return out;
    int32_t num = int32_t(num64);
    int32_t den = int32_t(den64);
    const int32_t g = gcd32(num, den);
    if (g > 1) {
        num /= g;
        den /= g;
    }
    // Plausible frame rate: ~12..240 fps. Rejects timing_info that decodes to a
    // field/tick rate or a garbage value from a mis-walked VUI.
    const double fps = double(num) / double(den);
    if (fps < 12.0 || fps > 240.0) return out;

    out.num = num;
    out.den = den;
    return out;
}
