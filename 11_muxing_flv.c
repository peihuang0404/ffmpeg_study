﻿/**
 * @file
 * libavformat API example.
 *
 * Output a media file in any supported libavformat format. The default
 * codecs are used.
 * @example muxing.c
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define STREAM_DURATION 5.0               // 流时长 单位秒
#define STREAM_FRAME_RATE 25              /* 25 images/s */
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P /* default pix_fmt */

#define SCALE_FLAGS SWS_BICUBIC // scale flag

// 封装单个输出AVStream   audio 、 video是独立的
typedef struct OutputStream {
  AVStream *st;        // 代表一个stream, 1路audio或1路video都代表独立的steam
  AVCodecContext *enc; // 编码器上下文

  /* pts of the next frame that will be generated */
  int64_t next_pts;  // 时间戳相关
  int samples_count; // 音频的采样数量累计

  AVFrame *frame;     // 重采样后的frame，  视频叫scale
  AVFrame *tmp_frame; // 重采样前
  AVPacket *tmp_pkt;

  float t, tincr, tincr2; // 这几个参数用来生成PCM和YUV用的

  struct SwsContext *sws_ctx; // 图像scale
  struct SwrContext *swr_ctx; // 音频重采样
} OutputStream;

static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt) {
  AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

  printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s "
         "stream_index:%d\n",
         av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
         av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
         av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
         pkt->stream_index);
}

static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, AVPacket *pkt) {
  int ret;

  // send the frame to the encoder
  ret = avcodec_send_frame(c, frame); // 3P   2B    I1
  if (ret < 0) {
    fprintf(stderr, "Error sending a frame to the encoder: %s\n",
            av_err2str(ret));
    exit(1);
  }

  while (ret >= 0) {
    ret = avcodec_receive_packet(c, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0) {
      fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
      exit(1);
    }

    /* rescale output packet timestamp values from codec to stream timebase */
    // 将packet的timestamp由codec to stream timebase pts_before = -1024
    av_packet_rescale_ts(
        pkt, c->time_base,
        st->time_base); // flv (1024/44100) * (1000/1) = 23
                        // ts (1024/44100) * (90000/1) =  2089.7 -- 2090
    pkt->stream_index =
        st->index; // 复用的时候 要去设置audio video packet的stream index

    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);
    ret = av_interleaved_write_frame(fmt_ctx, pkt); // write packet
    /* pkt is now blank (av_interleaved_write_frame() takes ownership of
     * its contents and resets pkt), so that no unreferencing is necessary.
     * This would be different if one used av_write_frame(). */
    if (ret < 0) {
      fprintf(stderr, "Error while writing output packet: %s\n",
              av_err2str(ret));
      exit(1);
    }
  }

  return ret == AVERROR_EOF ? 1 : 0;
}

