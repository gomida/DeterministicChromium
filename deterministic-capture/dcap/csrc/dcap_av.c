/* dcap_av: libav shim for dcap. See dcap_av.h. */
#include "dcap_av.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>

struct dcap_enc {
  AVFormatContext* fmt;
  AVCodecContext* vctx;
  AVStream* vstream;
  AVFrame* vframe;       /* frame in the encoder's pixel format */
  struct SwsContext* sws;
  int sws_needed;
  AVCodecContext* actx;
  AVStream* astream;
  AVFrame* aframe;
  int audio_sample_format;
  int audio_channels;
  AVPacket* pkt;
  int header_written;
  int width, height;
  dcap_enc_stats stats;
};

static void set_err(char* err, size_t errlen, const char* fmt, ...) {
  if (!err || errlen == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, errlen, fmt, ap);
  va_end(ap);
}

static void set_averr(char* err, size_t errlen, const char* what, int code) {
  char buf[128];
  av_strerror(code, buf, sizeof(buf));
  set_err(err, errlen, "%s: %s (%d)", what, buf, code);
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int is_rgb_like(enum AVPixelFormat f) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
  return d && (d->flags & AV_PIX_FMT_FLAG_RGB);
}

/* Writes all pending packets of one encoder; returns 0 or an AVERROR. */
static int drain(dcap_enc* e, AVCodecContext* ctx, AVStream* st, int64_t* packets) {
  for (;;) {
    int rc = avcodec_receive_packet(ctx, e->pkt);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return 0;
    if (rc < 0) return rc;
    e->pkt->stream_index = st->index;
    av_packet_rescale_ts(e->pkt, ctx->time_base, st->time_base);
    e->stats.bytes_written += e->pkt->size;
    rc = av_interleaved_write_frame(e->fmt, e->pkt);
    if (rc < 0) return rc;
    (*packets)++;
  }
}

static int open_video(dcap_enc* e, const dcap_enc_params* p, char* err, size_t errlen) {
  const AVCodec* codec = avcodec_find_encoder_by_name(p->codec);
  if (!codec) {
    set_err(err, errlen, "EncoderUnavailable: no encoder named %s in this build", p->codec);
    return -1;
  }
  enum AVPixelFormat pix = av_get_pix_fmt(p->pix_fmt);
  if (pix == AV_PIX_FMT_NONE) {
    set_err(err, errlen, "InvalidParameters: unknown pixel format %s", p->pix_fmt);
    return -1;
  }
  e->vctx = avcodec_alloc_context3(codec);
  if (!e->vctx) {
    set_err(err, errlen, "EncoderFailed: avcodec_alloc_context3");
    return -1;
  }
  e->vctx->width = p->width;
  e->vctx->height = p->height;
  e->vctx->pix_fmt = pix;
  e->vctx->time_base = (AVRational){p->fps_den, p->fps_num};
  e->vctx->framerate = (AVRational){p->fps_num, p->fps_den};
  e->vctx->thread_count = p->threads;
  if (is_rgb_like(pix)) {
    e->vctx->color_range = AVCOL_RANGE_JPEG;
    e->vctx->colorspace = AVCOL_SPC_RGB;
  } else {
    e->vctx->color_range = AVCOL_RANGE_MPEG;
    e->vctx->colorspace = AVCOL_SPC_BT709;
  }
  e->vctx->color_primaries = AVCOL_PRI_BT709;
  e->vctx->color_trc = AVCOL_TRC_IEC61966_2_1; /* sRGB */
  if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) {
    e->vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  for (int i = 0; i < p->n_options; i++) {
    const char* k = p->options[2 * i];
    const char* v = p->options[2 * i + 1];
    int rc = av_opt_set(e->vctx, k, v, AV_OPT_SEARCH_CHILDREN);
    if (rc < 0) {
      set_averr(err, errlen, "InvalidParameters: encoder option", rc);
      return -1;
    }
  }
  int rc = avcodec_open2(e->vctx, codec, NULL);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderUnavailable: avcodec_open2", rc);
    return -1;
  }
  e->vstream = avformat_new_stream(e->fmt, NULL);
  if (!e->vstream) {
    set_err(err, errlen, "EncoderFailed: avformat_new_stream");
    return -1;
  }
  e->vstream->time_base = e->vctx->time_base;
  e->vstream->avg_frame_rate = e->vctx->framerate;
  rc = avcodec_parameters_from_context(e->vstream->codecpar, e->vctx);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderFailed: codecpar", rc);
    return -1;
  }
  e->vframe = av_frame_alloc();
  e->vframe->format = pix;
  e->vframe->width = p->width;
  e->vframe->height = p->height;
  e->sws_needed = !(pix == AV_PIX_FMT_BGRA || pix == AV_PIX_FMT_BGR0);
  if (e->sws_needed) {
    rc = av_frame_get_buffer(e->vframe, 64);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderFailed: frame buffer", rc);
      return -1;
    }
    e->sws = sws_getContext(p->width, p->height, AV_PIX_FMT_BGRA, p->width, p->height, pix,
                            SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
    if (!e->sws) {
      set_err(err, errlen, "InvalidParameters: no conversion from bgra to %s", p->pix_fmt);
      return -1;
    }
    if (!is_rgb_like(pix)) {
      /* sRGB full-range RGB to BT.709 limited-range YUV. */
      int* inv = NULL; int* table = NULL; int srcR, dstR, b, c, s;
      if (sws_getColorspaceDetails(e->sws, &inv, &srcR, &table, &dstR, &b, &c, &s) >= 0) {
        sws_setColorspaceDetails(e->sws, sws_getCoefficients(SWS_CS_ITU709), 1,
                                 sws_getCoefficients(SWS_CS_ITU709), 0, b, c, s);
      }
    }
  }
  return 0;
}

