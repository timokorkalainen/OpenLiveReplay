#include <QtTest>
#include <QFile>

#include <limits>

#include "recorder_engine/ingest/h26xseitimecode.h"
#include "recorder_engine/ingest/h26xtimingcontext.h"
#include "recorder_engine/timing/smpte12m.h"

class TestH26xSeiTimecode : public QObject {
    Q_OBJECT

private slots:
    void h264StandardPicTimingIsDecoded();
    void h264PicTimingWithHrdIsDecoded();
    void h264LeadingBcdWithoutTimestampIsIgnored();
    void h264TruncatedClockTimestampIsRejected();
    void h264TimeOffsetIsBoundsChecked();
    void h264PartialTimestampIsNotReturned();
    void h264OutOfRateFrameLabelIsRejected();
    void h264ConsumesEveryClockTimestamp();
    void h264ClockTimestampsAreNondecreasing_data();
    void h264ClockTimestampsAreNondecreasing();
    void h264ConsecutiveFieldTimestampsRespectCtType_data();
    void h264ConsecutiveFieldTimestampsRespectCtType();
    void h264CountingSchemesApplyOffsetsBeforeClassification_data();
    void h264CountingSchemesApplyOffsetsBeforeClassification();
    void h264NonCfrTimingIsValidatedBeforeUnsupported();
    void h264SeiMessagesAreFullyValidated();
    void h264SeiMessagesAggregateTypedResults();
    void h264LaterIncompleteClockInheritsUnits_data();
    void h264LaterIncompleteClockInheritsUnits();
    void h264RejectsInvalidTimestampLabels();
    void h264InterpretsCountingTypeAndDroppedFlag();
    void h264ValidatesPartialTimestampFields_data();
    void h264ValidatesPartialTimestampFields();
    void h264RejectsInvalidPayloadAlignment();
    void h264RejectsMalformedEmulationPrevention();
    void h264RejectsInvalidSeiNalHeaders_data();
    void h264RejectsInvalidSeiNalHeaders();
    void h264AnnexBTrailingZeroBytesAreNotNalPayload();
    void rawEbspTrailingZeroBytesRemainStrict();
    void h264MalformedContextIsNotUnsupported();
    void h264PicTimingWithoutContextIsRejected();
    void h264X264FixtureDoesNotInventTimestamp();
    void h264JmFixtureDoesNotInventTimestamp();
    void hevcSpsVuiTimingContextIsParsed();
    void hevcSpsTimingIsBoundToReferencedVps();
    void hevcVpsHrdLayerSetIndexIsValidated();
    void hevcParameterSetsRejectEnhancementLayers();
    void hevcParameterSetsRequireTemporalIdZero();
    void hevcParameterSetConformanceFlagsAreValidated();
    void hevcUnavailableBaseLayerIsUnsupported();
    void hevcSpsStatusPrecedenceIsOrderIndependent();
    void hevcHmFixtureIsDecoded();
    void hevcShmFixtureIsDecoded();
    void hevcTimeCodeDecoded();
    void hevcClockCountAndFlagsAreValidated();
    void hevcFrameFieldInfoIsInferredWithoutVui();
    void hevcInferredFrameFieldInfoControlsClockCount();
    void hevcFrameFieldInfoExplicitZeroIsRejected_data();
    void hevcFrameFieldInfoExplicitZeroIsRejected();
    void hevcFrameFieldInfoRequiresMatchingPictureTiming();
    void hevcPictureTimingSyntaxIsFullyValidated();
    void hevcSourceScanMatchesProfileTierLevel_data();
    void hevcSourceScanMatchesProfileTierLevel();
    void hevcPartialTimestampInheritsEarlierUnits();
    void hevcPartialTimestampInheritsAcrossAccessUnitsPerStream();
    void hevcOutputOrderIdentityIsRequired();
    void hevcOutputOrderHandlesReorderDuplicateAndEpoch();
    void hevcAbsentClockClearsPredecessor();
    void hevcStateDoesNotCrossTimingContexts();
    void hevcSignedOffsetAndDiscontinuityArePreserved();
    void hevcCountingAndDropSemanticsAreValidated();
    void hevcCountingTypesUseNormativeOutputPredecessor_data();
    void hevcCountingTypesUseNormativeOutputPredecessor();
    void hevcDiscontinuityAllowsClockRegression();
    void hevcDropCountRequiresProvenPredecessor();
    void hevcTruncatedTimeCodeIsRejected();
    void hevcSuffixTimeCodeIsIgnored();
    void hevcDuplicateTimeCodeMessagesMustAgree();
    void hevcDuplicateTimeCodeMessagesCannotChainContinuity();
    void hevcEnhancementLayerSeiIsRejected();
    void registeredT35UnknownAndCaptionPayloadsAreIgnored();
    void registeredT35UnknownProviderDoesNotSuppressTimeCode();
    void registeredT35CountryOnlyPayloadPoisonsAccessUnit();
    void noSeiReturnsInvalid();
    void truncatedSeiPayloadReturnsInvalid();
    void truncatedPayloadSizeReturnsInvalid();
    void emptyBufferReturnsInvalid();
    void seiWithoutTimecodePayloadTypeReturnsInvalid();
    void unknownCodecReturnsInvalid();
    void hugePayloadSizeVarintDoesNotOverflowOrCrash();
};

namespace {

const char* kStartCode4 = "\x00\x00\x00\x01";

// Build the big-endian SMPTE 12M 32-bit word for a timecode (the on-wire layout
// the extractor reads back out of the recognised SEI payload).
QByteArray packedWordBytes(const Smpte12mTimecode& tc) {
    const uint32_t word = Smpte12m::toPackedWord(tc);
    QByteArray bytes(4, char(0));
    bytes[0] = char((word >> 24) & 0xFF);
    bytes[1] = char((word >> 16) & 0xFF);
    bytes[2] = char((word >> 8) & 0xFF);
    bytes[3] = char(word & 0xFF);
    return bytes;
}

// Encode a SEI payloadType/payloadSize value as the 0xFF-continuation byte run
// used by the SEI RBSP (a sum of 0xFF bytes followed by a final value < 0xFF).
QByteArray seiVarByte(int value) {
    QByteArray out;
    while (value >= 0xFF) {
        out.append(char(0xFF));
        value -= 0xFF;
    }
    out.append(char(value));
    return out;
}

// Assemble a SEI message body: payloadType, payloadSize, then payload bytes.
QByteArray seiMessage(int payloadType, const QByteArray& payload) {
    return seiVarByte(payloadType) + seiVarByte(payload.size()) + payload;
}

QByteArray escapeRbsp(const QByteArray& rbsp) {
    QByteArray escaped;
    int zeroCount = 0;
    for (char byte : rbsp) {
        const uchar value = uchar(byte);
        if (zeroCount >= 2 && value <= 0x03) {
            escaped.append(char(0x03));
            zeroCount = 0;
        }
        escaped.append(byte);
        zeroCount = value == 0 ? zeroCount + 1 : 0;
    }
    return escaped;
}

// Wrap a SEI RBSP (one or more messages) into an Annex-B SEI NAL with the given
// NAL header bytes, appending the RBSP trailing-bits stop byte (0x80).
QByteArray seiNal(const QByteArray& header, const QByteArray& rbsp) {
    return QByteArray(kStartCode4, 4) + header + escapeRbsp(rbsp + QByteArray(1, char(0x80)));
}

QByteArray h264SeiNal(const QByteArray& rbsp) {
    return seiNal(QByteArray(1, char(0x06)), rbsp); // nal_type 6
}

QByteArray rawH264SeiNal(const QByteArray& rbsp) {
    return QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) + escapeRbsp(rbsp);
}

QByteArray hevcPrefixSeiNal(const QByteArray& rbsp, int layerId = 0) {
    // nal_type 39 (PREFIX_SEI), followed by nuh_layer_id and temporal_id_plus1.
    QByteArray header(2, char(0));
    header[0] = char((39 << 1) | ((layerId >> 5) & 1));
    header[1] = char(((layerId & 0x1f) << 3) | 1);
    return seiNal(header, rbsp);
}

QByteArray hevcSuffixSeiNal(const QByteArray& rbsp) {
    // nal_type 40 (SUFFIX_SEI): byte0 = 40 << 1 = 0x50, byte1 = 0x01.
    return seiNal(QByteArray::fromHex("5001"), rbsp);
}

HevcTimingSyntax hevcTiming(uint32_t numUnitsInTick = 1001, uint32_t timeScale = 30000) {
    HevcTimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.numUnitsInTick = numUnitsInTick;
    syntax.timeScale = timeScale;
    syntax.timingInfoPresent = true;
    return syntax;
}

H26xSeiOutputOrderKey outputOrder(const H26xTimingContext& context, int64_t presentationKey,
                                  uint64_t sourceGeneration = 1, uint64_t domain = 1,
                                  uint64_t epoch = 0) {
    return {sourceGeneration, context.generation(), domain, epoch, presentationKey};
}

// A trivial VCL NAL so the buffer looks like a real access unit.
QByteArray h264VclNal() {
    return QByteArray(kStartCode4, 4) + QByteArray::fromHex("658884"); // IDR slice
}

QByteArray hevcVclNal() {
    return QByteArray(kStartCode4, 4) + QByteArray::fromHex("260100"); // IDR_W_RADL slice
}

