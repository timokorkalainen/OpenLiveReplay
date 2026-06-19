#ifndef OLR_SYNTHETICFRAMES_H
#define OLR_SYNTHETICFRAMES_H

extern "C" {
struct AVFrame;
}

// Deterministic YUV420P frame: a diagonal gradient + a block that shifts with
// seq, giving real spatial detail and inter-frame motion so encode cost is
// representative. No RNG — same seq always produces identical pixel bytes.
// Caller owns the returned frame (av_frame_free).
AVFrame* makeSyntheticFrame(int width, int height, int seq);

#endif // OLR_SYNTHETICFRAMES_H