static int open_audio(dcap_enc* e, const dcap_enc_params* p, char* err, size_t errlen) {
  const AVCodec* codec = avcodec_find_encoder_by_name(p->audio_codec);
  if (!codec) {
    set_err(err, errlen, "EncoderUnavailable: no audio encoder named %s", p->audio_codec);
    return -1;
  }
  e->actx = avcodec_alloc_context3(codec);
  e->actx->sample_rate = p->audio_rate;
  av_channel_layout_default(&e->actx->ch_layout, p->audio_channels);
  e->actx->sample_fmt = p->audio_sample_format == 2 ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
  e->actx->time_base = (AVRational){1, p->audio_rate};
  if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER) {
    e->actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  /* FLAC and other compressed codecs want planar or specific formats; check. */
  const enum AVSampleFormat* fmts = NULL;
  int nfmts = 0;
  if (avcodec_get_supported_config(e->actx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                   (const void**)&fmts, &nfmts) >= 0 && fmts) {
    int ok = 0;
    for (int i = 0; i < nfmts; i++) ok |= fmts[i] == e->actx->sample_fmt;
    if (!ok) {
      set_err(err, errlen, "InvalidParameters: %s does not accept sample format %s", p->audio_codec,
              av_get_sample_fmt_name(e->actx->sample_fmt));
      return -1;
    }
  }
  int rc = avcodec_open2(e->actx, codec, NULL);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderUnavailable: audio avcodec_open2", rc);
    return -1;
  }
  e->astream = avformat_new_stream(e->fmt, NULL);
  e->astream->time_base = e->actx->time_base;
  rc = avcodec_parameters_from_context(e->astream->codecpar, e->actx);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderFailed: audio codecpar", rc);
    return -1;
  }
  e->aframe = av_frame_alloc();
  e->audio_sample_format = p->audio_sample_format;
  e->audio_channels = p->audio_channels;
  return 0;
}

static void free_enc(dcap_enc* e) {
  if (!e) return;
  if (e->sws) sws_freeContext(e->sws);
  av_frame_free(&e->vframe);
  av_frame_free(&e->aframe);
  av_packet_free(&e->pkt);
  avcodec_free_context(&e->vctx);
  avcodec_free_context(&e->actx);
  if (e->fmt) {
    if (e->fmt->pb) avio_closep(&e->fmt->pb);
    avformat_free_context(e->fmt);
  }
  free(e);
}