QByteArray fixture(const char* name) {
    const QString path =
        QFINDTESTDATA(QStringLiteral("../fixtures/timecode/") + QString::fromLatin1(name));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QByteArray h264SpsFromAnnexB(const QByteArray& annexB) {
    for (int i = 0; i + 5 <= annexB.size(); ++i) {
        int prefix = 0;
        if (annexB.mid(i, 4) == QByteArray::fromHex("00000001")) {
            prefix = 4;
        } else if (annexB.mid(i, 3) == QByteArray::fromHex("000001")) {
            prefix = 3;
        }
        if (prefix == 0 || (uchar(annexB[i + prefix]) & 0x1f) != 7) {
            continue;
        }
        int end = annexB.size();
        for (int j = i + prefix + 1; j + 3 <= annexB.size(); ++j) {
            if (annexB.mid(j, 3) == QByteArray::fromHex("000001") ||
                (j + 4 <= annexB.size() && annexB.mid(j, 4) == QByteArray::fromHex("00000001"))) {
                end = j;
                break;
            }
        }
        return annexB.mid(i + prefix, end - i - prefix);
    }
    return {};
}

struct BitWriter {
    QByteArray bytes;
    int bitPosition = 0;

    void bit(bool value) {
        if (bitPosition == 0) bytes.append(char(0));
        if (value) bytes[bytes.size() - 1] = char(uchar(bytes.back()) | (1u << (7 - bitPosition)));
        bitPosition = (bitPosition + 1) & 7;
    }

    void bits(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bit(((value >> i) & 1u) != 0);
    }

    void signedBits(int32_t value, int count) {
        const uint32_t mask =
            count == 32 ? std::numeric_limits<uint32_t>::max() : (uint32_t(1) << count) - 1u;
        bits(uint32_t(value) & mask, count);
    }

    void ue(uint32_t value) {
        const uint64_t codeNum = uint64_t(value) + 1u;
        int width = 0;
        for (uint64_t probe = codeNum; probe != 0; probe >>= 1)
            ++width;
        for (int i = 0; i < width - 1; ++i)
            bit(false);
        bits(uint32_t(codeNum), width);
    }

    void rbspTrailingBits() {
        bit(true);
        while (bitPosition != 0)
            bit(false);
    }

    void payloadTrailingBits(bool marker = true, bool nonzeroPadding = false) {
        bit(marker);
        bool firstPaddingBit = true;
        while (bitPosition != 0) {
            bit(nonzeroPadding && firstPaddingBit);
            firstPaddingBit = false;
        }
    }
};

QByteArray hevcVps(uint32_t numUnitsInTick = 1001, uint32_t timeScale = 30000, uint32_t vpsId = 0,
                   int hrdLayerSetIndex = -1, bool progressiveSource = true,
                   bool interlacedSource = false) {
    BitWriter writer;
    writer.bits(vpsId, 4); // vps_video_parameter_set_id
    writer.bit(true);  // vps_base_layer_internal_flag
    writer.bit(true);  // vps_base_layer_available_flag
    writer.bits(0, 6); // vps_max_layers_minus1
    writer.bits(0, 3); // vps_max_sub_layers_minus1
    writer.bit(true);  // vps_temporal_id_nesting_flag
    writer.bits(0xffff, 16);
    writer.bits(0, 2);            // general_profile_space
    writer.bit(false);            // general_tier_flag
    writer.bits(1, 5);            // Main profile
    writer.bits(0x80000000u, 32); // Main profile compatibility
    writer.bit(progressiveSource); // progressive_source_flag
    writer.bit(interlacedSource);  // interlaced_source_flag
    writer.bit(false);            // non_packed_constraint_flag
    writer.bit(true);             // frame_only_constraint_flag
    writer.bits(0, 32);
    writer.bits(0, 12);  // reserved_zero_44bits
    writer.bits(120, 8); // general_level_idc
    writer.bit(false);   // sub_layer_ordering_info_present_flag
    writer.ue(0);        // max_dec_pic_buffering_minus1
    writer.ue(0);        // max_num_reorder_pics
    writer.ue(0);        // max_latency_increase_plus1
    writer.bits(0, 6);   // vps_max_layer_id
    writer.ue(0);        // vps_num_layer_sets_minus1
    writer.bit(true);    // vps_timing_info_present_flag
    writer.bits(numUnitsInTick, 32);
    writer.bits(timeScale, 32);
    writer.bit(true);  // vps_poc_proportional_to_timing_flag
    writer.ue(0);      // vps_num_ticks_poc_diff_one_minus1
    writer.ue(hrdLayerSetIndex < 0 ? 0 : 1); // vps_num_hrd_parameters
    if (hrdLayerSetIndex >= 0) {
        writer.ue(uint32_t(hrdLayerSetIndex)); // hrd_layer_set_idx[0]
        writer.bit(false);                     // nal_hrd_parameters_present_flag
        writer.bit(false);                     // vcl_hrd_parameters_present_flag
        writer.bit(true);                      // fixed_pic_rate_general_flag[0]
        writer.ue(0);                          // elemental_duration_in_tc_minus1[0]
        writer.ue(0);                          // cpb_cnt_minus1[0]
    }
    writer.bit(false); // vps_extension_flag
    writer.rbspTrailingBits();
    return QByteArray::fromHex("4001") + escapeRbsp(writer.bytes);
}

QList<QByteArray> hevcNalsOfType(const QByteArray& annexB, int wantedType) {
    QList<QByteArray> result;
    for (int i = 0; i + 5 <= annexB.size();) {
        int prefix = 0;
        if (annexB.mid(i, 4) == QByteArray::fromHex("00000001"))
            prefix = 4;
        else if (annexB.mid(i, 3) == QByteArray::fromHex("000001"))
            prefix = 3;
        if (prefix == 0) {
            ++i;
            continue;
        }
        const int nalStart = i + prefix;
        int end = annexB.size();
        for (int j = nalStart + 2; j + 3 <= annexB.size(); ++j) {
            if (annexB.mid(j, 3) == QByteArray::fromHex("000001") ||
                (j + 4 <= annexB.size() && annexB.mid(j, 4) == QByteArray::fromHex("00000001"))) {
                end = j;
                break;
            }
        }
        if (((uchar(annexB[nalStart]) >> 1) & 0x3f) == wantedType)
            result.append(annexB.mid(nalStart, end - nalStart));
        i = end;
    }
    return result;
}

QByteArray hevcSps(uint32_t numUnitsInTick = 1001, uint32_t timeScale = 30000,
                   uint32_t referencedVpsId = 0, bool frameFieldInfoPresent = false,
                   bool timingInfoPresent = true, bool fieldSeq = false,
                   bool progressiveSource = true, bool interlacedSource = false,
                   bool vuiParametersPresent = true) {
    BitWriter writer;
    writer.bits(referencedVpsId, 4); // sps_video_parameter_set_id
    writer.bits(0, 3); // sps_max_sub_layers_minus1
    writer.bit(true);  // sps_temporal_id_nesting_flag
    writer.bits(0, 2);
    writer.bit(false);
    writer.bits(1, 5);
    writer.bits(0x80000000u, 32);
    writer.bit(progressiveSource);
    writer.bit(interlacedSource);
    writer.bit(false);
    writer.bit(true);
    writer.bits(0, 32);
    writer.bits(0, 12);
    writer.bits(120, 8);
    writer.ue(0);      // sps_seq_parameter_set_id
    writer.ue(1);      // chroma_format_idc: 4:2:0
    writer.ue(64);     // pic_width_in_luma_samples
    writer.ue(64);     // pic_height_in_luma_samples
    writer.bit(false); // conformance_window_flag
    writer.ue(0);      // bit_depth_luma_minus8
    writer.ue(0);      // bit_depth_chroma_minus8
    writer.ue(4);      // log2_max_pic_order_cnt_lsb_minus4
    writer.bit(false); // sub_layer_ordering_info_present_flag
    writer.ue(0);
    writer.ue(0);
    writer.ue(0);
    writer.ue(0);      // log2_min_luma_coding_block_size_minus3
    writer.ue(3);      // log2_diff_max_min_luma_coding_block_size
    writer.ue(0);      // log2_min_luma_transform_block_size_minus2
    writer.ue(3);      // log2_diff_max_min_luma_transform_block_size
    writer.ue(0);      // max_transform_hierarchy_depth_inter
    writer.ue(0);      // max_transform_hierarchy_depth_intra
    writer.bit(false); // scaling_list_enabled_flag
    writer.bit(true);  // amp_enabled_flag
    writer.bit(true);  // sample_adaptive_offset_enabled_flag
    writer.bit(false); // pcm_enabled_flag
    writer.ue(0);      // num_short_term_ref_pic_sets
    writer.bit(false); // long_term_ref_pics_present_flag
    writer.bit(true);  // sps_temporal_mvp_enabled_flag
    writer.bit(true);  // strong_intra_smoothing_enabled_flag
    writer.bit(vuiParametersPresent); // vui_parameters_present_flag
    if (vuiParametersPresent) {
        writer.bit(false);                 // aspect_ratio_info_present_flag
        writer.bit(false);                 // overscan_info_present_flag
        writer.bit(false);                 // video_signal_type_present_flag
        writer.bit(false);                 // chroma_loc_info_present_flag
        writer.bit(false);                 // neutral_chroma_indication_flag
        writer.bit(fieldSeq);              // field_seq_flag
        writer.bit(frameFieldInfoPresent); // frame_field_info_present_flag
        writer.bit(false);                 // default_display_window_flag
        writer.bit(timingInfoPresent);     // vui_timing_info_present_flag
        if (timingInfoPresent) {
            writer.bits(numUnitsInTick, 32);
            writer.bits(timeScale, 32);
            writer.bit(true);  // vui_poc_proportional_to_timing_flag
            writer.ue(0);      // vui_num_ticks_poc_diff_one_minus1
            writer.bit(false); // vui_hrd_parameters_present_flag
        }
        writer.bit(false); // bitstream_restriction_flag
    }
    writer.bit(false); // sps_extension_present_flag
    writer.rbspTrailingBits();
    return QByteArray::fromHex("4201") + escapeRbsp(writer.bytes);
}

QByteArray hevcNalWithLayerId(QByteArray nal, int layerId) {
    nal[0] = char((uchar(nal[0]) & 0xfeu) | ((layerId >> 5) & 1));
    nal[1] = char(((layerId & 0x1f) << 3) | (uchar(nal[1]) & 0x07u));
    return nal;
}

QByteArray hevcNalWithTemporalIdPlus1(QByteArray nal, int temporalIdPlus1) {
    nal[1] = char((uchar(nal[1]) & 0xf8u) | (temporalIdPlus1 & 0x07));
    return nal;
}

void writeFullTimestamp(BitWriter& writer, int hours, int minutes, int seconds, int frames,
                        int countingType = 0, bool countDropped = false, bool nuitFieldBased = true,
                        int32_t timeOffset = 0, int timeOffsetLength = 0,
                        bool discontinuity = false, int ctType = 0) {
    writer.bit(true); // clock_timestamp_flag[0]
    writer.bits(uint32_t(ctType), 2);
    writer.bit(nuitFieldBased);
    writer.bits(uint32_t(countingType), 5);
    writer.bit(true); // full_timestamp_flag
    writer.bit(discontinuity);
    writer.bit(countDropped);
    writer.bits(uint32_t(frames), 8);
    writer.bits(uint32_t(seconds), 6);
    writer.bits(uint32_t(minutes), 6);
    writer.bits(uint32_t(hours), 5);
    writer.signedBits(timeOffset, timeOffsetLength);
}

QByteArray fullTimestampPayload(int hours, int minutes, int seconds, int frames,
                                int countingType = 0, bool countDropped = false,
                                bool nuitFieldBased = true) {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writeFullTimestamp(writer, hours, minutes, seconds, frames, countingType, countDropped,
                       nuitFieldBased);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

void writeHevcFullTimestamp(BitWriter& writer, int hours, int minutes, int seconds, int frames,
                            int countingType = 0, bool countDropped = false,
                            bool unitsFieldBased = false, int32_t timeOffset = 0,
                            int timeOffsetLength = 0, bool discontinuity = false) {
    writer.bit(true); // clock_timestamp_flag
    writer.bit(unitsFieldBased);
    writer.bits(uint32_t(countingType), 5);
    writer.bit(true); // full_timestamp_flag
    writer.bit(discontinuity);
    writer.bit(countDropped);
    writer.bits(uint32_t(frames), 9);
    writer.bits(uint32_t(seconds), 6);
    writer.bits(uint32_t(minutes), 6);
    writer.bits(uint32_t(hours), 5);
    writer.bits(uint32_t(timeOffsetLength), 5);
    writer.signedBits(timeOffset, timeOffsetLength);
}

QByteArray hevcFullTimestampPayload(int hours, int minutes, int seconds, int frames,
                                    int countingType = 0, bool countDropped = false,
                                    bool unitsFieldBased = false, int32_t timeOffset = 0,
                                    int timeOffsetLength = 0, bool discontinuity = false,
                                    int clockCount = 1) {
    BitWriter writer;
    writer.bits(uint32_t(clockCount), 2);
    writeHevcFullTimestamp(writer, hours, minutes, seconds, frames, countingType, countDropped,
                           unitsFieldBased, timeOffset, timeOffsetLength, discontinuity);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray hevcPictureTimingPayload(int picStruct, int sourceScanType = 1) {
    BitWriter writer;
    writer.bits(uint32_t(picStruct), 4);
    writer.bits(uint32_t(sourceScanType), 2);
    writer.bit(false); // duplicate_flag
    writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray hevcClockTimestampPayload(int clockCount, int hours, int minutes, int seconds,
                                     int frames) {
    BitWriter writer;
    writer.bits(uint32_t(clockCount), 2);
    writeHevcFullTimestamp(writer, hours, minutes, seconds, frames);
    for (int i = 1; i < clockCount; ++i)
        writer.bit(false);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray hevcDecreasingDiscontinuousClockPayload() {
    BitWriter writer;
    writer.bits(2, 2); // num_clock_ts
    writeHevcFullTimestamp(writer, 1, 2, 3, 5);
    writeHevcFullTimestamp(writer, 1, 2, 3, 4, 0, false, false, 0, 0, true);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray hevcNoUnitsTimestampPayload(int frames) {
    BitWriter writer;
    writer.bits(1, 2); // num_clock_ts
    writer.bit(true);  // clock_timestamp_flag
    writer.bit(false); // units_field_based_flag
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(uint32_t(frames), 9);
    writer.bit(false); // seconds_flag: inherit all clock units
    writer.bits(0, 5); // time_offset_length
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray hevcAbsentThenNoUnitsTimestampPayload(int frames) {
    BitWriter writer;
    writer.bits(2, 2); // num_clock_ts
    writer.bit(false); // clock_timestamp_flag[0]
    writer.bit(true);  // clock_timestamp_flag[1]
    writer.bit(false); // units_field_based_flag
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(uint32_t(frames), 9);
    writer.bit(false); // seconds_flag: predecessor clock is absent
    writer.bits(0, 5); // time_offset_length
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

QByteArray partialTimestampPayload(int frames = 4, int seconds = 3, int minutes = -1,
                                   int hours = -1) {
    BitWriter writer;
    writer.bits(0, 4); // pic_struct: frame
    writer.bit(true);  // clock_timestamp_flag[0]
    writer.bits(0, 2); // ct_type: progressive
    writer.bit(true);  // nuit_field_based_flag: n_frames has a frame-rate unit
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(uint32_t(frames), 8);
    writer.bit(seconds >= 0);
    if (seconds >= 0) {
        writer.bits(uint32_t(seconds), 6);
        writer.bit(minutes >= 0);
        if (minutes >= 0) {
            writer.bits(uint32_t(minutes), 6);
            writer.bit(hours >= 0);
            if (hours >= 0) writer.bits(uint32_t(hours), 5);
        }
    }
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    return writer.bytes;
}

} // namespace

void TestH26xSeiTimecode::h264StandardPicTimingIsDecoded() {
    const QByteArray annexB = fixture("h264_pic_timing_no_hrd.264");
    QVERIFY(!annexB.isEmpty());
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(annexB)}));
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 1);
    QCOMPARE(got.minutes, 2);
    QCOMPARE(got.seconds, 3);
    QCOMPARE(got.frames, 4);
}

void TestH26xSeiTimecode::h264PicTimingWithHrdIsDecoded() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    const Smpte12mTimecode got =
        extractH26xSeiTimecode(fixture("h264_pic_timing_hrd.264"), NativeVideoCodec::H264, context);
    QVERIFY(got.valid);
    QCOMPARE(got.hours, 10);
    QCOMPARE(got.minutes, 11);
    QCOMPARE(got.seconds, 12);
    QCOMPARE(got.frames, 13);
}

void TestH26xSeiTimecode::h264LeadingBcdWithoutTimestampIsIgnored() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    const Smpte12mTimecode got = extractH26xSeiTimecode(fixture("h264_bcd_false_positive.264"),
                                                        NativeVideoCodec::H264, context);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::h264TruncatedClockTimestampIsRejected() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    QByteArray truncated = fixture("h264_pic_timing_hrd.264");
    truncated.truncate(13); // declared eight-byte pic_timing payload has only six bytes
    const Smpte12mTimecode got = extractH26xSeiTimecode(truncated, NativeVideoCodec::H264, context);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::h264TimeOffsetIsBoundsChecked() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {fixture("h264_sps_hrd.bin")}));
    H264TimingSyntax constructedSyntax = *context.h264();
    constructedSyntax.timeOffsetLength = 5;
    BitWriter payloadWriter;
    payloadWriter.bits(0, constructedSyntax.cpbRemovalDelayLength);
    payloadWriter.bits(0, constructedSyntax.dpbOutputDelayLength);
    payloadWriter.bits(0, 4); // pic_struct: frame
    writeFullTimestamp(payloadWriter, 10, 11, 12, 13);
    payloadWriter.bits(0, constructedSyntax.timeOffsetLength);
    if (payloadWriter.bitPosition != 0) payloadWriter.payloadTrailingBits();
    const QByteArray payload = payloadWriter.bytes;
    auto parsed = H26xTimingDetail::parseH264PicTiming(payload, constructedSyntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(parsed.timecode.valid);

    constructedSyntax.timeOffsetLength = 24;
    parsed = H26xTimingDetail::parseH264PicTiming(payload, constructedSyntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);
}