// 增加输出流，返回AVStream，并给codec赋值,但此时codec并未打开
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       const AVCodec **codec, enum AVCodecID codec_id) {
  AVCodecContext *codec_ctx;
  int i;

  /* 查找编码器 */
  *codec = avcodec_find_encoder(codec_id); // 通过codec_id找到编码器
  if (!(*codec)) {
    fprintf(stderr, "Could not find encoder for '%s'\n",
            avcodec_get_name(codec_id));
    exit(1);
  }
  // 新建码流 绑定到 AVFormatContext stream->index 有设置
  ost->tmp_pkt = av_packet_alloc();
  if (!ost->tmp_pkt) {
    fprintf(stderr, "Could not allocate AVPacket\n");
    exit(1);
  }
  ost->st =
      avformat_new_stream(oc, NULL); // 创建一个流成分 oc->nb_streams 内部加一
  if (!ost->st) {
    fprintf(stderr, "Could not allocate stream\n");
    exit(1);
  }
  /* 为什么要 -1呢？每次调用avformat_new_stream的时候nb_streams+1
     但id是从0开始, 比如第1个流：对应流id = nb_streams(1) -1 = 0
                      第2个流：对应流id = nb_streams(2) -1 = 1
  */
  ost->st->id = oc->nb_streams - 1;
  codec_ctx = avcodec_alloc_context3(*codec); // 创建编码器上下文
  if (!codec_ctx) {
    fprintf(stderr, "Could not alloc an encoding context\n");
    exit(1);
  }
  ost->enc = codec_ctx;
  // 初始化编码器参数
  switch ((*codec)->type) {
  case AVMEDIA_TYPE_AUDIO:
    codec_ctx->sample_fmt = (*codec)->sample_fmts
                                ? (*codec)->sample_fmts[0]
                                : AV_SAMPLE_FMT_FLTP; // 采样格式
    codec_ctx->bit_rate = 64000;                      // 码率
    codec_ctx->sample_rate = 44100;                   // 采样率
    if ((*codec)->supported_samplerates) {
      codec_ctx->sample_rate = (*codec)->supported_samplerates[0];
      for (i = 0; (*codec)->supported_samplerates[i]; i++) {
        if ((*codec)->supported_samplerates[i] == 44100)
          codec_ctx->sample_rate = 44100;
      }
    } // channel相关的信息 channel layout和channels
    av_channel_layout_copy(&codec_ctx->ch_layout,
                           &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
    ost->st->time_base = (AVRational){
        1,
        codec_ctx
            ->sample_rate}; // stream timebase预设参数  有些封装格式预设是有作用
    break;

  case AVMEDIA_TYPE_VIDEO:
    codec_ctx->codec_id = codec_id;
    codec_ctx->bit_rate = 400000;
    /* Resolution must be a multiple of two. */
    codec_ctx->width = 352; // 分辨率
    codec_ctx->height = 288;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    ost->st->time_base = (AVRational){1, STREAM_FRAME_RATE};   // 帧率 时基
    codec_ctx->time_base = (AVRational){1, STREAM_FRAME_RATE}; // 时间基准

    codec_ctx->gop_size = STREAM_FRAME_RATE; /* emit one intra frame every
                                                twelve frames at most */
    codec_ctx->pix_fmt = STREAM_PIX_FMT;
    break;

  default:
    break;
  }

  /* Some formats want stream headers to be separate. */
  if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
    // sps pps信息  extra_data保存对应的 mkv flv mp4
    codec_ctx->flags |=
        AV_CODEC_FLAG_GLOBAL_HEADER; // mkv flv mp4,
                                     // 每个关键帧前面就不会添加SPS/PPS
    printf("%s set AV_CODEC_FLAG_GLOBAL_HEADER\n", oc->oformat->name);
  } else {
    // ts
    printf("%s no AV_CODEC_FLAG_GLOBAL_HEADER\n", oc->oformat->name);
  }

  /*
  AV_CODEC_FLAG_GLOBAL_HEADER对于编码的影响，比如libx264.c
  if (avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)
     x4->params.b_repeat_headers = 0;*/
}

/**************************************************************/
/* audio output */

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  const AVChannelLayout *channel_layout,
                                  int sample_rate, int nb_samples) {
  AVFrame *frame = av_frame_alloc();

  if (!frame) {
    fprintf(stderr, "Error allocating an audio frame\n");
    exit(1);
  }

  frame->format = sample_fmt;
  av_channel_layout_copy(&frame->ch_layout, channel_layout);
  frame->sample_rate = sample_rate;
  frame->nb_samples = nb_samples;

  if (nb_samples) {
    if (av_frame_get_buffer(frame, 0) < 0) {
      fprintf(stderr, "Error allocating an audio buffer\n");
      exit(1);
    }
  }

  return frame;
}

static void open_audio(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg) {
  AVCodecContext *codec_ctx;
  int nb_samples;
  int ret;
  AVDictionary *opt = NULL;

  codec_ctx = ost->enc;

  /* open it */
  av_dict_copy(&opt, opt_arg, 0);
  // 1. 关联编码器 会设置codec_ctx->time_base
  ret = avcodec_open2(codec_ctx, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
    exit(1);
  }

  /* init signal generator */
  // 2. 初始化产生PCM的参数
  ost->t = 0;
  ost->tincr = 2 * M_PI * 110.0 / codec_ctx->sample_rate;
  /* increment frequency by 110 Hz per second */
  ost->tincr2 =
      2 * M_PI * 110.0 / codec_ctx->sample_rate / codec_ctx->sample_rate;

  // 每次需要的samples
  //    if (codec_ctx->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
  //        nb_samples = 10000; // 支持可变FRAME size的编码器极少，直接注释掉
  //    else
  nb_samples =
      codec_ctx->frame_size; // 1024 样本； 单通道、双通道  s16格式 2字节;
                             // 双通道1024 *2 *2 ，双通道1024 *1 *2

  // signal generator -> PCM -> ost->tmp_frame -> swr_convert重采样 ->
  // ost->frame -> 编码器 分配送给编码器的帧, 并申请对应的buffer
  ost->frame = alloc_audio_frame(codec_ctx->sample_fmt, &codec_ctx->ch_layout,
                                 codec_ctx->sample_rate, nb_samples);
  // 分配送给信号生成PCM的帧, 并申请对应的buffer
  ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &codec_ctx->ch_layout,
                                     codec_ctx->sample_rate, nb_samples);

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, codec_ctx);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }

  /* create resampler context 创建重采样器 */
  ost->swr_ctx = swr_alloc();
  if (!ost->swr_ctx) {
    fprintf(stderr, "Could not allocate resampler context\n");
    exit(1);
  }

  /* set options */
  av_opt_set_chlayout(ost->swr_ctx, "in_chlayout", &codec_ctx->ch_layout, 0);
  av_opt_set_int(ost->swr_ctx, "in_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  av_opt_set_chlayout(ost->swr_ctx, "out_chlayout", &codec_ctx->ch_layout, 0);
  av_opt_set_int(ost->swr_ctx, "out_sample_rate", codec_ctx->sample_rate, 0);
  av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", codec_ctx->sample_fmt,
                        0);

  /* initialize the resampling context */
  if ((ret = swr_init(ost->swr_ctx)) < 0) {
    fprintf(stderr, "Failed to initialize the resampling context\n");
    exit(1);
  }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