dcap_enc* dcap_enc_open(const dcap_enc_params* p, char* err, size_t errlen) {
  av_log_set_level(AV_LOG_ERROR);
  dcap_enc* e = calloc(1, sizeof(*e));
  if (!e) return NULL;
  e->width = p->width;
  e->height = p->height;
  int rc = avformat_alloc_output_context2(&e->fmt, NULL, p->container, p->path);
  if (rc < 0 || !e->fmt) {
    set_averr(err, errlen, "InvalidParameters: container", rc);
    free_enc(e);
    return NULL;
  }
  if (open_video(e, p, err, errlen) < 0) {
    free_enc(e);
    return NULL;
  }
  if (p->audio_codec && p->audio_codec[0]) {
    if (open_audio(e, p, err, errlen) < 0) {
      free_enc(e);
      return NULL;
    }
  }
  e->pkt = av_packet_alloc();
  rc = avio_open(&e->fmt->pb, p->path, AVIO_FLAG_WRITE);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderWriteFailed: open output", rc);
    free_enc(e);
    return NULL;
  }
  rc = avformat_write_header(e->fmt, NULL);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderWriteFailed: write header", rc);
    free_enc(e);
    return NULL;
  }
  e->header_written = 1;
  return e;
}

int dcap_enc_video(dcap_enc* e, const uint8_t* bgra, int stride, int64_t pts, char* err, size_t errlen) {
  int rc;
  if (e->sws_needed) {
    double t0 = now_ms();
    rc = av_frame_make_writable(e->vframe);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderFailed: frame not writable", rc);
      return -1;
    }
    const uint8_t* src[4] = {bgra, NULL, NULL, NULL};
    int src_stride[4] = {stride, 0, 0, 0};
    sws_scale(e->sws, src, src_stride, 0, e->height, e->vframe->data, e->vframe->linesize);
    e->stats.convert_ms_total += now_ms() - t0;
  } else {
    /* Zero-copy: the encoder reads the canonical buffer directly. */
    e->vframe->data[0] = (uint8_t*)bgra;
    e->vframe->linesize[0] = stride;
  }
  e->vframe->pts = pts;
  e->vframe->duration = 1;
  double t1 = now_ms();
  rc = avcodec_send_frame(e->vctx, e->vframe);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderFailed: send_frame", rc);
    return -1;
  }
  e->stats.video_frames_in++;
  rc = drain(e, e->vctx, e->vstream, &e->stats.video_packets);
  e->stats.encode_ms_total += now_ms() - t1;
  if (rc < 0) {
    set_averr(err, errlen, "EncoderWriteFailed: video packet", rc);
    return -1;
  }
  return 0;
}

int dcap_enc_audio(dcap_enc* e, const uint8_t* pcm, int sample_frames, int64_t pts, char* err, size_t errlen) {
  if (!e->actx) {
    set_err(err, errlen, "InvalidParameters: encoder has no audio stream");
    return -1;
  }
  if (sample_frames <= 0) return 0;
  const int bps = e->audio_sample_format == 2 ? 4 : 2;
  int offset = 0;
  while (offset < sample_frames) {
    int chunk = sample_frames - offset;
    if (e->actx->frame_size > 0 && chunk > e->actx->frame_size) chunk = e->actx->frame_size;
    av_frame_unref(e->aframe);
    e->aframe->format = e->actx->sample_fmt;
    e->aframe->sample_rate = e->actx->sample_rate;
    av_channel_layout_copy(&e->aframe->ch_layout, &e->actx->ch_layout);
    e->aframe->nb_samples = chunk;
    int rc = av_frame_get_buffer(e->aframe, 0);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderFailed: audio frame buffer", rc);
      return -1;
    }
    memcpy(e->aframe->data[0], pcm + (size_t)offset * bps * e->audio_channels,
           (size_t)chunk * bps * e->audio_channels);
    e->aframe->pts = pts + offset;
    rc = avcodec_send_frame(e->actx, e->aframe);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderFailed: audio send_frame", rc);
      return -1;
    }
    rc = drain(e, e->actx, e->astream, &e->stats.audio_packets);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderWriteFailed: audio packet", rc);
      return -1;
    }
    offset += chunk;
  }
  e->stats.audio_sample_frames += sample_frames;
  return 0;
}