void TestH26xSeiTimecode::h264PartialTimestampIsNotReturned() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    const QByteArray annexB = h264SeiNal(seiMessage(1, partialTimestampPayload())) + h264VclNal();
    const Smpte12mTimecode parsed = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context);
    QVERIFY(!parsed.valid);
}

void TestH26xSeiTimecode::h264OutOfRateFrameLabelIsRejected() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{25, 1}));

    const QByteArray annexB =
        h264SeiNal(seiMessage(1, fullTimestampPayload(1, 2, 3, 25))) + h264VclNal();
    const Smpte12mTimecode parsed = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context);
    QVERIFY(!parsed.valid);
}

void TestH26xSeiTimecode::h264ConsumesEveryClockTimestamp() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    BitWriter truncatedLaterClock;
    truncatedLaterClock.bits(3, 4); // pic_struct: top field, bottom field (two clocks)
    writeFullTimestamp(truncatedLaterClock, 1, 2, 3, 4);
    truncatedLaterClock.bit(true);  // clock_timestamp_flag[1]
    truncatedLaterClock.bits(0, 2); // truncated ct_type only
    auto parsed = H26xTimingDetail::parseH264PicTiming(truncatedLaterClock.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);

    BitWriter malformedLaterClock;
    malformedLaterClock.bits(3, 4);
    writeFullTimestamp(malformedLaterClock, 1, 2, 3, 4);
    writeFullTimestamp(malformedLaterClock, 24, 0, 0, 0);
    malformedLaterClock.payloadTrailingBits();
    parsed = H26xTimingDetail::parseH264PicTiming(malformedLaterClock.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);

    BitWriter malformedThirdClock;
    malformedThirdClock.bits(5, 4); // pic_struct: three clock_timestamp_flag entries
    writeFullTimestamp(malformedThirdClock, 1, 2, 3, 4);
    malformedThirdClock.bit(false);
    malformedThirdClock.bit(true);
    malformedThirdClock.bits(0, 1); // truncated clock_timestamp[2]
    parsed = H26xTimingDetail::parseH264PicTiming(malformedThirdClock.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    QVERIFY(!parsed.timecode.valid);
}

void TestH26xSeiTimecode::h264ClockTimestampsAreNondecreasing_data() {
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<int>("timeOffsetLength");
    QTest::addColumn<int>("expectedStatus");
    QTest::addColumn<int>("expectedHours");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    const auto payload = [](int picStruct, auto writeClocks) {
        BitWriter writer;
        writer.bits(uint32_t(picStruct), 4);
        writeClocks(writer);
        if (writer.bitPosition != 0) writer.payloadTrailingBits();
        return writer.bytes;
    };

    QTest::newRow("three clocks equal then increasing") << payload(5, [](BitWriter& writer) {
        writeFullTimestamp(writer, 1, 2, 3, 4);
        writeFullTimestamp(writer, 1, 2, 3, 4);
        writeFullTimestamp(writer, 1, 2, 3, 5);
    }) << 0 << int(Status::Valid) << 1;
    QTest::newRow("complete clocks reversed") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 1, 2, 3, 5);
        writeFullTimestamp(writer, 1, 2, 3, 4);
    }) << 0 << int(Status::Malformed) << -1;
    QTest::newRow("current discontinuity does not permit reversal")
        << payload(3,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 5);
                       writeFullTimestamp(writer, 1, 2, 3, 4, 0, false, true, 0, 0, true);
                   })
        << 0 << int(Status::Malformed) << -1;
    QTest::newRow("middle discontinuity does not reset three-clock ordering")
        << payload(5,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 8);
                       writeFullTimestamp(writer, 1, 2, 3, 4, 0, false, true, 0, 0, true);
                       writeFullTimestamp(writer, 1, 2, 3, 5);
                   })
        << 0 << int(Status::Malformed) << -1;
    QTest::newRow("third current discontinuity does not permit reversal")
        << payload(5,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 4);
                       writeFullTimestamp(writer, 1, 2, 3, 5);
                       writeFullTimestamp(writer, 1, 2, 3, 3, 0, false, true, 0, 0, true);
                   })
        << 0 << int(Status::Malformed) << -1;
    QTest::newRow("increasing three clocks allow middle discontinuity")
        << payload(5,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 1, 2, 3, 4);
                       writeFullTimestamp(writer, 1, 2, 3, 5, 0, false, true, 0, 0, true);
                       writeFullTimestamp(writer, 1, 2, 3, 6);
                   })
        << 0 << int(Status::Valid) << 1;
    QTest::newRow("type zero offset cannot compensate reversal")
        << payload(3,
                   [](BitWriter& writer) {
                       writeFullTimestamp(writer, 0, 0, 0, 5, 0, false, true, -4, 4);
                       writeFullTimestamp(writer, 0, 0, 0, 4, 0, false, true, 0, 4);
                   })
        << 4 << int(Status::Malformed) << -1;
    QTest::newRow("type one offset compensates reversal") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 0, 0, 0, 5, 1, false, true, -4, 4);
        writeFullTimestamp(writer, 0, 0, 0, 4, 1, false, true, 0, 4);
    }) << 4 << int(Status::Valid) << 0;
    QTest::newRow("signed offsets reverse increasing labels") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 0, 0, 0, 0, 1, false, true, 2, 4);
        writeFullTimestamp(writer, 0, 0, 0, 1, 1, false, true, -1, 4);
    }) << 4 << int(Status::Malformed) << -1;
    QTest::newRow("field basis reverses equal labels") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 0, 0, 0, 1, 1, false, true);
        writeFullTimestamp(writer, 0, 0, 0, 1, 1, false, false);
    }) << 0 << int(Status::Malformed) << -1;
    QTest::newRow("unadjusted day wrap decreases") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 23, 59, 59, 24, 1);
        writeFullTimestamp(writer, 0, 0, 0, 0, 1);
    }) << 0 << int(Status::Malformed) << -1;
    QTest::newRow("day wrap offset remains nondecreasing") << payload(3, [](BitWriter& writer) {
        writeFullTimestamp(writer, 23, 59, 59, 24, 1, false, true, 0, 24);
        writeFullTimestamp(writer, 0, 0, 0, 0, 1, false, true, 4'320'000, 24);
    }) << 24 << int(Status::Valid) << 23;
}

void TestH26xSeiTimecode::h264SeiMessagesAreFullyValidated() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray valid = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray truncated =
        valid + seiVarByte(1) + seiVarByte(4) + QByteArray::fromHex("0a0b");
    QVERIFY(
        !extractH26xSeiTimecode(rawH264SeiNal(truncated), NativeVideoCodec::H264, context).valid);

    BitWriter reservedCountingType;
    reservedCountingType.bits(0, 4);
    writeFullTimestamp(reservedCountingType, 1, 2, 3, 5, 7);
    reservedCountingType.payloadTrailingBits();
    const QByteArray malformedType = valid + seiMessage(1, reservedCountingType.bytes);
    QVERIFY(
        !extractH26xSeiTimecode(h264SeiNal(malformedType), NativeVideoCodec::H264, context).valid);

    QVERIFY(!extractH26xSeiTimecode(rawH264SeiNal(valid), NativeVideoCodec::H264, context).valid);
    QVERIFY(!extractH26xSeiTimecode(rawH264SeiNal(valid + QByteArray(1, char(0x81))),
                                    NativeVideoCodec::H264, context)
                 .valid);

    const QByteArray validFirstNal = h264SeiNal(valid);
    QVERIFY(!extractH26xSeiTimecode(validFirstNal + rawH264SeiNal(QByteArray::fromHex("0104aabb")),
                                    NativeVideoCodec::H264, context)
                 .valid);

    for (const int zeroCount : {1, 2, 9}) {
        const auto parsed =
            extractH26xSeiTimecode(h264SeiNal(valid + seiMessage(5, QByteArray::fromHex("aabb"))) +
                                       QByteArray(zeroCount, char(0)),
                                   NativeVideoCodec::H264, context);
        QVERIFY2(parsed.valid,
                 qPrintable(QStringLiteral("multi-message trailing zeros=%1").arg(zeroCount)));
        QCOMPARE(parsed.frames, 4);
    }
}

void TestH26xSeiTimecode::h264SeiMessagesAggregateTypedResults() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray first = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray later = seiMessage(1, fullTimestampPayload(2, 3, 4, 5));
    auto parsed =
        extractH26xSeiTimecode(h264SeiNal(first + later), NativeVideoCodec::H264, context);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.hours, 1);
    QCOMPARE(parsed.minutes, 2);
    QCOMPARE(parsed.seconds, 3);
    QCOMPARE(parsed.frames, 4);

    parsed =
        extractH26xSeiTimecode(h264SeiNal(first + seiMessage(5, QByteArray::fromHex("aabbcc"))),
                               NativeVideoCodec::H264, context);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.frames, 4);

    const QByteArray unsupported = seiMessage(1, fullTimestampPayload(1, 2, 3, 5, 5));
    QVERIFY(
        !extractH26xSeiTimecode(h264SeiNal(first + unsupported), NativeVideoCodec::H264, context)
             .valid);
    QVERIFY(
        !extractH26xSeiTimecode(h264SeiNal(unsupported + later), NativeVideoCodec::H264, context)
             .valid);
}

