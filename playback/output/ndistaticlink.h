#ifndef NDISTATICLINK_H
#define NDISTATICLINK_H

#include "playback/output/ndiabi.h"

#if defined(OLR_NDI_STATIC_LINK)
extern "C" {
bool NDIlib_initialize(void);
void NDIlib_destroy(void);
olr::ndi::NDIlib_send_instance_t NDIlib_send_create(const olr::ndi::NDIlib_send_create_t*);
void NDIlib_send_destroy(olr::ndi::NDIlib_send_instance_t);
void NDIlib_send_send_video_v2(olr::ndi::NDIlib_send_instance_t,
                               const olr::ndi::NDIlib_video_frame_v2_t*);
void NDIlib_send_send_audio_v3(olr::ndi::NDIlib_send_instance_t,
                               const olr::ndi::NDIlib_audio_frame_v3_t*);
}
#endif

#endif // NDISTATICLINK_H