int dcap_enc_finish(dcap_enc* e, dcap_enc_stats* stats, char* err, size_t errlen) {
  int result = 0;
  int rc = avcodec_send_frame(e->vctx, NULL);
  if (rc >= 0) rc = drain(e, e->vctx, e->vstream, &e->stats.video_packets);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderWriteFailed: video flush", rc);
    result = -1;
  }
  if (e->actx && result == 0) {
    rc = avcodec_send_frame(e->actx, NULL);
    if (rc >= 0) rc = drain(e, e->actx, e->astream, &e->stats.audio_packets);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderWriteFailed: audio flush", rc);
      result = -1;
    }
  }
  if (result == 0) {
    rc = av_write_trailer(e->fmt);
    if (rc < 0) {
      set_averr(err, errlen, "EncoderWriteFailed: trailer", rc);
      result = -1;
    }
  }
  if (e->vstream && e->vstream->codecpar->extradata_size > 0) {
    int n = e->vstream->codecpar->extradata_size;
    if (n > (int)sizeof(e->stats.extradata)) n = sizeof(e->stats.extradata);
    memcpy(e->stats.extradata, e->vstream->codecpar->extradata, n);
    e->stats.extradata_len = n;
  }
  if (e->fmt->pb) {
    int64_t size = avio_size(e->fmt->pb);
    if (size > 0) e->stats.bytes_written = size;
    rc = avio_closep(&e->fmt->pb);
    if (rc < 0 && result == 0) {
      set_averr(err, errlen, "EncoderWriteFailed: close", rc);
      result = -1;
    }
  }
  if (stats) *stats = e->stats;
  free_enc(e);
  return result;
}

void dcap_enc_abort(dcap_enc* e) {
  free_enc(e);
}

int dcap_codec_probe(const dcap_enc_params* p, char* err, size_t errlen) {
  av_log_set_level(AV_LOG_ERROR);
  const AVCodec* codec = avcodec_find_encoder_by_name(p->codec);
  if (!codec) {
    set_err(err, errlen, "EncoderUnavailable: no encoder named %s in this build", p->codec);
    return -1;
  }
  enum AVPixelFormat pix = av_get_pix_fmt(p->pix_fmt);
  if (pix == AV_PIX_FMT_NONE) {
    set_err(err, errlen, "InvalidParameters: unknown pixel format %s", p->pix_fmt);
    return -1;
  }
  const enum AVPixelFormat* fmts = NULL;
  int nfmts = 0;
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void**)&fmts, &nfmts) >= 0 && fmts) {
    int ok = 0;
    for (int i = 0; i < nfmts; i++) ok |= fmts[i] == pix;
    if (!ok) {
      set_err(err, errlen, "InvalidParameters: %s does not accept pixel format %s", p->codec, p->pix_fmt);
      avcodec_free_context(&ctx);
      return -1;
    }
  }
  ctx->width = p->width;
  ctx->height = p->height;
  ctx->pix_fmt = pix;
  ctx->time_base = (AVRational){p->fps_den, p->fps_num};
  ctx->framerate = (AVRational){p->fps_num, p->fps_den};
  ctx->thread_count = p->threads;
  for (int i = 0; i < p->n_options; i++) {
    int rc = av_opt_set(ctx, p->options[2 * i], p->options[2 * i + 1], AV_OPT_SEARCH_CHILDREN);
    if (rc < 0) {
      set_averr(err, errlen, "InvalidParameters: encoder option", rc);
      avcodec_free_context(&ctx);
      return -1;
    }
  }
  int rc = avcodec_open2(ctx, codec, NULL);
  avcodec_free_context(&ctx);
  if (rc < 0) {
    set_averr(err, errlen, "EncoderUnavailable: avcodec_open2", rc);
    return -1;
  }
  return 0;
}