void TestH26xSeiTimecode::h264ClockTimestampsAreNondecreasing() {
    QFETCH(QByteArray, payload);
    QFETCH(int, timeOffsetLength);
    QFETCH(int, expectedStatus);
    QFETCH(int, expectedHours);

    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    H264TimingSyntax syntax = *context.h264();
    syntax.timeOffsetLength = uint8_t(timeOffsetLength);

    const auto parsed = H26xTimingDetail::parseH264PicTiming(payload, syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
    if (parsed.timecode.valid) QCOMPARE(parsed.timecode.hours, expectedHours);
}

void TestH26xSeiTimecode::h264ConsecutiveFieldTimestampsRespectCtType_data() {
    QTest::addColumn<int>("picStruct");
    QTest::addColumn<int>("firstCtType");
    QTest::addColumn<int>("secondCtType");
    QTest::addColumn<bool>("equalClockTimestamps");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    for (const int picStruct : {3, 4, 5, 6}) {
        const auto row = [picStruct](const char* description, int firstCtType, int secondCtType,
                                     bool equal, Status status) {
            QTest::newRow(qPrintable(QStringLiteral("pic_struct %1 %2")
                                         .arg(picStruct)
                                         .arg(QString::fromLatin1(description))))
                << picStruct << firstCtType << secondCtType << equal << int(status);
        };
        row("equal progressive", 0, 0, true, Status::Valid);
        row("equal progressive unknown", 0, 2, true, Status::Valid);
        row("equal unknown", 2, 2, true, Status::Valid);
        row("equal second interlaced", 0, 1, true, Status::Malformed);
        row("equal first interlaced", 1, 0, true, Status::Malformed);
        row("equal both interlaced", 1, 1, true, Status::Malformed);
        row("different second interlaced", 0, 1, false, Status::Valid);
        row("different first interlaced", 1, 0, false, Status::Valid);
    }
    QTest::newRow("pic_struct 7 equal interlaced frame timestamps")
        << 7 << 1 << 1 << true << int(Status::Valid);
}

void TestH26xSeiTimecode::h264ConsecutiveFieldTimestampsRespectCtType() {
    QFETCH(int, picStruct);
    QFETCH(int, firstCtType);
    QFETCH(int, secondCtType);
    QFETCH(bool, equalClockTimestamps);
    QFETCH(int, expectedStatus);

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.numUnitsInTick = 1;
    syntax.timeScale = 50;
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    BitWriter writer;
    writer.bits(uint32_t(picStruct), 4);
    writeFullTimestamp(writer, 1, 2, 3, 4, 0, false, true, 0, 0, false, firstCtType);
    writeFullTimestamp(writer, 1, 2, 3, equalClockTimestamps ? 4 : 5, 0, false, true, 0, 0, false,
                       secondCtType);
    if (picStruct == 5 || picStruct == 6)
        writeFullTimestamp(writer, 1, 2, 3, equalClockTimestamps ? 5 : 6);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();

    const auto parsed = H26xTimingDetail::parseH264PicTiming(writer.bytes, syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
}

void TestH26xSeiTimecode::h264CountingSchemesApplyOffsetsBeforeClassification_data() {
    QTest::addColumn<int>("countingType");
    QTest::addColumn<bool>("reversedLabels");
    QTest::addColumn<int>("firstOffset");
    QTest::addColumn<int>("secondOffset");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::newRow("type 4 offset compensates reversed 29.97 DF labels")
        << 4 << true << -2002 << 0 << int(Status::Valid);
    for (const int countingType : {2, 3, 5, 6}) {
        QTest::newRow(
            qPrintable(QStringLiteral("type %1 offset reversal is malformed").arg(countingType)))
            << countingType << false << 2003 << 0 << int(Status::Malformed);
    }
}

void TestH26xSeiTimecode::h264CountingSchemesApplyOffsetsBeforeClassification() {
    QFETCH(int, countingType);
    QFETCH(bool, reversedLabels);
    QFETCH(int, firstOffset);
    QFETCH(int, secondOffset);
    QFETCH(int, expectedStatus);

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.numUnitsInTick = 1001;
    syntax.timeScale = 60000;
    syntax.fixedFrameRate = true;
    syntax.timeOffsetLength = 12;
    syntax.picStructPresent = true;

    BitWriter writer;
    writer.bits(3, 4); // pic_struct: top field, bottom field
    writeFullTimestamp(writer, 0, 0, 0, reversedLabels ? 5 : 0, countingType, false, true,
                       firstOffset, syntax.timeOffsetLength);
    writeFullTimestamp(writer, 0, 0, 0, reversedLabels ? 4 : 1, countingType, false, true,
                       secondOffset, syntax.timeOffsetLength);
    if (writer.bitPosition != 0) writer.payloadTrailingBits();

    const auto parsed = H26xTimingDetail::parseH264PicTiming(writer.bytes, syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
    if (parsed.timecode.valid) QVERIFY(parsed.timecode.dropFrame);
}

void TestH26xSeiTimecode::h264NonCfrTimingIsValidatedBeforeUnsupported() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.numUnitsInTick = 1001;
    syntax.timeScale = 60000;
    syntax.fixedFrameRate = false;
    syntax.picStructPresent = true;

    auto parsed = H26xTimingDetail::parseH264PicTiming(fullTimestampPayload(1, 2, 3, 4), syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Unsupported);
    QVERIFY(!parsed.timecode.valid);

    parsed = H26xTimingDetail::parseH264PicTiming(fullTimestampPayload(1, 2, 3, 30), syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    parsed = H26xTimingDetail::parseH264PicTiming(fullTimestampPayload(0, 1, 0, 0, 4, false, true),
                                                  syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    BitWriter reversedClocks;
    reversedClocks.bits(3, 4);
    writeFullTimestamp(reversedClocks, 1, 2, 3, 5);
    writeFullTimestamp(reversedClocks, 1, 2, 3, 4);
    if (reversedClocks.bitPosition != 0) reversedClocks.payloadTrailingBits();
    parsed = H26xTimingDetail::parseH264PicTiming(reversedClocks.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::h264LaterIncompleteClockInheritsUnits_data() {
    QTest::addColumn<int>("seconds");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::newRow("inherits all units and advances frame") << -1 << int(Status::Valid);
    QTest::newRow("inherits minute and hour but decreases second") << 2 << int(Status::Malformed);
    QTest::newRow("inherits minute and hour and advances second") << 4 << int(Status::Valid);
}

void TestH26xSeiTimecode::h264LaterIncompleteClockInheritsUnits() {
    QFETCH(int, seconds);
    QFETCH(int, expectedStatus);

    BitWriter writer;
    writer.bits(3, 4); // pic_struct: top field, bottom field
    writeFullTimestamp(writer, 1, 2, 3, 4);
    writer.bit(true);  // clock_timestamp_flag[1]
    writer.bits(0, 2); // ct_type
    writer.bit(true);  // nuit_field_based_flag
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(5, 8); // n_frames
    writer.bit(seconds >= 0);
    if (seconds >= 0) {
        writer.bits(uint32_t(seconds), 6);
        writer.bit(false); // inherit minutes_value and hours_value
    }
    if (writer.bitPosition != 0) writer.payloadTrailingBits();

    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));
    const auto parsed = H26xTimingDetail::parseH264PicTiming(writer.bytes, *context.h264());
    QCOMPARE(int(parsed.status), expectedStatus);
    QCOMPARE(parsed.timecode.valid,
             expectedStatus == int(H26xTimingDetail::TimecodeParseStatus::Valid));
    if (parsed.timecode.valid) {
        QCOMPARE(parsed.timecode.hours, 1);
        QCOMPARE(parsed.timecode.minutes, 2);
        QCOMPARE(parsed.timecode.seconds, 3);
        QCOMPARE(parsed.timecode.frames, 4);
    }
}

void TestH26xSeiTimecode::h264RejectsInvalidTimestampLabels() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    const struct {
        int hours;
        int minutes;
        int seconds;
        int frames;
        int countingType;
    } cases[] = {
        {24, 0, 0, 0, 0}, {0, 60, 0, 0, 0}, {0, 0, 60, 0, 0}, {0, 0, 0, 25, 0}, {0, 0, 0, 0, 7}};
    for (const auto& test : cases) {
        const auto parsed = H26xTimingDetail::parseH264PicTiming(
            fullTimestampPayload(test.hours, test.minutes, test.seconds, test.frames,
                                 test.countingType),
            syntax);
        QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
        QVERIFY(!parsed.timecode.valid);
    }
}

void TestH26xSeiTimecode::h264InterpretsCountingTypeAndDroppedFlag() {
    struct Case {
        FrameRateQ rate;
        int countingType;
        bool countDropped;
        bool nuitFieldBased;
        int minutes;
        int seconds;
        int frames;
        H26xTimingDetail::TimecodeParseStatus status;
        bool dropFrame;
    };
    const Case cases[] = {
        {{25, 1}, 0, false, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {{25, 1}, 0, true, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {{25, 1}, 1, false, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Valid, false},
        {{25, 1}, 1, true, true, 1, 0, 0, H26xTimingDetail::TimecodeParseStatus::Malformed, false},
        {{25, 1},
         0,
         false,
         false,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         2,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         2,
         true,
         true,
         1,
         0,
         1,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         2,
         true,
         true,
         1,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         3,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         3,
         true,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         3,
         true,
         true,
         1,
         0,
         1,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        // Type 4 is the NTSC drop-frame scheme even when this timestamp did not skip.
        {{30000, 1001},
         4,
         false,
         true,
         1,
         1,
         0,
         H26xTimingDetail::TimecodeParseStatus::Valid,
         true},
        // The two forbidden minute-boundary labels stay illegal independently of cnt_dropped_flag.
        {{30000, 1001},
         4,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         4,
         false,
         true,
         1,
         0,
         1,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001}, 4, true, true, 1, 0, 2, H26xTimingDetail::TimecodeParseStatus::Valid, true},
        // Tenth-minute boundaries do not skip and frame 00 is legal drop-frame timecode.
        {{30000, 1001},
         4,
         false,
         true,
         10,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Valid,
         true},
        {{30000, 1001},
         4,
         true,
         true,
         0,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         4,
         true,
         true,
         1,
         1,
         2,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         4,
         true,
         true,
         1,
         0,
         3,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        // The type-4 two-label skip is not SMPTE 59.94 DF's four-label scheme.
        {{60000, 1001},
         4,
         false,
         true,
         1,
         1,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         4,
         false,
         false,
         1,
         1,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         5,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         5,
         true,
         true,
         1,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         6,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         6,
         true,
         true,
         1,
         0,
         2,
         H26xTimingDetail::TimecodeParseStatus::Unsupported,
         false},
        {{30000, 1001},
         7,
         false,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
        {{30000, 1001},
         31,
         true,
         true,
         1,
         0,
         0,
         H26xTimingDetail::TimecodeParseStatus::Malformed,
         false},
    };

    for (const Case& test : cases) {
        H264TimingSyntax syntax;
        syntax.status = H26xTimingSyntaxStatus::Valid;
        syntax.frameRate = test.rate;
        syntax.fixedFrameRate = true;
        syntax.picStructPresent = true;
        const auto parsed = H26xTimingDetail::parseH264PicTiming(
            fullTimestampPayload(1, test.minutes, test.seconds, test.frames, test.countingType,
                                 test.countDropped, test.nuitFieldBased),
            syntax);
        QCOMPARE(parsed.status, test.status);
        QCOMPARE(parsed.timecode.valid,
                 test.status == H26xTimingDetail::TimecodeParseStatus::Valid);
        if (parsed.timecode.valid) QCOMPARE(parsed.timecode.dropFrame, test.dropFrame);
    }

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {30000, 1001};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;
    BitWriter inheritedDropUnits;
    inheritedDropUnits.bits(0, 4);
    inheritedDropUnits.bit(true);  // clock_timestamp_flag
    inheritedDropUnits.bits(0, 2); // ct_type
    inheritedDropUnits.bit(true);  // nuit_field_based_flag
    inheritedDropUnits.bits(4, 5); // counting_type: NTSC two-lowest-count method
    inheritedDropUnits.bit(false); // full_timestamp_flag
    inheritedDropUnits.bit(false); // discontinuity_flag
    inheritedDropUnits.bit(true);  // cnt_dropped_flag
    inheritedDropUnits.bits(2, 8); // n_frames
    inheritedDropUnits.bit(false); // seconds_flag: inherit prior seconds/minutes
    inheritedDropUnits.payloadTrailingBits();
    const auto inherited = H26xTimingDetail::parseH264PicTiming(inheritedDropUnits.bytes, syntax);
    QCOMPARE(inherited.status, H26xTimingDetail::TimecodeParseStatus::Unsupported);
    QVERIFY(!inherited.timecode.valid);
}

void TestH26xSeiTimecode::h264ValidatesPartialTimestampFields_data() {
    QTest::addColumn<int>("rateNum");
    QTest::addColumn<int>("rateDen");
    QTest::addColumn<int>("frames");
    QTest::addColumn<int>("seconds");
    QTest::addColumn<int>("minutes");
    QTest::addColumn<int>("hours");
    QTest::addColumn<int>("expectedStatus");

    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::newRow("no-units") << 25 << 1 << 4 << -1 << -1 << -1 << int(Status::NoTimestamp);
    QTest::newRow("seconds-only") << 25 << 1 << 4 << 3 << -1 << -1 << int(Status::NoTimestamp);
    QTest::newRow("minutes-no-hours") << 25 << 1 << 4 << 3 << 2 << -1 << int(Status::NoTimestamp);
    QTest::newRow("seconds-out-of-range")
        << 25 << 1 << 4 << 60 << -1 << -1 << int(Status::Malformed);
    QTest::newRow("minutes-out-of-range")
        << 25 << 1 << 4 << 3 << 60 << -1 << int(Status::Malformed);
    QTest::newRow("hours-out-of-range") << 25 << 1 << 4 << 3 << 2 << 24 << int(Status::Malformed);
    QTest::newRow("integer-rate-frame-out-of-range")
        << 25 << 1 << 25 << -1 << -1 << -1 << int(Status::Malformed);
    QTest::newRow("fractional-rate-last-frame")
        << 30000 << 1001 << 29 << -1 << -1 << -1 << int(Status::NoTimestamp);
    QTest::newRow("fractional-rate-frame-out-of-range")
        << 30000 << 1001 << 30 << -1 << -1 << -1 << int(Status::Malformed);
}

void TestH26xSeiTimecode::h264ValidatesPartialTimestampFields() {
    QFETCH(int, rateNum);
    QFETCH(int, rateDen);
    QFETCH(int, frames);
    QFETCH(int, seconds);
    QFETCH(int, minutes);
    QFETCH(int, hours);
    QFETCH(int, expectedStatus);

    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {rateNum, rateDen};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;
    const auto parsed = H26xTimingDetail::parseH264PicTiming(
        partialTimestampPayload(frames, seconds, minutes, hours), syntax);
    QCOMPARE(int(parsed.status), expectedStatus);
    QVERIFY(!parsed.timecode.valid);
}

void TestH26xSeiTimecode::h264RejectsInvalidPayloadAlignment() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Valid;
    syntax.frameRate = {25, 1};
    syntax.fixedFrameRate = true;
    syntax.picStructPresent = true;

    BitWriter missingMarker;
    missingMarker.bits(0, 4);
    writeFullTimestamp(missingMarker, 1, 2, 3, 4);
    missingMarker.payloadTrailingBits(false);
    auto parsed = H26xTimingDetail::parseH264PicTiming(missingMarker.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    BitWriter nonzeroPadding;
    nonzeroPadding.bits(0, 4);
    writeFullTimestamp(nonzeroPadding, 1, 2, 3, 4);
    nonzeroPadding.payloadTrailingBits(true, true);
    parsed = H26xTimingDetail::parseH264PicTiming(nonzeroPadding.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    H264TimingSyntax noPicStruct = syntax;
    noPicStruct.picStructPresent = false;
    parsed = H26xTimingDetail::parseH264PicTiming({}, noPicStruct);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::NoTimestamp);
    parsed = H26xTimingDetail::parseH264PicTiming(QByteArray::fromHex("00"), noPicStruct);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
    parsed = H26xTimingDetail::parseH264PicTiming(QByteArray::fromHex("80"), noPicStruct);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);

    H264TimingSyntax byteAligned = syntax;
    byteAligned.timeOffsetLength = 7;
    BitWriter exactByteAligned;
    exactByteAligned.bits(0, 4);
    writeFullTimestamp(exactByteAligned, 1, 2, 3, 4);
    exactByteAligned.bits(0, 7); // time_offset: syntax ends on the byte boundary
    QCOMPARE(exactByteAligned.bitPosition, 0);
    parsed = H26xTimingDetail::parseH264PicTiming(exactByteAligned.bytes, byteAligned);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(parsed.timecode.valid);

    H264TimingSyntax byteAlignedNoTimestamp = syntax;
    byteAlignedNoTimestamp.picStructPresent = false;
    byteAlignedNoTimestamp.cpbDpbDelaysPresent = true;
    byteAlignedNoTimestamp.cpbRemovalDelayLength = 4;
    byteAlignedNoTimestamp.dpbOutputDelayLength = 4;
    parsed =
        H26xTimingDetail::parseH264PicTiming(QByteArray::fromHex("00"), byteAlignedNoTimestamp);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::NoTimestamp);

    BitWriter reservedPicStruct;
    reservedPicStruct.bits(9, 4);
    reservedPicStruct.payloadTrailingBits();
    parsed = H26xTimingDetail::parseH264PicTiming(reservedPicStruct.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::h264RejectsMalformedEmulationPrevention() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray validMessage = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray invalidFollowingByte = QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) +
                                            QByteArray::fromHex("000300000304") + validMessage +
                                            QByteArray(1, char(0x80));
    QVERIFY(!extractH26xSeiTimecode(invalidFollowingByte, NativeVideoCodec::H264, context).valid);

    const QByteArray terminalEscape = QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) +
                                      validMessage + QByteArray::fromHex("000003");
    QVERIFY(!extractH26xSeiTimecode(terminalEscape, NativeVideoCodec::H264, context).valid);

    for (const QByteArray& forbidden :
         {QByteArray::fromHex("000000"), QByteArray::fromHex("000002")}) {
        const QByteArray rawNal = QByteArray(kStartCode4, 4) + QByteArray(1, char(0x06)) +
                                  forbidden + validMessage + QByteArray(1, char(0x80));
        QVERIFY(!extractH26xSeiTimecode(rawNal, NativeVideoCodec::H264, context).valid);
    }

    const QByteArray escapedPayload = seiMessage(5, QByteArray::fromHex("000002")) + validMessage;
    const auto escapedResult =
        extractH26xSeiTimecode(h264SeiNal(escapedPayload), NativeVideoCodec::H264, context);
    QVERIFY(escapedResult.valid);
    QCOMPARE(escapedResult.frames, 4);

    const QByteArray separatedNals = QByteArray(kStartCode4, 4) + QByteArray::fromHex("06050080") +
                                     QByteArray::fromHex("000001") + QByteArray(1, char(0x06)) +
                                     escapeRbsp(validMessage + QByteArray(1, char(0x80)));
    const auto splitResult = extractH26xSeiTimecode(separatedNals, NativeVideoCodec::H264, context);
    QVERIFY(splitResult.valid);
    QCOMPARE(splitResult.frames, 4);
}

void TestH26xSeiTimecode::h264RejectsInvalidSeiNalHeaders_data() {
    QTest::addColumn<int>("header");
    QTest::addColumn<int>("position");

    for (const int header : {0x86, 0x26}) {
        const QString headerName = QString::number(header, 16);
        QTest::newRow(qPrintable(QStringLiteral("0x%1 only").arg(headerName))) << header << 0;
        QTest::newRow(qPrintable(QStringLiteral("0x%1 before valid").arg(headerName)))
            << header << 1;
        QTest::newRow(qPrintable(QStringLiteral("0x%1 after valid").arg(headerName)))
            << header << 2;
    }
}

void TestH26xSeiTimecode::h264RejectsInvalidSeiNalHeaders() {
    QFETCH(int, header);
    QFETCH(int, position);

    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray message = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray invalid = seiNal(QByteArray(1, char(header)), message);
    const QByteArray valid = h264SeiNal(message);
    QByteArray annexB = invalid;
    if (position == 1) annexB += valid;
    if (position == 2) annexB = valid + invalid;

    QVERIFY(!extractH26xSeiTimecode(annexB, NativeVideoCodec::H264, context).valid);
}

void TestH26xSeiTimecode::h264AnnexBTrailingZeroBytesAreNotNalPayload() {
    const QByteArray reference = fixture("h264_pic_timing_no_hrd.264");
    H26xTimingContext context;
    QVERIFY(
        context.updateParameterSets(NativeVideoCodec::H264, {}, {h264SpsFromAnnexB(reference)}));

    const QByteArray validMessage = seiMessage(1, fullTimestampPayload(1, 2, 3, 4));
    const QByteArray sei = h264SeiNal(validMessage);
    for (const int zeroCount : {1, 2, 9}) {
        const QByteArray zeros(zeroCount, char(0));

        const auto finalNal = extractH26xSeiTimecode(sei + zeros, NativeVideoCodec::H264, context);
        QVERIFY2(finalNal.valid, qPrintable(QStringLiteral("final zeros=%1").arg(zeroCount)));
        QCOMPARE(finalNal.frames, 4);

        const auto beforeNext =
            extractH26xSeiTimecode(sei + zeros + h264VclNal(), NativeVideoCodec::H264, context);
        QVERIFY2(beforeNext.valid,
                 qPrintable(QStringLiteral("zeros before next NAL=%1").arg(zeroCount)));
        QCOMPARE(beforeNext.frames, 4);
    }

    // Zeros inside a declared payload precede rbsp_trailing_bits and are NAL data,
    // not Annex-B padding. They must survive splitting and EBSP decoding unchanged.
    const QByteArray leadingPayloadWithZeros = seiMessage(5, QByteArray::fromHex("120000"));
    const auto payloadZeros = extractH26xSeiTimecode(
        h264SeiNal(leadingPayloadWithZeros + validMessage), NativeVideoCodec::H264, context);
    QVERIFY(payloadZeros.valid);
    QCOMPARE(payloadZeros.frames, 4);
}

void TestH26xSeiTimecode::rawEbspTrailingZeroBytesRemainStrict() {
    QByteArray decoded;
    QVERIFY(H26xTimingDetail::unescapeRbsp(QByteArray::fromHex("128000"), decoded));
    QCOMPARE(decoded, QByteArray::fromHex("128000"));

    QVERIFY(H26xTimingDetail::unescapeRbsp(QByteArray::fromHex("12800000"), decoded));
    QCOMPARE(decoded, QByteArray::fromHex("12800000"));

    QVERIFY(!H26xTimingDetail::unescapeRbsp(QByteArray::fromHex("1280000000"), decoded));
    QVERIFY(decoded.isEmpty());
}

void TestH26xSeiTimecode::h264MalformedContextIsNotUnsupported() {
    H264TimingSyntax syntax;
    syntax.status = H26xTimingSyntaxStatus::Malformed;
    const auto parsed = H26xTimingDetail::parseH264PicTiming({}, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::h264PicTimingWithoutContextIsRejected() {
    H26xTimingContext context;
    const Smpte12mTimecode got = extractH26xSeiTimecode(fixture("h264_pic_timing_no_hrd.264"),
                                                        NativeVideoCodec::H264, context);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::h264X264FixtureDoesNotInventTimestamp() {
    const QByteArray bitstream = fixture("h264_x264_pic_timing.264");
    QVERIFY(!bitstream.isEmpty());
    const QByteArray sps = h264SpsFromAnnexB(bitstream);
    QVERIFY(!sps.isEmpty());

    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {sps}));
    QVERIFY(context.h264() != nullptr);
    QVERIFY(context.h264()->picStructPresent);
    QVERIFY(
        !extractH26xSeiTimecodeResult(bitstream, NativeVideoCodec::H264, context).timecode.valid);
}

void TestH26xSeiTimecode::h264JmFixtureDoesNotInventTimestamp() {
    const QByteArray bitstream = fixture("h264_jm_pic_timing.264");
    QVERIFY(!bitstream.isEmpty());
    const QByteArray sps = h264SpsFromAnnexB(bitstream);
    QVERIFY(!sps.isEmpty());

    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::H264, {}, {sps}));
    QVERIFY(context.h264() != nullptr);
    QVERIFY(context.h264()->picStructPresent);
    QVERIFY(
        !extractH26xSeiTimecodeResult(bitstream, NativeVideoCodec::H264, context).timecode.valid);
}

void TestH26xSeiTimecode::hevcSpsVuiTimingContextIsParsed() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {hevcSps()}));
    QVERIFY(context.hevc() != nullptr);
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Valid);
    QCOMPARE(context.hevc()->numUnitsInTick, uint32_t(1001));
    QCOMPARE(context.hevc()->timeScale, uint32_t(30000));
    QVERIFY(context.fixedFrameRate());
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{30000, 1001}));

    QVERIFY(
        !context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {hevcSps(1001, 60000)}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Unsupported);
}

