#ifndef NDIABI_H
#define NDIABI_H

#include <QtGlobal>

#include <limits>

namespace olr::ndi {

constexpr quint32 fourCc(char a, char b, char c, char d) {
    return quint32(quint8(a)) | (quint32(quint8(b)) << 8) | (quint32(quint8(c)) << 16) |
           (quint32(quint8(d)) << 24);
}

constexpr quint32 kFourCcI420 = fourCc('I', '4', '2', '0');
constexpr quint32 kFourCcFltp = fourCc('F', 'L', 'T', 'p');
constexpr int kFrameFormatProgressive = 1;
constexpr qint64 kTimecodeSynthesize = std::numeric_limits<qint64>::max();

enum FrameType {
    FrameTypeNone = 0,
    FrameTypeVideo = 1,
    FrameTypeAudio = 2,
    FrameTypeMetadata = 3,
    FrameTypeError = 4,
    FrameTypeStatusChange = 100,
};

struct NDIlib_source_t {
    const char* p_ndi_name = nullptr;
    const char* p_url_address = nullptr;
};

struct NDIlib_find_create_t {
    bool show_local_sources = true;
    const char* p_groups = nullptr;
    const char* p_extra_ips = nullptr;
};

struct NDIlib_recv_create_v3_t {
    NDIlib_source_t source_to_connect_to;
    int color_format = 3;
    int bandwidth = 100;
    bool allow_video_fields = false;
    const char* p_ndi_recv_name = nullptr;
};

struct NDIlib_send_create_t {
    const char* p_ndi_name = nullptr;
    const char* p_groups = nullptr;
    bool clock_video = false;
    bool clock_audio = false;
};

struct NDIlib_video_frame_v2_t {
    int xres = 0;
    int yres = 0;
    quint32 FourCC = kFourCcI420;
    int frame_rate_N = 0;
    int frame_rate_D = 1;
    float picture_aspect_ratio = 0.0f;
    int frame_format_type = kFrameFormatProgressive;
    qint64 timecode = kTimecodeSynthesize;
    quint8* p_data = nullptr;
    int line_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    qint64 timestamp = 0;
};

struct NDIlib_audio_frame_v3_t {
    int sample_rate = 48000;
    int no_channels = 2;
    int no_samples = 0;
    qint64 timecode = kTimecodeSynthesize;
    quint32 FourCC = kFourCcFltp;
    quint8* p_data = nullptr;
    int channel_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    qint64 timestamp = 0;
};

using NDIlib_find_instance_t = void*;
using NDIlib_recv_instance_t = void*;
using NDIlib_send_instance_t = void*;

using NDIlib_initialize_fn = bool (*)();
using NDIlib_destroy_fn = void (*)();
using NDIlib_find_create_v2_fn = NDIlib_find_instance_t (*)(const NDIlib_find_create_t*);
using NDIlib_find_destroy_fn = void (*)(NDIlib_find_instance_t);
using NDIlib_find_wait_for_sources_fn = bool (*)(NDIlib_find_instance_t, quint32);
using NDIlib_find_get_current_sources_fn = const NDIlib_source_t* (*) (NDIlib_find_instance_t,
                                                                       quint32*);
using NDIlib_recv_create_v3_fn = NDIlib_recv_instance_t (*)(const NDIlib_recv_create_v3_t*);
using NDIlib_recv_destroy_fn = void (*)(NDIlib_recv_instance_t);
using NDIlib_recv_capture_v3_fn = int (*)(NDIlib_recv_instance_t, NDIlib_video_frame_v2_t*,
                                          NDIlib_audio_frame_v3_t*, void*, quint32);
using NDIlib_recv_free_video_v2_fn = void (*)(NDIlib_recv_instance_t,
                                              const NDIlib_video_frame_v2_t*);
using NDIlib_recv_free_audio_v3_fn = void (*)(NDIlib_recv_instance_t,
                                              const NDIlib_audio_frame_v3_t*);
using NDIlib_send_create_fn = NDIlib_send_instance_t (*)(const NDIlib_send_create_t*);
using NDIlib_send_destroy_fn = void (*)(NDIlib_send_instance_t);
using NDIlib_send_send_video_v2_fn = void (*)(NDIlib_send_instance_t,
                                              const NDIlib_video_frame_v2_t*);
using NDIlib_send_send_audio_v3_fn = void (*)(NDIlib_send_instance_t,
                                              const NDIlib_audio_frame_v3_t*);

} // namespace olr::ndi

#endif // NDIABI_H