int dcap_probe(const char* path, dcap_probe_info* info, char* err, size_t errlen) {
  memset(info, 0, sizeof(*info));
  av_log_set_level(AV_LOG_ERROR);
  AVFormatContext* fmt = NULL;
  int rc = avformat_open_input(&fmt, path, NULL, NULL);
  if (rc < 0) {
    set_averr(err, errlen, "ProbeFailed: open", rc);
    return -1;
  }
  rc = avformat_find_stream_info(fmt, NULL);
  if (rc < 0) {
    set_averr(err, errlen, "ProbeFailed: stream info", rc);
    avformat_close_input(&fmt);
    return -1;
  }
  int vidx = -1, aidx = -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++) {
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = i;
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = i;
  }
  if (vidx < 0) {
    set_err(err, errlen, "ProbeFailed: no video stream");
    avformat_close_input(&fmt);
    return -1;
  }
  AVCodecParameters* vp = fmt->streams[vidx]->codecpar;
  snprintf(info->codec, sizeof(info->codec), "%s", avcodec_get_name(vp->codec_id));
  snprintf(info->pix_fmt, sizeof(info->pix_fmt), "%s", av_get_pix_fmt_name(vp->format) ? av_get_pix_fmt_name(vp->format) : "?");
  info->width = vp->width;
  info->height = vp->height;
  info->time_base_num = fmt->streams[vidx]->time_base.num;
  info->time_base_den = fmt->streams[vidx]->time_base.den;
  info->has_audio = aidx >= 0;
  if (vp->extradata_size > 0) {
    int n = vp->extradata_size > (int)sizeof(info->extradata) ? (int)sizeof(info->extradata) : vp->extradata_size;
    memcpy(info->extradata, vp->extradata, n);
    info->extradata_len = n;
  }
  info->video_all_keyframes = 1;
  AVPacket* pkt = av_packet_alloc();
  int first = 1;
  int64_t pts0 = AV_NOPTS_VALUE;
  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vidx) {
      int key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      if (first) { info->video_first_keyframe = key; first = 0; pts0 = pkt->pts; }
      else if (info->video_packets == 1 && pts0 != AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) info->first_pts_delta = pkt->pts - pts0;
      if (!key) info->video_all_keyframes = 0;
      info->video_packets++;
    } else if (pkt->stream_index == aidx) {
      AVCodecParameters* ap = fmt->streams[aidx]->codecpar;
      int64_t samples = pkt->duration;
      if (samples <= 0 && ap->codec_id == AV_CODEC_ID_PCM_S16LE) samples = pkt->size / (2 * ap->ch_layout.nb_channels);
      if (samples <= 0 && ap->codec_id == AV_CODEC_ID_PCM_F32LE) samples = pkt->size / (4 * ap->ch_layout.nb_channels);
      info->audio_sample_frames += samples;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  avformat_close_input(&fmt);
  return 0;
}

static int64_t samples_through(int64_t frame, int rate, int fps_num, int fps_den) {
  return (frame * (int64_t)rate * fps_den) / fps_num;
}