void TestH26xSeiTimecode::hevcSpsTimingIsBoundToReferencedVps() {
    H26xTimingContext context;
    const QList<QByteArray> vps{hevcVps(1001, 30000, 0), hevcVps(1001, 60000, 1)};

    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, vps,
                                        {hevcSps(1001, 30000, 1, false, false)}));
    QCOMPARE(context.hevc()->numUnitsInTick, uint32_t(1001));
    QCOMPARE(context.hevc()->timeScale, uint32_t(60000));
    QCOMPARE(context.constantFrameRate(), (FrameRateQ{60000, 1001}));

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0)},
                                         {hevcSps(1001, 30000, 1, false, false)}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Unsupported);

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc,
                                         {hevcVps(1001, 30000, 1), hevcVps(1001, 60000, 1)},
                                         {hevcSps(1001, 30000, 1, false, false)}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Unsupported);
}

void TestH26xSeiTimecode::hevcVpsHrdLayerSetIndexIsValidated() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0, 0)}, {}));

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0, 1)}, {}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
}

void TestH26xSeiTimecode::hevcParameterSetsRejectEnhancementLayers() {
    H26xTimingContext context;
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcNalWithLayerId(hevcVps(), 1)},
                                         {hevcSps()}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                         {hevcNalWithLayerId(hevcSps(), 1)}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
}

void TestH26xSeiTimecode::hevcParameterSetsRequireTemporalIdZero() {
    H26xTimingContext context;
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc,
                                         {hevcNalWithTemporalIdPlus1(hevcVps(), 2)}, {hevcSps()}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);

    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                         {hevcNalWithTemporalIdPlus1(hevcSps(), 2)}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
}

void TestH26xSeiTimecode::hevcParameterSetConformanceFlagsAreValidated() {
    QByteArray vpsWithoutBaseLayerInternal = hevcVps();
    QCOMPARE(uchar(vpsWithoutBaseLayerInternal[2]), uchar(0x0c));
    vpsWithoutBaseLayerInternal[2] = char(0x04);

    QByteArray vpsWithoutTemporalNesting = hevcVps();
    QCOMPARE(uchar(vpsWithoutTemporalNesting[3]), uchar(0x01));
    vpsWithoutTemporalNesting[3] = char(0x00);

    QByteArray spsWithoutTemporalNesting = hevcSps();
    QCOMPARE(uchar(spsWithoutTemporalNesting[2]), uchar(0x01));
    spsWithoutTemporalNesting[2] = char(0x00);

    for (const QList<QByteArray>& vps : {QList<QByteArray>{vpsWithoutBaseLayerInternal},
                                         QList<QByteArray>{vpsWithoutTemporalNesting}}) {
        H26xTimingContext context;
        QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, vps, {hevcSps()}));
        QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
        QVERIFY(!context.hevc()->timingInfoPresent);
    }

    H26xTimingContext context;
    QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                         {spsWithoutTemporalNesting}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
    QVERIFY(!context.hevc()->timingInfoPresent);
}

