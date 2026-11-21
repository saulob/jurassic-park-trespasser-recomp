#include <stddef.h>

#include "smacker_backend.h"

#ifndef JPT_SMACKER_FFMPEG

const smk_backend_ops_t* smk_ffmpeg_backend(void) { return NULL; }
int smk_ffmpeg_describe(char* out, unsigned capacity) {
    if (out && capacity) out[0] = 0;
    return 0;
}

#else

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>

#define IO_BUFFER_BYTES 32768

struct smk_backend {
    const smk_source_t* source;
    smk_header_t header;
    unsigned streams;
    uint64_t position;
    AVIOContext* io;
    AVFormatContext* format;
    AVCodecContext* video_codec;
    AVCodecContext* audio_codec[SMK_TRACKS];
    int video_stream;
    int track_stream[SMK_TRACKS]; /* the format's stream of each track, -1 when the track is absent */
    AVPacket* packet;
    AVFrame* frame;
    uint8_t* fifo[SMK_TRACKS];
    size_t fifo_bytes[SMK_TRACKS], fifo_capacity[SMK_TRACKS];
    int ended;
};

static int io_read(void* opaque, uint8_t* buffer, int size) {
    smk_backend_t* b = (smk_backend_t*)opaque;
    uint32_t got = b->source->read_at(b->source->context, b->position, buffer, (uint32_t)size);
    if (!got) return AVERROR_EOF;
    b->position += got;
    return (int)got;
}

static int64_t io_seek(void* opaque, int64_t offset, int whence) {
    smk_backend_t* b = (smk_backend_t*)opaque;
    int64_t target;
    if (whence == AVSEEK_SIZE) return (int64_t)b->source->size;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = (int64_t)b->position + offset; break;
    case SEEK_END: target = (int64_t)b->source->size + offset; break;
    default: return AVERROR(EINVAL);
    }
    if (target < 0) return AVERROR(EINVAL);
    b->position = (uint64_t)target;
    return target;
}

static void backend_close(smk_backend_t* b) {
    if (!b) return;
    avcodec_free_context(&b->video_codec);
    for (unsigned t = 0; t < SMK_TRACKS; t++) {
        avcodec_free_context(&b->audio_codec[t]);
        free(b->fifo[t]);
    }
    av_frame_free(&b->frame);
    av_packet_free(&b->packet);
    if (b->format) avformat_close_input(&b->format);
    if (b->io) {
        av_freep(&b->io->buffer);
        avio_context_free(&b->io);
    }
    free(b);
}

static AVCodecContext* open_decoder(const AVStream* stream) {
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* context;
    if (!codec) return NULL;
    context = avcodec_alloc_context3(codec);
    if (!context) return NULL;
    if (avcodec_parameters_to_context(context, stream->codecpar) < 0 || avcodec_open2(context, codec, NULL) < 0)
        avcodec_free_context(&context);
    return context;
}

static smk_backend_t* backend_open(const smk_source_t* source, const smk_header_t* header, unsigned streams) {
    smk_backend_t* b = (smk_backend_t*)calloc(1, sizeof *b);
    uint8_t* buffer;
    int audio_index = 0;
    if (!b) return NULL;
    b->source = source;
    b->header = *header;
    b->streams = streams;
    b->video_stream = -1;
    for (unsigned t = 0; t < SMK_TRACKS; t++) b->track_stream[t] = -1;
    buffer = (uint8_t*)av_malloc(IO_BUFFER_BYTES);
    b->io = buffer ? avio_alloc_context(buffer, IO_BUFFER_BYTES, 0, b, io_read, NULL, io_seek) : NULL;
    if (!b->io) {
        av_free(buffer);
        goto fail;
    }
    b->format = avformat_alloc_context();
    if (!b->format) goto fail;
    b->format->pb = b->io;
    b->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&b->format, "", av_find_input_format("smk"), NULL) < 0) goto fail;
    if (avformat_find_stream_info(b->format, NULL) < 0) goto fail;
    for (unsigned i = 0; i < b->format->nb_streams; i++) {
        AVStream* stream = b->format->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && b->video_stream < 0) {
            b->video_stream = (int)i;
            if (!(streams & SMK_STREAM_VIDEO)) stream->discard = AVDISCARD_ALL;
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            unsigned seen = 0, track = 0;
            for (; track < SMK_TRACKS; track++)
                if (header->track[track].rate && seen++ == (unsigned)audio_index) break;
            audio_index++;
            if (track >= SMK_TRACKS) continue;
            b->track_stream[track] = (int)i;
            if (!(streams & SMK_STREAM_AUDIO)) stream->discard = AVDISCARD_ALL;
        }
    }
    if ((streams & SMK_STREAM_VIDEO) && b->video_stream < 0) goto fail;
    if (streams & SMK_STREAM_VIDEO) {
        b->video_codec = open_decoder(b->format->streams[b->video_stream]);
        if (!b->video_codec) goto fail;
    }
    if (streams & SMK_STREAM_AUDIO) {
        for (unsigned t = 0; t < SMK_TRACKS; t++)
            if (b->track_stream[t] >= 0) b->audio_codec[t] = open_decoder(b->format->streams[b->track_stream[t]]);
    }
    b->packet = av_packet_alloc();
    b->frame = av_frame_alloc();
    if (!b->packet || !b->frame) goto fail;
    return b;