int dcap_join(const dcap_join_input* inputs, int n_inputs, int64_t start_frame, const char* out_path,
              const char* container, int fps_num, int fps_den, int audio_rate, dcap_join_stats* stats,
              char* err, size_t errlen) {
  memset(stats, 0, sizeof(*stats));
  av_log_set_level(AV_LOG_ERROR);
  if (n_inputs <= 0) {
    set_err(err, errlen, "JoinFailed: no inputs");
    return -1;
  }
  AVFormatContext* out = NULL;
  int rc = avformat_alloc_output_context2(&out, NULL, container, out_path);
  if (rc < 0) {
    set_averr(err, errlen, "JoinFailed: output context", rc);
    return -1;
  }
  AVStream* ov = NULL;
  AVStream* oa = NULL;
  AVCodecParameters* ref_v = avcodec_parameters_alloc();
  AVCodecParameters* ref_a = avcodec_parameters_alloc();
  AVPacket* pkt = av_packet_alloc();
  int result = 0;
  for (int k = 0; k < n_inputs && result == 0; k++) {
    AVFormatContext* in = NULL;
    rc = avformat_open_input(&in, inputs[k].path, NULL, NULL);
    if (rc < 0) { set_averr(err, errlen, "JoinFailed: open input", rc); result = -1; break; }
    rc = avformat_find_stream_info(in, NULL);
    if (rc < 0) { set_averr(err, errlen, "JoinFailed: stream info", rc); avformat_close_input(&in); result = -1; break; }
    int vidx = -1, aidx = -1;
    for (unsigned i = 0; i < in->nb_streams; i++) {
      if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = i;
      if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = i;
    }
    if (vidx < 0) { set_err(err, errlen, "JoinFailed: %s has no video stream", inputs[k].path); avformat_close_input(&in); result = -1; break; }
    AVCodecParameters* vp = in->streams[vidx]->codecpar;
    AVCodecParameters* ap = aidx >= 0 ? in->streams[aidx]->codecpar : NULL;
    if (k == 0) {
      avcodec_parameters_copy(ref_v, vp);
      ov = avformat_new_stream(out, NULL);
      avcodec_parameters_copy(ov->codecpar, vp);
      ov->codecpar->codec_tag = 0;
      ov->time_base = (AVRational){fps_den, fps_num};
      ov->avg_frame_rate = (AVRational){fps_num, fps_den};
      if (ap) {
        avcodec_parameters_copy(ref_a, ap);
        oa = avformat_new_stream(out, NULL);
        avcodec_parameters_copy(oa->codecpar, ap);
        oa->codecpar->codec_tag = 0;
        oa->time_base = (AVRational){1, audio_rate};
      }
      if (out->oformat->flags & AVFMT_GLOBALHEADER) out->flags |= AVFMT_FLAG_AUTO_BSF;
      rc = avio_open(&out->pb, out_path, AVIO_FLAG_WRITE);
      if (rc < 0) { set_averr(err, errlen, "JoinFailed: open output", rc); avformat_close_input(&in); result = -1; break; }
      rc = avformat_write_header(out, NULL);
      if (rc < 0) { set_averr(err, errlen, "JoinFailed: write header", rc); avformat_close_input(&in); result = -1; break; }
    } else {
      int same = vp->codec_id == ref_v->codec_id && vp->width == ref_v->width && vp->height == ref_v->height &&
                 vp->format == ref_v->format && vp->extradata_size == ref_v->extradata_size &&
                 (vp->extradata_size == 0 || memcmp(vp->extradata, ref_v->extradata, vp->extradata_size) == 0);
      if (!same) { set_err(err, errlen, "JoinMismatch: %s differs from chunk 0 in codec, size, format or extradata", inputs[k].path); avformat_close_input(&in); result = -1; break; }
      if ((ap != NULL) != (oa != NULL)) { set_err(err, errlen, "JoinMismatch: %s audio presence differs from chunk 0", inputs[k].path); avformat_close_input(&in); result = -1; break; }
      if (ap && (ap->codec_id != ref_a->codec_id || ap->sample_rate != ref_a->sample_rate || ap->ch_layout.nb_channels != ref_a->ch_layout.nb_channels)) {
        set_err(err, errlen, "JoinMismatch: %s audio format differs from chunk 0", inputs[k].path); avformat_close_input(&in); result = -1; break;
      }
    }
    const int64_t video_base = inputs[k].first_frame - start_frame;
    const int64_t audio_base = samples_through(inputs[k].first_frame, audio_rate, fps_num, fps_den) -
                               samples_through(start_frame, audio_rate, fps_num, fps_den);
    int64_t vcount = 0, acount = 0;
    int first_key = -1;
    while (av_read_frame(in, pkt) >= 0 && result == 0) {
      if (pkt->stream_index == vidx) {
        if (first_key < 0) first_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        /* Timestamps are computed in frame units (fps_den/fps_num) and
         * rescaled to whatever time base the muxer chose at write_header. */
        const AVRational frame_tb = {fps_den, fps_num};
        pkt->stream_index = ov->index;
        pkt->pts = av_rescale_q(video_base + vcount, frame_tb, ov->time_base);
        pkt->dts = pkt->pts;
        pkt->duration = av_rescale_q(video_base + vcount + 1, frame_tb, ov->time_base) - pkt->pts;
        pkt->pos = -1;
        vcount++;
        stats->bytes_written += pkt->size;
        rc = av_interleaved_write_frame(out, pkt);
        if (rc < 0) { set_averr(err, errlen, "JoinFailed: write video", rc); result = -1; }
      } else if (oa && pkt->stream_index == aidx) {
        int64_t samples = pkt->duration;
        if (samples <= 0 && ap->codec_id == AV_CODEC_ID_PCM_S16LE) samples = pkt->size / (2 * ap->ch_layout.nb_channels);
        if (samples <= 0 && ap->codec_id == AV_CODEC_ID_PCM_F32LE) samples = pkt->size / (4 * ap->ch_layout.nb_channels);
        const AVRational sample_tb = {1, audio_rate};
        pkt->stream_index = oa->index;
        pkt->pts = av_rescale_q(audio_base + acount, sample_tb, oa->time_base);
        pkt->dts = pkt->pts;
        pkt->duration = av_rescale_q(samples, sample_tb, oa->time_base);
        pkt->pos = -1;
        acount += samples;
        stats->bytes_written += pkt->size;
        rc = av_interleaved_write_frame(out, pkt);
        if (rc < 0) { set_averr(err, errlen, "JoinFailed: write audio", rc); result = -1; }
      }
      av_packet_unref(pkt);
    }
    av_packet_unref(pkt);
    avformat_close_input(&in);
    if (result == 0) {
      if (vcount != inputs[k].frames) {
        set_err(err, errlen, "JoinMismatch: %s holds %lld video packets, expected %lld", inputs[k].path, (long long)vcount, (long long)inputs[k].frames);
        result = -1;
      } else if (first_key != 1) {
        set_err(err, errlen, "JoinMismatch: %s does not start with a keyframe", inputs[k].path);
        result = -1;
      } else if (oa && acount != inputs[k].audio_sample_frames) {
        set_err(err, errlen, "JoinMismatch: %s holds %lld audio samples, expected %lld", inputs[k].path, (long long)acount, (long long)inputs[k].audio_sample_frames);
        result = -1;
      }
    }
    stats->video_packets += vcount;
    stats->audio_sample_frames += acount;
  }
  if (result == 0 && out->pb) {
    rc = av_write_trailer(out);
    if (rc < 0) { set_averr(err, errlen, "JoinFailed: trailer", rc); result = -1; }
  }
  if (out->pb) avio_closep(&out->pb);
  av_packet_free(&pkt);
  avcodec_parameters_free(&ref_v);
  avcodec_parameters_free(&ref_a);
  avformat_free_context(out);
  return result;
}