void TestH26xSeiTimecode::hevcUnavailableBaseLayerIsUnsupported() {
    QByteArray unavailableBaseLayer = hevcVps();
    QCOMPARE(uchar(unavailableBaseLayer[2]), uchar(0x0c));
    unavailableBaseLayer[2] = char(0x08);

    H26xTimingContext context;
    QVERIFY(
        !context.updateParameterSets(NativeVideoCodec::Hevc, {unavailableBaseLayer}, {hevcSps()}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Unsupported);
    QVERIFY(!context.hevc()->timingInfoPresent);
}

void TestH26xSeiTimecode::hevcSpsStatusPrecedenceIsOrderIndependent() {
    QByteArray malformed = hevcSps();
    malformed.chop(1);
    const QByteArray missingVpsReference = hevcSps(1001, 30000, 1, false, false);

    for (const QList<QByteArray>& parameterSets :
         {QList<QByteArray>{missingVpsReference, malformed},
          QList<QByteArray>{malformed, missingVpsReference}}) {
        H26xTimingContext context;
        QVERIFY(!context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, parameterSets));
        QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
    }
}

void TestH26xSeiTimecode::hevcHmFixtureIsDecoded() {
    const QByteArray bitstream = fixture("hevc_hm_time_code.265");
    QVERIFY(!bitstream.isEmpty());
    const QList<QByteArray> vps = hevcNalsOfType(bitstream, 32);
    const QList<QByteArray> sps = hevcNalsOfType(bitstream, 33);
    QVERIFY(!vps.isEmpty());
    QVERIFY(!sps.isEmpty());
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, vps, sps));
    QVERIFY(context.hevc()->timingInfoPresent);
    QCOMPARE(context.hevc()->numUnitsInTick, uint32_t(900900));
    QCOMPARE(context.hevc()->timeScale, uint32_t(27000000));
    const auto payload =
        H26xTimingDetail::parseHevcTimeCode(QByteArray::fromHex("60e06985a8d3c0"), *context.hevc());
    QCOMPARE(payload.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    const H26xSeiTimecodeResult got =
        extractH26xSeiTimecodeResult(bitstream, NativeVideoCodec::Hevc, context);
    QVERIFY(got.timecode.valid);
    QCOMPARE(got.timecode.hours, 10);
    QCOMPARE(got.timecode.minutes, 11);
    QCOMPARE(got.timecode.seconds, 12);
    QCOMPARE(got.timecode.frames, 13);
    QCOMPARE(got.labelRate, (FrameRateQ{30000, 1001}));
    QVERIFY(got.discontinuity);
    QCOMPARE(got.provenance, TimecodeProvenance::HevcTimeCode);
}

void TestH26xSeiTimecode::hevcShmFixtureIsDecoded() {
    const QByteArray bitstream = fixture("hevc_shm_time_code.265");
    QVERIFY(!bitstream.isEmpty());
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, hevcNalsOfType(bitstream, 32),
                                        hevcNalsOfType(bitstream, 33)));
    const H26xSeiTimecodeResult got =
        extractH26xSeiTimecodeResult(bitstream, NativeVideoCodec::Hevc, context);
    QVERIFY(got.timecode.valid);
    QCOMPARE(got.timecode.hours, 1);
    QCOMPARE(got.timecode.minutes, 2);
    QCOMPARE(got.timecode.seconds, 3);
    QCOMPARE(got.timecode.frames, 4);
    QCOMPARE(got.labelRate, (FrameRateQ{30000, 1001}));
    QVERIFY(!got.discontinuity);
    QCOMPARE(got.provenance, TimecodeProvenance::HevcTimeCode);
}

void TestH26xSeiTimecode::hevcTimeCodeDecoded() {
    const QByteArray payload = hevcFullTimestampPayload(1, 2, 3, 4);
    const auto parsed = H26xTimingDetail::parseHevcTimeCode(payload, hevcTiming());
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QCOMPARE(parsed.labelRate, (FrameRateQ{30000, 1001}));
    QCOMPARE(parsed.provenance, TimecodeProvenance::HevcTimeCode);

    const QByteArray rbsp = seiMessage(/*time_code*/ 136, payload);
    const QByteArray annexB = hevcPrefixSeiNal(rbsp) + hevcVclNal();
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    const H26xSeiTimecodeResult got =
        extractH26xSeiTimecodeResult(annexB, NativeVideoCodec::Hevc, context);
    QVERIFY(got.timecode.valid);
    QCOMPARE(got.timecode.hours, 1);
    QCOMPARE(got.timecode.minutes, 2);
    QCOMPARE(got.timecode.seconds, 3);
    QCOMPARE(got.timecode.frames, 4);
    QCOMPARE(got.labelRate, (FrameRateQ{30000, 1001}));
    QCOMPARE(got.provenance, TimecodeProvenance::HevcTimeCode);
}

void TestH26xSeiTimecode::hevcClockCountAndFlagsAreValidated() {
    auto syntax = hevcTiming();
    BitWriter zero;
    zero.bits(0, 2);
    zero.payloadTrailingBits();
    QCOMPARE(H26xTimingDetail::parseHevcTimeCode(zero.bytes, syntax).status,
             H26xTimingDetail::TimecodeParseStatus::Malformed);

    BitWriter three;
    three.bits(3, 2);
    writeHevcFullTimestamp(three, 1, 2, 3, 4);
    three.bit(false);
    writeHevcFullTimestamp(three, 1, 2, 3, 5);
    if (three.bitPosition != 0) three.payloadTrailingBits();
    const auto parsed = H26xTimingDetail::parseHevcTimeCode(three.bytes, syntax);
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QCOMPARE(parsed.timecode.frames, 4);
}

void TestH26xSeiTimecode::hevcFrameFieldInfoIsInferredWithoutVui() {
    H26xTimingContext inferredContext;
    QVERIFY(inferredContext.updateParameterSets(
        NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0, -1, true, true)},
        {hevcSps(1001, 30000, 0, false, true, false, true, true, false)}));
    QVERIFY(inferredContext.hevc()->frameFieldInfoPresent);

    H26xTimingContext inferredAbsentContext;
    QVERIFY(inferredAbsentContext.updateParameterSets(
        NativeVideoCodec::Hevc, {hevcVps()},
        {hevcSps(1001, 30000, 0, false, true, false, true, false, false)}));
    QVERIFY(!inferredAbsentContext.hevc()->frameFieldInfoPresent);
}

void TestH26xSeiTimecode::hevcInferredFrameFieldInfoControlsClockCount() {
    H26xTimingContext inferredContext;
    QVERIFY(inferredContext.updateParameterSets(
        NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0, -1, true, true)},
        {hevcSps(1001, 30000, 0, false, true, false, true, true, false)}));

    const QByteArray twoClockTimeCode = seiMessage(136, hevcClockTimestampPayload(2, 1, 2, 3, 4));
    const QByteArray twoClockPictureTiming = seiMessage(1, hevcPictureTimingPayload(3));
    QVERIFY(extractH26xSeiTimecodeResult(hevcPrefixSeiNal(twoClockTimeCode + twoClockPictureTiming),
                                         NativeVideoCodec::Hevc, inferredContext)
                .timecode.valid);
    QVERIFY(!extractH26xSeiTimecodeResult(
                 hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4)) +
                                  twoClockPictureTiming),
                 NativeVideoCodec::Hevc, inferredContext)
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcFrameFieldInfoExplicitZeroIsRejected_data() {
    QTest::addColumn<bool>("fieldSeq");
    QTest::addColumn<bool>("progressiveSource");
    QTest::addColumn<bool>("interlacedSource");

    QTest::newRow("field sequence") << true << true << false;
    QTest::newRow("per-picture source scan") << false << true << true;
}

void TestH26xSeiTimecode::hevcFrameFieldInfoExplicitZeroIsRejected() {
    QFETCH(bool, fieldSeq);
    QFETCH(bool, progressiveSource);
    QFETCH(bool, interlacedSource);

    H26xTimingContext context;
    QVERIFY(!context.updateParameterSets(
        NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0, -1, progressiveSource, interlacedSource)},
        {hevcSps(1001, 30000, 0, false, true, fieldSeq, progressiveSource, interlacedSource)}));
    QCOMPARE(context.hevc()->status, H26xTimingSyntaxStatus::Malformed);
}

void TestH26xSeiTimecode::hevcFrameFieldInfoRequiresMatchingPictureTiming() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                        {hevcSps(1001, 30000, 0, true)}));
    H26xTimingContext fieldContext;
    QVERIFY(fieldContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                             {hevcSps(1001, 30000, 0, true, true, true)}));

    const QByteArray oneClock = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));
    QVERIFY(
        !extractH26xSeiTimecodeResult(hevcPrefixSeiNal(oneClock), NativeVideoCodec::Hevc, context)
             .timecode.valid);

    const QByteArray twoFieldPicture = seiMessage(1, hevcPictureTimingPayload(3));
    QVERIFY(!extractH26xSeiTimecodeResult(hevcPrefixSeiNal(oneClock + twoFieldPicture),
                                          NativeVideoCodec::Hevc, context)
                 .timecode.valid);

    const int expectedCounts[] = {1, 1, 1, 2, 2, 3, 3, 2, 3, 1, 1, 1, 1};
    for (int picStruct = 0; picStruct <= 12; ++picStruct) {
        const int expected = expectedCounts[picStruct];
        const bool fieldPicture = picStruct == 1 || picStruct == 2 || picStruct >= 9;
        const H26xTimingContext& activeContext = fieldPicture ? fieldContext : context;
        const QByteArray pictureTiming = seiMessage(1, hevcPictureTimingPayload(picStruct));
        const QByteArray matchingTimeCode =
            seiMessage(136, hevcClockTimestampPayload(expected, 1, 2, 3, 4));
        const auto matching =
            extractH26xSeiTimecodeResult(hevcPrefixSeiNal(matchingTimeCode + pictureTiming),
                                         NativeVideoCodec::Hevc, activeContext);
        if (picStruct == 7 || picStruct == 8) {
            QVERIFY(!matching.timecode.valid);
            continue;
        }
        QVERIFY2(
            matching.timecode.valid,
            qPrintable(
                QStringLiteral("pic_struct %1 expected %2 clocks").arg(picStruct).arg(expected)));

        const int wrong = expected == 3 ? 1 : expected + 1;
        const QByteArray mismatchedTimeCode =
            seiMessage(136, hevcClockTimestampPayload(wrong, 1, 2, 3, 4));
        QVERIFY2(!extractH26xSeiTimecodeResult(hevcPrefixSeiNal(mismatchedTimeCode + pictureTiming),
                                               NativeVideoCodec::Hevc, activeContext)
                      .timecode.valid,
                 qPrintable(
                     QStringLiteral("pic_struct %1 rejected %2 clocks").arg(picStruct).arg(wrong)));
    }
}

void TestH26xSeiTimecode::hevcPictureTimingSyntaxIsFullyValidated() {
    const QByteArray timeCode = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));

    H26xTimingContext frameContext;
    QVERIFY(frameContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                             {hevcSps(1001, 30000, 0, true)}));
    QVERIFY(!extractH26xSeiTimecodeResult(
                 hevcPrefixSeiNal(seiMessage(1, QByteArray::fromHex("0100")) + timeCode),
                 NativeVideoCodec::Hevc, frameContext)
                 .timecode.valid);

    H26xTimingContext fieldContext;
    QVERIFY(fieldContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()},
                                             {hevcSps(1001, 30000, 0, true, true, true)}));
    QVERIFY(!extractH26xSeiTimecodeResult(
                 hevcPrefixSeiNal(seiMessage(1, hevcPictureTimingPayload(0)) + timeCode),
                 NativeVideoCodec::Hevc, fieldContext)
                 .timecode.valid);
    QVERIFY(extractH26xSeiTimecodeResult(
                hevcPrefixSeiNal(seiMessage(1, hevcPictureTimingPayload(1)) + timeCode),
                NativeVideoCodec::Hevc, fieldContext)
                .timecode.valid);

    QVERIFY(!extractH26xSeiTimecodeResult(
                 hevcPrefixSeiNal(seiMessage(1, hevcPictureTimingPayload(1)) + timeCode),
                 NativeVideoCodec::Hevc, frameContext)
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcSourceScanMatchesProfileTierLevel_data() {
    QTest::addColumn<bool>("progressiveSource");
    QTest::addColumn<bool>("interlacedSource");
    QTest::addColumn<int>("sourceScanType");
    QTest::addColumn<bool>("accepted");

    QTest::newRow("progressive-progressive") << true << false << 1 << true;
    QTest::newRow("progressive-interlaced") << true << false << 0 << false;
    QTest::newRow("progressive-unspecified") << true << false << 2 << false;
    QTest::newRow("interlaced-interlaced") << false << true << 0 << true;
    QTest::newRow("interlaced-progressive") << false << true << 1 << false;
    QTest::newRow("interlaced-unspecified") << false << true << 2 << false;
    QTest::newRow("unspecified-unspecified") << false << false << 2 << true;
    QTest::newRow("unspecified-interlaced") << false << false << 0 << false;
    QTest::newRow("unspecified-progressive") << false << false << 1 << false;
    QTest::newRow("per-picture-interlaced") << true << true << 0 << true;
    QTest::newRow("per-picture-progressive") << true << true << 1 << true;
    QTest::newRow("per-picture-unspecified") << true << true << 2 << true;
}

void TestH26xSeiTimecode::hevcSourceScanMatchesProfileTierLevel() {
    QFETCH(bool, progressiveSource);
    QFETCH(bool, interlacedSource);
    QFETCH(int, sourceScanType);
    QFETCH(bool, accepted);

    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(
        NativeVideoCodec::Hevc, {hevcVps(1001, 30000, 0, -1, progressiveSource, interlacedSource)},
        {hevcSps(1001, 30000, 0, true, true, false, progressiveSource, interlacedSource)}));
    const QByteArray annexB =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4)) +
                         seiMessage(1, hevcPictureTimingPayload(0, sourceScanType)));
    QCOMPARE(extractH26xSeiTimecodeResult(annexB, NativeVideoCodec::Hevc, context).timecode.valid,
             accepted);
}