static AVFrame *get_audio_frame(OutputStream *ost) {
  AVFrame *frame = ost->tmp_frame;
  int j, i, v;
  int16_t *q = (int16_t *)frame->data[0];

  /* check if we want to generate more frames */
  // 44100 * {1, 44100} = 1  -》44100*5 * {1, 44100} = 5
  // 5 *{1,1} = 5
  if (av_compare_ts(ost->next_pts, ost->enc->time_base, STREAM_DURATION,
                    (AVRational){1, 1}) >= 0) // 对比时间 转成秒对比
    return NULL;

  for (j = 0; j < frame->nb_samples; j++) {
    v = (int)(sin(ost->t) * 10000);
    for (i = 0; i < ost->enc->ch_layout.nb_channels; i++)
      *q++ = v;
    ost->t += ost->tincr;
    ost->tincr += ost->tincr2;
  }

  frame->pts =
      ost->next_pts; // 使用samples作为计数 设置pts 0, nb_samples(1024) 2048
  ost->next_pts += frame->nb_samples; // 音频PTS使用采样samples叠加

  return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(AVFormatContext *oc, OutputStream *ost) {
  AVCodecContext *codec_ctx;
  AVFrame *frame;
  int ret;
  int dst_nb_samples;

  codec_ctx = ost->enc;

  frame = get_audio_frame(ost);

  if (frame) {
    /* convert samples from native format to destination codec format, using the
     * resampler */
    /* compute destination number of samples */
    dst_nb_samples = av_rescale_rnd(
        swr_get_delay(ost->swr_ctx, codec_ctx->sample_rate) + frame->nb_samples,
        codec_ctx->sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
    av_assert0(dst_nb_samples == frame->nb_samples);

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(ost->frame);
    if (ret < 0)
      exit(1);

    /* convert to destination format */
    ret = swr_convert(ost->swr_ctx, ost->frame->data, dst_nb_samples,
                      (const uint8_t **)frame->data, frame->nb_samples);
    if (ret < 0) {
      fprintf(stderr, "Error while converting\n");
      exit(1);
    }
    frame = ost->frame;

    frame->pts = av_rescale_q(ost->samples_count,
                              (AVRational){1, codec_ctx->sample_rate},
                              codec_ctx->time_base);
    ost->samples_count += dst_nb_samples;
  }

  return write_frame(oc, codec_ctx, ost->st, frame, ost->tmp_pkt);
}

/**************************************************************/
/* video output */

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width,
                              int height) {
  AVFrame *picture;
  int ret;

  picture = av_frame_alloc();
  if (!picture)
    return NULL;

  picture->format = pix_fmt;
  picture->width = width;
  picture->height = height;

  /* allocate the buffers for the frame data */
  ret = av_frame_get_buffer(picture, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate frame data.\n");
    exit(1);
  }

  return picture;
}

static void open_video(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg) {
  int ret;
  AVCodecContext *codec_ctx = ost->enc;
  AVDictionary *opt = NULL;

  av_dict_copy(&opt, opt_arg, 0);

  /* open the codec */
  // 1. 关联编码器
  ret = avcodec_open2(codec_ctx, codec, &opt);
  av_dict_free(&opt);
  if (ret < 0) {
    fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
    exit(1);
  }
  // 2. 分配帧buffer
  /* allocate and init a re-usable frame */
  ost->frame =
      alloc_picture(codec_ctx->pix_fmt, codec_ctx->width, codec_ctx->height);
  if (!ost->frame) {
    fprintf(stderr, "Could not allocate video frame\n");
    exit(1);
  }

  /* If the output format is not YUV420P, then a temporary YUV420P
   * picture is needed too. It is then converted to the required
   * output format. */
  ost->tmp_frame = NULL;
  if (codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
    // 编码器格式需要的数据不是 AV_PIX_FMT_YUV420P才需要 调用图像scale
    ost->tmp_frame =
        alloc_picture(AV_PIX_FMT_YUV420P, codec_ctx->width, codec_ctx->height);
    if (!ost->tmp_frame) {
      fprintf(stderr, "Could not allocate temporary picture\n");
      exit(1);
    }
  }

  /* copy the stream parameters to the muxer */
  ret = avcodec_parameters_from_context(ost->st->codecpar, codec_ctx);
  if (ret < 0) {
    fprintf(stderr, "Could not copy the stream parameters\n");
    exit(1);
  }
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index, int width,
                           int height) {
  int x, y, i;

  i = frame_index;

  /* Y */
  for (y = 0; y < height; y++)
    for (x = 0; x < width; x++)
      pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

  /* Cb and Cr */
  for (y = 0; y < height / 2; y++) {
    for (x = 0; x < width / 2; x++) {
      pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
      pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
    }
  }
}

static AVFrame *get_video_frame(OutputStream *ost) {
  AVCodecContext *codec_ctx = ost->enc;

  /* check if we want to generate more frames */
  // 我们测试时只产生STREAM_DURATION(这里是10.0秒)的视频数据
  if (av_compare_ts(ost->next_pts, codec_ctx->time_base, STREAM_DURATION,
                    (AVRational){1, 1}) >= 0)
    return NULL;

  /* when we pass a frame to the encoder, it may keep a reference to it
   * internally; make sure we do not overwrite it here */
  if (av_frame_make_writable(ost->frame) < 0)
    exit(1);

  if (codec_ctx->pix_fmt != AV_PIX_FMT_YUV420P) {
    /* as we only generate a YUV420P picture, we must convert it
     * to the codec pixel format if needed */
    if (!ost->sws_ctx) {
      ost->sws_ctx = sws_getContext(codec_ctx->width, codec_ctx->height,
                                    AV_PIX_FMT_YUV420P, codec_ctx->width,
                                    codec_ctx->height, codec_ctx->pix_fmt,
                                    SCALE_FLAGS, NULL, NULL, NULL);
      if (!ost->sws_ctx) {
        fprintf(stderr, "Could not initialize the conversion context\n");
        exit(1);
      }
    }
    fill_yuv_image(ost->tmp_frame, ost->next_pts, codec_ctx->width,
                   codec_ctx->height);
    sws_scale(ost->sws_ctx, (const uint8_t *const *)ost->tmp_frame->data,
              ost->tmp_frame->linesize, 0, codec_ctx->height, ost->frame->data,
              ost->frame->linesize);
  } else {
    fill_yuv_image(ost->frame, ost->next_pts, codec_ctx->width,
                   codec_ctx->height);
  }

  ost->frame->pts = ost->next_pts++; // 为什么+1? 单位是 1*(1/25) = 0.04秒=40ms
                                     // ; 25*(1/25)=1秒=1000ms
  // 0  1 2  -> 0 40ms 80ms
  return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost) {
  return write_frame(oc, ost->enc, ost->st, get_video_frame(ost), ost->tmp_pkt);
}

static void close_stream(AVFormatContext *oc, OutputStream *ost) {
  avcodec_free_context(&ost->enc);
  av_frame_free(&ost->frame);
  av_frame_free(&ost->tmp_frame);
  av_packet_free(&ost->tmp_pkt);
  sws_freeContext(ost->sws_ctx);
  swr_free(&ost->swr_ctx);
}

/**************************************************************/
/* media file output */

int main(int argc, char **argv) {
  OutputStream video_st = {0}; // 封装视频编码相关的
  OutputStream audio_st = {0}; // 封装音频编码相关的
  const char *filename;        // 输出文件
  // AVOutputFormat ff_flv_muxer
  const AVOutputFormat
      *fmt; // 输出文件容器格式,
            // 封装了复用规则，AVInputFormat则是封装了解复用规则
  AVFormatContext *oc;
  const AVCodec *audio_codec, *video_codec;
  int ret;
  int have_video = 0, have_audio = 0;
  int encode_video = 0, encode_audio = 0;
  AVDictionary *opt = NULL;
  int i;

  if (argc < 2) {
    printf("usage: %s output_file\n"
           "API example program to output a media file with libavformat.\n"
           "This program generates a synthetic audio and video stream, encodes "
           "and\n"
           "muxes them into a file named output_file.\n"
           "The output format is automatically guessed according to the file "
           "extension.\n"
           "Raw images can also be output by using '%%d' in the filename.\n"
           "\n",
           argv[0]);
    return 1;
  }

  filename = argv[1];
  for (i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags"))
      av_dict_set(&opt, argv[i] + 1, argv[i + 1], 0);
  }

  /* 分配AVFormatContext并根据filename绑定合适的AVOutputFormat */
  avformat_alloc_output_context2(&oc, NULL, NULL, filename);
  if (!oc) {
    // 如果不能根据文件后缀名找到合适的格式，那缺省使用flv格式
    printf("Could not deduce output format from file extension: using flv.\n");
    avformat_alloc_output_context2(&oc, NULL, "flv", filename);
  }
  if (!oc)
    return 1;

  fmt = oc->oformat; // 获取绑定的AVOutputFormat
  // 我们音视频课程音视频编解码主要涉及H264和AAC, 所以我们指定为H264+AAC
  enum AVCodecID audio_codec_id = AV_CODEC_ID_AAC;  /**< default audio codec */
  enum AVCodecID video_codec_id = AV_CODEC_ID_H264; /**< default video codec */
  /* 使用指定的音视频编码格式增加音频流和视频流 */
  if (video_codec_id != AV_CODEC_ID_NONE) {
    add_stream(&video_st, oc, &video_codec, video_codec_id);
    have_video = 1;
    encode_video = 1;
  }
  if (audio_codec_id != AV_CODEC_ID_NONE) {
    add_stream(&audio_st, oc, &audio_codec, audio_codec_id);
    have_audio = 1;
    encode_audio = 1;
  }

  /* Now that all the parameters are set, we can open the audio and
   * video codecs and allocate the necessary encode buffers. */
  if (have_video)
    open_video(oc, video_codec, &video_st, opt);

  if (have_audio)
    open_audio(oc, audio_codec, &audio_st, opt);

  av_dump_format(oc, 0, filename, 1);

  /* open the output file, if needed */
  if (!(fmt->flags & AVFMT_NOFILE)) // flv没有这个flags
  {
    // 打开对应的输出文件，没有则创建
    ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      fprintf(stderr, "Could not open '%s': %s\n", filename, av_err2str(ret));
      return 1;
    }
  }
  // audio AVstream->base_time = 1/44100, video AVstream->base_time = 1/25
  /* 写头部. 到底做了什么操作呢？ 对应steam的time_base被改写 和封装格式有关系*/
  // 比如flv audio、video stream都为{1,1000}
  // 比如ts audio、video stream都为{1,90000}
  // 比如mp4: audio{1, 44100(采样率)} video{1,12800}
  // 比如mkv audio、video stream都为{1,1000}
  ret = avformat_write_header(oc,
                              &opt); // base_time audio = 1/1000 video = 1/1000
  if (ret < 0) {
    fprintf(stderr, "Error occurred when opening output file: %s\n",
            av_err2str(ret));
    return 1;
  }

  while (encode_video || encode_audio) {
    // next_pts = 50 ,   time_base= {1,25}, 50*(1/25)=2秒
    // next_pts  = 44100, time_base= {1,44100}, 44100*(1/44100)=1秒
    // next_pts  = 44100*2, time_base= {1,44100}, 88200*(1/44100)=2秒
    /* select the stream to encode */
    if (encode_video && // video_st.next_pts值 <= audio_st.next_pts时
        (!encode_audio ||
         av_compare_ts(video_st.next_pts, video_st.enc->time_base,
                       audio_st.next_pts, audio_st.enc->time_base) <= 0)) {
      //  printf("\nwrite_video_frame\n");
      encode_video = !write_video_frame(oc, &video_st);
    } else {
      //  printf("\nwrite_audio_frame\n");
      encode_audio = !write_audio_frame(oc, &audio_st);
    }
  }

  /* Write the trailer, if any. The trailer must be written before you
   * close the CodecContexts open when you wrote the header; otherwise
   * av_write_trailer() may try to use memory that was freed on
   * av_codec_close(). */
  av_write_trailer(oc);

  /* Close each codec. */
  if (have_video)
    close_stream(oc, &video_st);
  if (have_audio)
    close_stream(oc, &audio_st);

  if (!(fmt->flags & AVFMT_NOFILE))
    /* Close the output file. */
    avio_closep(&oc->pb);

  /* free the stream */
  avformat_free_context(oc);

  printf("mux finish\n");

  return 0;
}