void dcap_av_versions(char* buf, size_t len) {
  snprintf(buf, len, "lavc %u.%u.%u lavf %u.%u.%u lavu %u.%u.%u sws %u.%u.%u",
           AV_VERSION_MAJOR(avcodec_version()), AV_VERSION_MINOR(avcodec_version()), AV_VERSION_MICRO(avcodec_version()),
           AV_VERSION_MAJOR(avformat_version()), AV_VERSION_MINOR(avformat_version()), AV_VERSION_MICRO(avformat_version()),
           AV_VERSION_MAJOR(avutil_version()), AV_VERSION_MINOR(avutil_version()), AV_VERSION_MICRO(avutil_version()),
           AV_VERSION_MAJOR(swscale_version()), AV_VERSION_MINOR(swscale_version()), AV_VERSION_MICRO(swscale_version()));
}

void dcap_av_encoders(char* buf, size_t len) {
  void* it = NULL;
  const AVCodec* c;
  size_t used = 0;
  buf[0] = 0;
  while ((c = av_codec_iterate(&it)) != NULL) {
    if (!av_codec_is_encoder(c)) continue;
    int n = snprintf(buf + used, len - used, "%s%s", used ? "," : "", c->name);
    if (n < 0 || (size_t)n >= len - used) break;
    used += n;
  }
}
