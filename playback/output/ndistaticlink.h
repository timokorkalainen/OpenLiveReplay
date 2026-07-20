#ifndef NDISTATICLINK_H
#define NDISTATICLINK_H

#include "playback/output/ndiabi.h"

#if defined(OLR_NDI_STATIC_LINK)
extern "C" {
bool NDIlib_initialize(void);
void NDIlib_destroy(void);

olr::ndi::NDIlib_find_instance_t NDIlib_find_create_v2(const olr::ndi::NDIlib_find_create_t*);
void NDIlib_find_destroy(olr::ndi::NDIlib_find_instance_t);
bool NDIlib_find_wait_for_sources(olr::ndi::NDIlib_find_instance_t, quint32);
const olr::ndi::NDIlib_source_t* NDIlib_find_get_current_sources(olr::ndi::NDIlib_find_instance_t,
                                                                 quint32*);

olr::ndi::NDIlib_recv_instance_t NDIlib_recv_create_v3(const olr::ndi::NDIlib_recv_create_v3_t*);
void NDIlib_recv_destroy(olr::ndi::NDIlib_recv_instance_t);
int NDIlib_recv_capture_v3(olr::ndi::NDIlib_recv_instance_t, olr::ndi::NDIlib_video_frame_v2_t*,
                           olr::ndi::NDIlib_audio_frame_v3_t*, void*, quint32);
void NDIlib_recv_free_video_v2(olr::ndi::NDIlib_recv_instance_t,
                               const olr::ndi::NDIlib_video_frame_v2_t*);
void NDIlib_recv_free_audio_v3(olr::ndi::NDIlib_recv_instance_t,
                               const olr::ndi::NDIlib_audio_frame_v3_t*);

olr::ndi::NDIlib_send_instance_t NDIlib_send_create(const olr::ndi::NDIlib_send_create_t*);
void NDIlib_send_destroy(olr::ndi::NDIlib_send_instance_t);
void NDIlib_send_send_video_v2(olr::ndi::NDIlib_send_instance_t,
                               const olr::ndi::NDIlib_video_frame_v2_t*);
void NDIlib_send_send_audio_v3(olr::ndi::NDIlib_send_instance_t,
                               const olr::ndi::NDIlib_audio_frame_v3_t*);
}
#endif

#endif // NDISTATICLINK_H
