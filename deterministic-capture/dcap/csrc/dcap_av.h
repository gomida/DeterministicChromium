/* dcap_av: a small, stable C surface over libavcodec/libavformat/libswscale
 * for the dcap controller (DESIGN.md 13.3). The Rust side declares exactly
 * these functions; ffmpeg structs never cross the boundary.
 *
 * Every function that can fail returns 0 on success or a negative value and
 * writes a NUL-terminated message into err (errlen bytes). */
#ifndef DCAP_AV_H
#define DCAP_AV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dcap_enc dcap_enc;

typedef struct dcap_enc_params {
  const char* codec;        /* ffv1, utvideo, magicyuv, ffvhuff, rawvideo, h264_nvenc, hevc_nvenc */
  const char* pix_fmt;      /* encoder input: bgr0, bgra, gbrp, gbrap, yuv444p, yuv420p, nv12 */
  int width;
  int height;
  int fps_num;
  int fps_den;
  int threads;              /* 0 = library default */
  const char* const* options; /* key, value, key, value, ... */
  int n_options;            /* number of pairs */
  const char* container;    /* mp4, mov, matroska, rawvideo */
  const char* path;
  /* Audio stream; audio_codec NULL disables it. */
  const char* audio_codec;  /* pcm_s16le, pcm_f32le, flac */
  int audio_rate;
  int audio_channels;
  int audio_sample_format;  /* 1 = s16 interleaved, 2 = f32 interleaved */
} dcap_enc_params;

typedef struct dcap_enc_stats {
  int64_t video_frames_in;
  int64_t video_packets;
  int64_t audio_sample_frames;
  int64_t audio_packets;
  int64_t bytes_written;
  int extradata_len;
  uint8_t extradata[4096];
  double encode_ms_total;   /* wall time inside send/receive */
  double convert_ms_total;  /* wall time inside swscale */
} dcap_enc_stats;

/* Opens the encoder and the output container and writes the header. */
dcap_enc* dcap_enc_open(const dcap_enc_params* params, char* err, size_t errlen);

/* Encodes one canonical BGRA (premultiplied, packed) frame with pts = frame
 * index relative to the chunk start, in units of fps_den/fps_num. */
int dcap_enc_video(dcap_enc* enc, const uint8_t* bgra, int stride, int64_t pts,
                   char* err, size_t errlen);

/* Encodes interleaved PCM with pts in sample frames relative to the chunk start. */
int dcap_enc_audio(dcap_enc* enc, const uint8_t* pcm, int sample_frames, int64_t pts,
                   char* err, size_t errlen);

/* Flushes the encoders, writes the trailer, closes the file and frees the
 * handle (even on failure). */
int dcap_enc_finish(dcap_enc* enc, dcap_enc_stats* stats, char* err, size_t errlen);

/* Frees without a trailer (the output is unusable). */
void dcap_enc_abort(dcap_enc* enc);

/* Opens the encoder with the given parameters and closes it again: reports
 * whether the codec and pixel format are usable here (NVENC loads its
 * libraries and creates a session). */
int dcap_codec_probe(const dcap_enc_params* params, char* err, size_t errlen);

typedef struct dcap_probe_info {
  char codec[32];
  char pix_fmt[32];
  int width;
  int height;
  int64_t video_packets;
  int video_first_keyframe;
  int video_all_keyframes;
  int64_t audio_sample_frames;
  int has_audio;
  int extradata_len;
  uint8_t extradata[4096];
  int time_base_num;
  int time_base_den;
  int64_t first_pts_delta;   /* pts[1] - pts[0] of the video stream, in time_base units */
} dcap_probe_info;

/* Demuxes a produced chunk and counts packets (no decoding). */
int dcap_probe(const char* path, dcap_probe_info* info, char* err, size_t errlen);

typedef struct dcap_join_input {
  const char* path;
  int64_t first_frame;       /* absolute index of the chunk's first frame */
  int64_t frames;            /* expected video packets */
  int64_t audio_sample_frames; /* expected audio sample frames (0 when no audio) */
} dcap_join_input;

typedef struct dcap_join_stats {
  int64_t video_packets;
  int64_t audio_sample_frames;
  int64_t bytes_written;
} dcap_join_stats;

/* Stream-copies the chunks into one file. Video pts are computed from the
 * frame indices (first_frame - start_frame + packet ordinal) and audio pts
 * from the sample counts; input timestamps are never read. Every input must
 * match the first one in codec, dimensions, pixel format and extradata, hold
 * exactly `frames` video packets starting with a keyframe and exactly
 * `audio_sample_frames` audio samples. */
int dcap_join(const dcap_join_input* inputs, int n_inputs, int64_t start_frame,
              const char* out_path, const char* container, int fps_num, int fps_den,
              int audio_rate, dcap_join_stats* stats, char* err, size_t errlen);

/* Library identification for reports: "lavc 62.x.x lavf ..." */
void dcap_av_versions(char* buf, size_t len);

/* Names of the encoders compiled in, comma-separated. */
void dcap_av_encoders(char* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DCAP_AV_H */