fail:
    backend_close(b);
    return NULL;
}

static int fifo_append(smk_backend_t* b, unsigned track, const uint8_t* data, size_t bytes) {
    if (b->fifo_bytes[track] + bytes > b->fifo_capacity[track]) {
        size_t capacity = (b->fifo_bytes[track] + bytes) * 2;
        uint8_t* grown = (uint8_t*)realloc(b->fifo[track], capacity);
        if (!grown) return 0;
        b->fifo[track] = grown;
        b->fifo_capacity[track] = capacity;
    }
    memcpy(b->fifo[track] + b->fifo_bytes[track], data, bytes);
    b->fifo_bytes[track] += bytes;
    return 1;
}

static void take_audio_frames(smk_backend_t* b, unsigned track) {
    AVCodecContext* codec = b->audio_codec[track];
    while (avcodec_receive_frame(codec, b->frame) >= 0) {
        if (!av_sample_fmt_is_planar((enum AVSampleFormat)b->frame->format)) {
            size_t bytes = (size_t)b->frame->nb_samples * (size_t)b->frame->ch_layout.nb_channels *
                           (size_t)av_get_bytes_per_sample((enum AVSampleFormat)b->frame->format);
            fifo_append(b, track, b->frame->data[0], bytes);
        }
        av_frame_unref(b->frame);
    }
}

static int copy_picture(smk_backend_t* b, uint8_t* picture, uint8_t* palette) {
    const AVFrame* f = b->frame;
    if (f->format != AV_PIX_FMT_PAL8 || (uint32_t)f->width != b->header.width || (uint32_t)f->height != b->header.height || !f->data[1])
        return 0;
    for (int y = 0; y < f->height; y++) memcpy(picture + (size_t)y * (size_t)f->width, f->data[0] + (size_t)y * (size_t)f->linesize[0], (size_t)f->width);
    for (int i = 0; i < 256; i++) {
        uint32_t color;
        memcpy(&color, f->data[1] + i * 4, sizeof color);
        palette[i * 3] = (uint8_t)(color >> 16);
        palette[i * 3 + 1] = (uint8_t)(color >> 8);
        palette[i * 3 + 2] = (uint8_t)color;
    }
    return 1;
}

static int backend_next_video(smk_backend_t* b, uint8_t* picture, uint8_t* palette) {
    if (!b->video_codec) return 0;
    while (!b->ended) {
        int result = avcodec_receive_frame(b->video_codec, b->frame);
        if (result >= 0) {
            int copied = copy_picture(b, picture, palette);
            av_frame_unref(b->frame);
            return copied;
        }
        if (result != AVERROR(EAGAIN)) return 0;
        if (av_read_frame(b->format, b->packet) < 0) {
            avcodec_send_packet(b->video_codec, NULL);
            b->ended = 1;
            continue;
        }
        if (b->packet->stream_index == b->video_stream) avcodec_send_packet(b->video_codec, b->packet);
        av_packet_unref(b->packet);
    }
    if (avcodec_receive_frame(b->video_codec, b->frame) >= 0) {
        int copied = copy_picture(b, picture, palette);
        av_frame_unref(b->frame);
        return copied;
    }
    return 0;
}

static uint32_t backend_next_audio(smk_backend_t* b, uint32_t track, uint8_t* out, uint32_t capacity) {
    size_t take;
    if (track >= SMK_TRACKS || !b->audio_codec[track]) return 0;
    while (!b->fifo_bytes[track] && !b->ended) {
        if (av_read_frame(b->format, b->packet) < 0) {
            b->ended = 1;
            for (unsigned t = 0; t < SMK_TRACKS; t++) {
                if (!b->audio_codec[t]) continue;
                avcodec_send_packet(b->audio_codec[t], NULL);
                take_audio_frames(b, t);
            }
            break;
        }
        for (unsigned t = 0; t < SMK_TRACKS; t++) {
            if (b->audio_codec[t] && b->packet->stream_index == b->track_stream[t]) {
                avcodec_send_packet(b->audio_codec[t], b->packet);
                take_audio_frames(b, t);
            }
        }
        av_packet_unref(b->packet);
    }
    take = b->fifo_bytes[track] < capacity ? b->fifo_bytes[track] : capacity;
    if (!take) return 0;
    memcpy(out, b->fifo[track], take);
    memmove(b->fifo[track], b->fifo[track] + take, b->fifo_bytes[track] - take);
    b->fifo_bytes[track] -= take;
    return (uint32_t)take;
}

const smk_backend_ops_t* smk_ffmpeg_backend(void) {
    static const smk_backend_ops_t ops = { "ffmpeg", backend_open, backend_close, backend_next_video, backend_next_audio };
    return &ops;
}

int smk_ffmpeg_describe(char* out, unsigned capacity) {
    return snprintf(out, capacity, "libavformat %u.%u.%u (%s), libavcodec %u.%u.%u (%s), libavutil %u.%u.%u (%s)",
                    avformat_version() >> 16, (avformat_version() >> 8) & 255, avformat_version() & 255, avformat_license(),
                    avcodec_version() >> 16, (avcodec_version() >> 8) & 255, avcodec_version() & 255, avcodec_license(),
                    avutil_version() >> 16, (avutil_version() >> 8) & 255, avutil_version() & 255, avutil_license());
}

#endif