void TestH26xSeiTimecode::hevcPartialTimestampInheritsEarlierUnits() {
    BitWriter writer;
    writer.bits(2, 2);
    writeHevcFullTimestamp(writer, 7, 8, 9, 10);
    writer.bit(true);
    writer.bit(false); // units_field_based_flag
    writer.bits(0, 5); // counting_type
    writer.bit(false); // full_timestamp_flag
    writer.bit(false); // discontinuity_flag
    writer.bit(false); // cnt_dropped_flag
    writer.bits(11, 9);
    writer.bit(false); // seconds_flag: inherit the preceding clock's units
    writer.bits(0, 5); // time_offset_length
    if (writer.bitPosition != 0) writer.payloadTrailingBits();
    const auto parsed = H26xTimingDetail::parseHevcTimeCode(writer.bytes, hevcTiming());
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QCOMPARE(parsed.timecode.hours, 7);
    QCOMPARE(parsed.timecode.minutes, 8);
    QCOMPARE(parsed.timecode.seconds, 9);
    QCOMPARE(parsed.timecode.frames, 10);
}

void TestH26xSeiTimecode::hevcPartialTimestampInheritsAcrossAccessUnitsPerStream() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    H26xSeiTimecodeState firstStream;
    H26xSeiTimecodeState secondStream;
    const QByteArray complete =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(7, 8, 9, 10)));
    const QByteArray partial = hevcPrefixSeiNal(seiMessage(136, hevcNoUnitsTimestampPayload(11)));

    QVERIFY(extractH26xSeiTimecodeResult(complete, NativeVideoCodec::Hevc, context, firstStream,
                                         outputOrder(context, 100))
                .timecode.valid);
    const auto inherited = extractH26xSeiTimecodeResult(partial, NativeVideoCodec::Hevc, context,
                                                        firstStream, outputOrder(context, 200));
    QVERIFY(inherited.timecode.valid);
    QCOMPARE(inherited.timecode.hours, 7);
    QCOMPARE(inherited.timecode.minutes, 8);
    QCOMPARE(inherited.timecode.seconds, 9);
    QCOMPARE(inherited.timecode.frames, 11);

    const QByteArray conflicting =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4)) +
                         seiMessage(136, hevcFullTimestampPayload(4, 5, 6, 7)));
    QVERIFY(!extractH26xSeiTimecodeResult(conflicting, NativeVideoCodec::Hevc, context, firstStream,
                                          outputOrder(context, 300))
                 .timecode.valid);
    const auto afterConflict = extractH26xSeiTimecodeResult(
        partial, NativeVideoCodec::Hevc, context, firstStream, outputOrder(context, 400));
    QVERIFY(!afterConflict.timecode.valid);

    QVERIFY(!extractH26xSeiTimecodeResult(partial, NativeVideoCodec::Hevc, context, secondStream,
                                          outputOrder(context, 100))
                 .timecode.valid);
    firstStream.reset();
    QVERIFY(!extractH26xSeiTimecodeResult(partial, NativeVideoCodec::Hevc, context, firstStream,
                                          outputOrder(context, 100))
                 .timecode.valid);

    QVERIFY(extractH26xSeiTimecodeResult(complete, NativeVideoCodec::Hevc, context, firstStream,
                                         outputOrder(context, 200))
                .timecode.valid);
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps(1001, 60000)}, {}));
    QVERIFY(!extractH26xSeiTimecodeResult(partial, NativeVideoCodec::Hevc, context, firstStream,
                                          outputOrder(context, 300))
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcAbsentClockClearsPredecessor() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    H26xSeiTimecodeState state;
    const QByteArray complete =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(7, 8, 9, 10)));
    const QByteArray absentThenPartial =
        hevcPrefixSeiNal(seiMessage(136, hevcAbsentThenNoUnitsTimestampPayload(11)));

    QVERIFY(extractH26xSeiTimecodeResult(complete, NativeVideoCodec::Hevc, context, state,
                                         outputOrder(context, 100))
                .timecode.valid);
    QVERIFY(!extractH26xSeiTimecodeResult(absentThenPartial, NativeVideoCodec::Hevc, context, state,
                                          outputOrder(context, 200))
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcStateDoesNotCrossTimingContexts() {
    H26xTimingContext firstContext;
    H26xTimingContext secondContext;
    QVERIFY(firstContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    QVERIFY(secondContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    QCOMPARE(firstContext.generation(), secondContext.generation());

    H26xSeiTimecodeState state;
    const QByteArray complete =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(7, 8, 9, 10)));
    const QByteArray partial = hevcPrefixSeiNal(seiMessage(136, hevcNoUnitsTimestampPayload(11)));
    QVERIFY(extractH26xSeiTimecodeResult(complete, NativeVideoCodec::Hevc, firstContext, state,
                                         outputOrder(firstContext, 100))
                .timecode.valid);
    QVERIFY(!extractH26xSeiTimecodeResult(partial, NativeVideoCodec::Hevc, secondContext, state,
                                          outputOrder(secondContext, 200))
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcSignedOffsetAndDiscontinuityArePreserved() {
    const auto parsed = H26xTimingDetail::parseHevcTimeCode(
        hevcFullTimestampPayload(10, 11, 12, 13, 1, false, false, -7, 6, true), hevcTiming());
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(parsed.discontinuity);
}

void TestH26xSeiTimecode::hevcCountingAndDropSemanticsAreValidated() {
    QCOMPARE(H26xTimingDetail::parseHevcTimeCode(hevcFullTimestampPayload(1, 2, 3, 4, 0, true),
                                                 hevcTiming())
                 .status,
             H26xTimingDetail::TimecodeParseStatus::Malformed);

    const auto drop = H26xTimingDetail::parseHevcTimeCode(
        hevcFullTimestampPayload(1, 1, 0, 2, 4, true), hevcTiming());
    QCOMPARE(drop.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(drop.timecode.dropFrame);

    QCOMPARE(H26xTimingDetail::parseHevcTimeCode(hevcFullTimestampPayload(1, 1, 0, 3, 4, true),
                                                 hevcTiming())
                 .status,
             H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::hevcOutputOrderIdentityIsRequired() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    H26xSeiTimecodeState state;
    const QByteArray complete =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(7, 8, 9, 10)));
    const QByteArray partial = hevcPrefixSeiNal(seiMessage(136, hevcNoUnitsTimestampPayload(11)));

    QVERIFY(extractH26xSeiTimecodeResult(complete, NativeVideoCodec::Hevc, context, state)
                .timecode.valid);
    const auto missingOrder =
        extractH26xSeiTimecodeResult(partial, NativeVideoCodec::Hevc, context, state);
    QCOMPARE(missingOrder.status, H26xTimingDetail::TimecodeParseStatus::Unsupported);
    QVERIFY(!missingOrder.timecode.valid);
}

void TestH26xSeiTimecode::hevcOutputOrderHandlesReorderDuplicateAndEpoch() {
    using Status = H26xTimingDetail::TimecodeParseStatus;
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    H26xSeiTimecodeState state;
    const auto key = [&context](uint64_t sourceGeneration, uint64_t domain, uint64_t epoch,
                                int64_t presentationTime) {
        return H26xSeiOutputOrderKey{sourceGeneration, context.generation(), domain, epoch,
                                     presentationTime};
    };
    const auto au = [](int hours, int minutes, int seconds, int frames,
                       bool discontinuity = false) {
        return hevcPrefixSeiNal(
            seiMessage(136, hevcFullTimestampPayload(hours, minutes, seconds, frames, 0, false,
                                                     false, 0, 0, discontinuity)));
    };

    const QByteArray late = au(10, 0, 0, 10);
    QVERIFY(extractH26xSeiTimecodeResult(late, NativeVideoCodec::Hevc, context, state,
                                         key(1, 7, 0, 300))
                .timecode.valid);
    QVERIFY(extractH26xSeiTimecodeResult(au(10, 0, 0, 0), NativeVideoCodec::Hevc, context, state,
                                         key(1, 7, 0, 100))
                .timecode.valid);
    QVERIFY(extractH26xSeiTimecodeResult(au(10, 0, 0, 5), NativeVideoCodec::Hevc, context, state,
                                         key(1, 7, 0, 200))
                .timecode.valid);

    H26xSeiTimecodeState successorState;
    QVERIFY(extractH26xSeiTimecodeResult(late, NativeVideoCodec::Hevc, context, successorState,
                                         key(1, 7, 0, 300))
                .timecode.valid);
    QCOMPARE(extractH26xSeiTimecodeResult(au(11, 0, 0, 0), NativeVideoCodec::Hevc, context,
                                          successorState, key(1, 7, 0, 100))
                 .status,
             Status::Malformed);

    QVERIFY(extractH26xSeiTimecodeResult(late, NativeVideoCodec::Hevc, context, state,
                                         key(1, 7, 0, 300))
                .timecode.valid);
    QCOMPARE(extractH26xSeiTimecodeResult(au(10, 0, 0, 11), NativeVideoCodec::Hevc, context, state,
                                          key(1, 7, 0, 300))
                 .status,
             Status::Malformed);

    QCOMPARE(extractH26xSeiTimecodeResult(au(9, 0, 0, 0), NativeVideoCodec::Hevc, context, state,
                                          key(1, 7, 0, 400))
                 .status,
             Status::Malformed);
    QVERIFY(extractH26xSeiTimecodeResult(au(9, 0, 0, 0, true), NativeVideoCodec::Hevc, context,
                                         state, key(1, 7, 0, 400))
                .timecode.valid);

    // A higher caller-owned epoch makes a wrapped low presentation key a new order domain.
    QVERIFY(extractH26xSeiTimecodeResult(au(0, 0, 0, 0), NativeVideoCodec::Hevc, context, state,
                                         key(1, 7, 1, 3))
                .timecode.valid);
    QCOMPARE(extractH26xSeiTimecodeResult(au(23, 59, 59, 29), NativeVideoCodec::Hevc, context,
                                          state, key(1, 7, 0, (int64_t(1) << 33) - 1))
                 .status,
             Status::Unsupported);

    // Timing generation, source generation, and PTS domain are part of the identity contract.
    QCOMPARE(
        extractH26xSeiTimecodeResult(au(0, 0, 0, 1), NativeVideoCodec::Hevc, context, state,
                                     H26xSeiOutputOrderKey{1, context.generation() + 1, 7, 1, 4})
            .status,
        Status::Unsupported);
    QCOMPARE(extractH26xSeiTimecodeResult(au(0, 0, 0, 1), NativeVideoCodec::Hevc, context, state,
                                          key(1, 8, 1, 4))
                 .status,
             Status::Unsupported);
    QVERIFY(extractH26xSeiTimecodeResult(au(0, 0, 0, 1), NativeVideoCodec::Hevc, context, state,
                                         key(2, 8, 0, 4))
                .timecode.valid);
    QCOMPARE(extractH26xSeiTimecodeResult(au(0, 0, 0, 2), NativeVideoCodec::Hevc, context, state,
                                          key(1, 7, 1, 5))
                 .status,
             Status::Unsupported);
}

void TestH26xSeiTimecode::hevcCountingTypesUseNormativeOutputPredecessor_data() {
    using Status = H26xTimingDetail::TimecodeParseStatus;
    QTest::addColumn<int>("countingType");
    QTest::addColumn<int>("predecessorFrame");
    QTest::addColumn<bool>("discontinuity");
    QTest::addColumn<int>("expectedStatus");

    QTest::newRow("type2-prohibited-zero") << 2 << 0 << false << int(Status::Malformed);
    QTest::newRow("type2-sparse-predecessor") << 2 << 15 << false << int(Status::Unsupported);
    QTest::newRow("type2-discontinuity") << 2 << 0 << true << int(Status::Unsupported);
    QTest::newRow("type3-prohibited-max-minus-one") << 3 << 29 << false << int(Status::Malformed);
    QTest::newRow("type3-sparse-predecessor") << 3 << 15 << false << int(Status::Unsupported);
    QTest::newRow("type3-discontinuity") << 3 << 29 << true << int(Status::Unsupported);
    QTest::newRow("type4-prohibited-zero") << 4 << 0 << false << int(Status::Malformed);
    QTest::newRow("type4-prohibited-one") << 4 << 1 << false << int(Status::Malformed);
    QTest::newRow("type4-sparse-predecessor") << 4 << 15 << false << int(Status::Valid);
    QTest::newRow("type4-discontinuity") << 4 << 0 << true << int(Status::Valid);
}

void TestH26xSeiTimecode::hevcCountingTypesUseNormativeOutputPredecessor() {
    QFETCH(int, countingType);
    QFETCH(int, predecessorFrame);
    QFETCH(bool, discontinuity);
    QFETCH(int, expectedStatus);

    H26xTimingDetail::HevcTimeCodeContinuity previous;
    previous.haveSeconds = true;
    previous.haveMinutes = true;
    previous.haveHours = true;
    previous.seconds = 17;
    previous.minutes = 42;
    previous.hours = 6;
    previous.haveFrameSemantics = true;
    previous.frames = uint32_t(predecessorFrame);
    previous.labelRate = FrameRateQ{30000, 1001};
    previous.countingType = uint8_t(countingType);
    previous.dropFrame = countingType == 4;

    H26xTimingDetail::HevcTimeCodeOutput previousOutput;
    previousOutput.present = true;
    previousOutput.comparable = true;
    previousOutput.frames = uint32_t(predecessorFrame);
    previousOutput.maxFps = 30;

    const int currentFrame = countingType == 2 ? 1 : countingType == 3 ? 0 : 2;
    const auto parsed = H26xTimingDetail::parseHevcTimeCode(
        hevcFullTimestampPayload(6, 41, 0, currentFrame, countingType, true, false, 0, 0,
                                 discontinuity),
        hevcTiming(), &previous, nullptr, -1, &previousOutput);
    QCOMPARE(int(parsed.status), expectedStatus);
}

void TestH26xSeiTimecode::hevcDiscontinuityAllowsClockRegression() {
    const auto parsed = H26xTimingDetail::parseHevcTimeCode(
        hevcDecreasingDiscontinuousClockPayload(), hevcTiming());
    QCOMPARE(parsed.status, H26xTimingDetail::TimecodeParseStatus::Valid);
    QVERIFY(parsed.discontinuity);
    QCOMPARE(parsed.timecode.frames, 5);
}

void TestH26xSeiTimecode::hevcDropCountRequiresProvenPredecessor() {
    H26xTimingContext firstContext;
    H26xTimingContext secondContext;
    QVERIFY(firstContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    QVERIFY(secondContext.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    const QByteArray current =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(1, 1, 0, 2, 4, true)));

    for (const int predecessorFrame : {0, 1}) {
        H26xSeiTimecodeState state;
        const QByteArray predecessor = hevcPrefixSeiNal(
            seiMessage(136, hevcFullTimestampPayload(1, 0, 0, predecessorFrame, 4)));
        QVERIFY(extractH26xSeiTimecodeResult(predecessor, NativeVideoCodec::Hevc, firstContext,
                                             state, outputOrder(firstContext, 100))
                    .timecode.valid);
        QVERIFY(!extractH26xSeiTimecodeResult(current, NativeVideoCodec::Hevc, firstContext, state,
                                              outputOrder(firstContext, 200))
                     .timecode.valid);
    }

    H26xSeiTimecodeState state;
    const QByteArray validPredecessor =
        hevcPrefixSeiNal(seiMessage(136, hevcFullTimestampPayload(1, 0, 59, 29, 4)));
    QVERIFY(extractH26xSeiTimecodeResult(validPredecessor, NativeVideoCodec::Hevc, firstContext,
                                         state, outputOrder(firstContext, 100))
                .timecode.valid);
    const auto validDrop = extractH26xSeiTimecodeResult(
        current, NativeVideoCodec::Hevc, firstContext, state, outputOrder(firstContext, 200));
    QVERIFY(validDrop.timecode.valid);
    QVERIFY(validDrop.timecode.dropFrame);

    state.reset();
    QVERIFY(extractH26xSeiTimecodeResult(current, NativeVideoCodec::Hevc, firstContext, state,
                                         outputOrder(firstContext, 100))
                .timecode.valid);

    state.reset();
    QVERIFY(extractH26xSeiTimecodeResult(validPredecessor, NativeVideoCodec::Hevc, firstContext,
                                         state, outputOrder(firstContext, 200))
                .timecode.valid);
    QVERIFY(extractH26xSeiTimecodeResult(current, NativeVideoCodec::Hevc, secondContext, state,
                                         outputOrder(secondContext, 300))
                .timecode.valid);
}

void TestH26xSeiTimecode::hevcTruncatedTimeCodeIsRejected() {
    QByteArray payload = hevcFullTimestampPayload(1, 2, 3, 4);
    payload.chop(1);
    QCOMPARE(H26xTimingDetail::parseHevcTimeCode(payload, hevcTiming()).status,
             H26xTimingDetail::TimecodeParseStatus::Malformed);
}

void TestH26xSeiTimecode::hevcSuffixTimeCodeIsIgnored() {
    const QByteArray rbsp = seiMessage(136, hevcFullTimestampPayload(23, 59, 58, 24));
    const QByteArray annexB = hevcVclNal() + hevcSuffixSeiNal(rbsp);
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    QVERIFY(!extractH26xSeiTimecode(annexB, NativeVideoCodec::Hevc, context).valid);
}

void TestH26xSeiTimecode::hevcDuplicateTimeCodeMessagesMustAgree() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    const QByteArray first = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));
    const QByteArray equivalent = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));
    const QByteArray conflicting = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 5));

    const auto coalesced = extractH26xSeiTimecodeResult(hevcPrefixSeiNal(first + equivalent),
                                                        NativeVideoCodec::Hevc, context);
    QVERIFY(coalesced.timecode.valid);
    QCOMPARE(coalesced.timecode.frames, 4);

    QVERIFY(!extractH26xSeiTimecodeResult(hevcPrefixSeiNal(first + conflicting),
                                          NativeVideoCodec::Hevc, context)
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcDuplicateTimeCodeMessagesCannotChainContinuity() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    H26xSeiTimecodeState state;
    const QByteArray full = seiMessage(136, hevcFullTimestampPayload(7, 8, 9, 11));
    const QByteArray partial = seiMessage(136, hevcNoUnitsTimestampPayload(11));

    QVERIFY(!extractH26xSeiTimecodeResult(hevcPrefixSeiNal(full + partial), NativeVideoCodec::Hevc,
                                          context, state)
                 .timecode.valid);

    state.reset();
    QVERIFY(!extractH26xSeiTimecodeResult(hevcPrefixSeiNal(full) + hevcPrefixSeiNal(partial),
                                          NativeVideoCodec::Hevc, context, state)
                 .timecode.valid);
}

void TestH26xSeiTimecode::hevcEnhancementLayerSeiIsRejected() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    const QByteArray timeCode = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));

    QVERIFY(!extractH26xSeiTimecodeResult(hevcPrefixSeiNal(timeCode, 1), NativeVideoCodec::Hevc,
                                          context)
                 .timecode.valid);
}

void TestH26xSeiTimecode::registeredT35UnknownAndCaptionPayloadsAreIgnored() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));

    const QList<QByteArray> payloads{
        QByteArray::fromHex("b500314741393403c0ff"),         // ATSC GA94 type 3: cc_data, not ATC
        QByteArray::fromHex("b5003147413934037104aabbccdd"), // CDP-like bytes in cc_data
        QByteArray::fromHex("ff0100904f4c525401020304"),     // extended country, unknown user id
        QByteArray::fromHex("b500904154432001020304")        // unknown provider-owned user id
    };
    for (const QByteArray& payload : payloads) {
        const QByteArray annexB = hevcPrefixSeiNal(seiMessage(4, payload));
        QVERIFY(!extractH26xSeiTimecode(annexB, NativeVideoCodec::Hevc, context).valid);
    }

    const QList<QByteArray> malformed{QByteArray::fromHex("ff"), QByteArray::fromHex("ff01"),
                                      QByteArray::fromHex("b5"), QByteArray::fromHex("b500")};
    for (const QByteArray& payload : malformed) {
        const QByteArray annexB = hevcPrefixSeiNal(seiMessage(4, payload));
        QVERIFY(!extractH26xSeiTimecode(annexB, NativeVideoCodec::Hevc, context).valid);
    }
}

void TestH26xSeiTimecode::registeredT35UnknownProviderDoesNotSuppressTimeCode() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    const QByteArray timeCode = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));

    const auto parsed = extractH26xSeiTimecodeResult(
        hevcPrefixSeiNal(seiMessage(4, QByteArray::fromHex("b51234")) + timeCode),
        NativeVideoCodec::Hevc, context);
    QVERIFY(parsed.timecode.valid);
    QCOMPARE(parsed.timecode.frames, 4);

    QVERIFY(!extractH26xSeiTimecodeResult(
                 hevcPrefixSeiNal(seiMessage(4, QByteArray::fromHex("b512")) + timeCode),
                 NativeVideoCodec::Hevc, context)
                 .timecode.valid);
}

void TestH26xSeiTimecode::registeredT35CountryOnlyPayloadPoisonsAccessUnit() {
    H26xTimingContext context;
    QVERIFY(context.updateParameterSets(NativeVideoCodec::Hevc, {hevcVps()}, {}));
    const QByteArray timeCode = seiMessage(136, hevcFullTimestampPayload(1, 2, 3, 4));

    QVERIFY(!extractH26xSeiTimecodeResult(
                 hevcPrefixSeiNal(seiMessage(4, QByteArray::fromHex("01")) + timeCode),
                 NativeVideoCodec::Hevc, context)
                 .timecode.valid);

    const auto completeUnknown = extractH26xSeiTimecodeResult(
        hevcPrefixSeiNal(seiMessage(4, QByteArray::fromHex("0100")) + timeCode),
        NativeVideoCodec::Hevc, context);
    QVERIFY(completeUnknown.timecode.valid);
    QCOMPARE(completeUnknown.timecode.frames, 4);
}

void TestH26xSeiTimecode::noSeiReturnsInvalid() {
    const QByteArray annexB = h264VclNal(); // VCL only, no SEI
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::truncatedSeiPayloadReturnsInvalid() {
    // payloadType=1, payloadSize=4, but only 2 payload bytes present.
    const QByteArray rbsp = seiVarByte(1) + seiVarByte(4) + QByteArray::fromHex("0a0b");
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid); // must not read OOB
}

void TestH26xSeiTimecode::truncatedPayloadSizeReturnsInvalid() {
    // payloadType present, but the buffer ends mid 0xFF continuation run with no
    // terminating size byte.
    const QByteArray rbsp = seiVarByte(1) + QByteArray::fromHex("ffff");
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::emptyBufferReturnsInvalid() {
    const Smpte12mTimecode got = extractH26xSeiTimecode(QByteArray(), NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::seiWithoutTimecodePayloadTypeReturnsInvalid() {
    // A SEI message with an unrelated payloadType (e.g. 5 = user_data_unregistered
    // with no recognised TC) yields no timecode.
    const QByteArray rbsp = seiMessage(/*buffering_period*/ 0, QByteArray::fromHex("01020304"));
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::unknownCodecReturnsInvalid() {
    const QByteArray rbsp =
        seiMessage(1, packedWordBytes(Smpte12mTimecode{1, 0, 0, 0, false, true}));
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::Unknown);
    QVERIFY(!got.valid);
}

void TestH26xSeiTimecode::hugePayloadSizeVarintDoesNotOverflowOrCrash() {
    // Regression: a non-timecode payloadType (0) followed by a multi-megabyte run
    // of 0xFF size-continuation bytes. A 32-bit accumulator would overflow, the
    // `pos + payloadSize` bound would wrap, `pos` would go negative and the next
    // iteration would index out of bounds (SEGV). Must return invalid, no crash,
    // no OOB (runs clean under ASan/UBSan in the sanitizer CI job).
    QByteArray rbsp;
    rbsp.append(char(0));                           // payloadType = 0 (skips the TC decode branch)
    rbsp.append(QByteArray(8'400'000, char(0xFF))); // enormous size varint run
    rbsp.append(char(0x10));                        // terminating size byte
    rbsp.append(QByteArray::fromHex("0a0b0c0d"));   // a little trailing data
    const QByteArray annexB = h264SeiNal(rbsp) + h264VclNal();
    const Smpte12mTimecode got = extractH26xSeiTimecode(annexB, NativeVideoCodec::H264);
    QVERIFY(!got.valid);
}

QTEST_GUILESS_MAIN(TestH26xSeiTimecode)
#include "tst_h26xseitimecode.moc"
