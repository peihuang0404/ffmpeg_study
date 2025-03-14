﻿/*
 * Copyright (c) 2012 Stefano Sabatini
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @example resampling_audio.c
 * libswresample API use example.
 */

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt) {
  int i;
  struct sample_fmt_entry {
    enum AVSampleFormat sample_fmt;
    const char *fmt_be, *fmt_le;
  } sample_fmt_entries[] = {
      {AV_SAMPLE_FMT_U8, "u8", "u8"},
      {AV_SAMPLE_FMT_S16, "s16be", "s16le"},
      {AV_SAMPLE_FMT_S32, "s32be", "s32le"},
      {AV_SAMPLE_FMT_FLT, "f32be", "f32le"},
      {AV_SAMPLE_FMT_DBL, "f64be", "f64le"},
  };
  *fmt = NULL;

  for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
    struct sample_fmt_entry *entry = &sample_fmt_entries[i];
    if (sample_fmt == entry->sample_fmt) {
      *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
      return 0;
    }
  }

  fprintf(stderr, "Sample format %s not supported as output format\n",
          av_get_sample_fmt_name(sample_fmt));
  return AVERROR(EINVAL);
}

/**
 * Fill dst buffer with nb_samples, generated starting from t. 交错模式的
 */
static void fill_samples(double *dst, int nb_samples, int nb_channels,
                         int sample_rate, double *t) {
  int i, j;
  double tincr = 1.0 / sample_rate, *dstp = dst;
  const double c = 2 * M_PI * 440.0;

  /* generate sin tone with 440Hz frequency and duplicated channels */
  for (i = 0; i < nb_samples; i++) {
    *dstp = sin(c * *t);
    for (j = 1; j < nb_channels; j++)
      dstp[j] = dstp[0];
    dstp += nb_channels;
    *t += tincr;
  }
}

int main(int argc, char **argv) {
  // 输入参数
  int64_t src_ch_layout = AV_CH_LAYOUT_STEREO;
  int src_rate = 48000;
  enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_DBL;
  uint8_t **src_data = NULL; // 二级指针
  int src_linesize;
  int src_nb_samples = 1024;

  // 输出参数
  int64_t dst_ch_layout = AV_CH_LAYOUT_STEREO;
  int dst_rate = 44100;
  enum AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;
  uint8_t **dst_data = NULL; // 二级指针
  int dst_linesize;
  int dst_nb_samples;
  int max_dst_nb_samples;

  // 输出文件
  const char *dst_filename = NULL; // 保存输出的pcm到本地，然后播放验证
  FILE *dst_file;

  int dst_bufsize;
  const char *fmt;

  // 重采样实例
  struct SwrContext *swr_ctx;

  double t;
  int ret;

  if (argc != 2) {
    fprintf(stderr,
            "Usage: %s output_file\n"
            "API example program to show how to resample an audio stream with "
            "libswresample.\n"
            "This program generates a series of audio frames, resamples them "
            "to a specified "
            "output format and rate and saves them to an output file named "
            "output_file.\n",
            argv[0]);
    exit(1);
  }
  dst_filename = argv[1];

  dst_file = fopen(dst_filename, "wb");
  if (!dst_file) {
    fprintf(stderr, "Could not open destination file %s\n", dst_filename);
    exit(1);
  }

  // 创建重采样器
  /* create resampler context */
  swr_ctx = swr_alloc();
  if (!swr_ctx) {
    fprintf(stderr, "Could not allocate resampler context\n");
    ret = AVERROR(ENOMEM);
    goto end;
  }

  // 设置重采样参数
  /* set options */
  // 输入参数
  AVChannelLayout src_nb_channels, dst_nb_channels;
  av_opt_set_chlayout(swr_ctx, "in_channel_layout", &src_nb_channels, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", src_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", src_sample_fmt, 0);
  // 输出参数
  av_opt_set_chlayout(swr_ctx, "out_channel_layout", &dst_nb_channels, 0);
  av_opt_set_int(swr_ctx, "out_sample_rate", dst_rate, 0);
  av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

  // 初始化重采样
  /* initialize the resampling context */
  if ((ret = swr_init(swr_ctx)) < 0) {
    fprintf(stderr, "Failed to initialize the resampling context\n");
    goto end;
  }

  /* allocate source and destination samples buffers */
  // 计算出输入源的通道数量
  // 给输入源分配内存空间
  ret = av_samples_alloc_array_and_samples(&src_data, &src_linesize,
                                           src_nb_channels.nb_channels,
                                           src_nb_samples, src_sample_fmt, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate source samples\n");
    goto end;
  }

  /* compute the number of converted samples: buffering is avoided
   * ensuring that the output buffer will contain at least all the
   * converted input samples */
  // 计算输出采样数量
  max_dst_nb_samples = dst_nb_samples =
      av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

  /* buffer is going to be directly written to a rawaudio file, no alignment
   */
  // 分配输出缓存内存
  ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize,
                                           dst_nb_channels.nb_channels,
                                           dst_nb_samples, dst_sample_fmt, 0);
  if (ret < 0) {
    fprintf(stderr, "Could not allocate destination samples\n");
    goto end;
  }

  t = 0;
  do {
    /* generate synthetic audio */
    // 生成输入源
    fill_samples((double *)src_data[0], src_nb_samples,
                 src_nb_channels.nb_channels, src_rate, &t);

    /* compute destination number of samples */
    int64_t delay = swr_get_delay(swr_ctx, src_rate);
    dst_nb_samples =
        av_rescale_rnd(delay + src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);
    if (dst_nb_samples > max_dst_nb_samples) {
      av_freep(&dst_data[0]);
      ret =
          av_samples_alloc(dst_data, &dst_linesize, dst_nb_channels.nb_channels,
                           dst_nb_samples, dst_sample_fmt, 1);
      if (ret < 0)
        break;
      max_dst_nb_samples = dst_nb_samples;
    }
    //        int fifo_size = swr_get_out_samples(swr_ctx,src_nb_samples);
    //        printf("fifo_size:%d\n", fifo_size);
    //        if(fifo_size < 1024)
    //            continue;

    /* convert to destination format */
    //        ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, (const
    //        uint8_t **)src_data, src_nb_samples);
    ret = swr_convert(swr_ctx, dst_data, dst_nb_samples,
                      (const uint8_t **)src_data, src_nb_samples);
    if (ret < 0) {
      fprintf(stderr, "Error while converting\n");
      goto end;
    }
    dst_bufsize = av_samples_get_buffer_size(
        &dst_linesize, dst_nb_channels.nb_channels, ret, dst_sample_fmt, 1);
    if (dst_bufsize < 0) {
      fprintf(stderr, "Could not get sample buffer size\n");
      goto end;
    }
    printf("t:%f in:%d out:%d\n", t, src_nb_samples, ret);
    fwrite(dst_data[0], 1, dst_bufsize, dst_file);
  } while (t < 10);

  ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, NULL, 0);
  if (ret < 0) {
    fprintf(stderr, "Error while converting\n");
    goto end;
  }
  dst_bufsize = av_samples_get_buffer_size(
      &dst_linesize, dst_nb_channels.nb_channels, ret, dst_sample_fmt, 1);
  if (dst_bufsize < 0) {
    fprintf(stderr, "Could not get sample buffer size\n");
    goto end;
  }
  printf("flush in:%d out:%d\n", 0, ret);
  fwrite(dst_data[0], 1, dst_bufsize, dst_file);

  if ((ret = get_format_from_sample_fmt(&fmt, dst_sample_fmt)) < 0)
    goto end;
  fprintf(stderr,
          "Resampling succeeded. Play the output file with the command:\n"
          "ffplay -f %s -channel_layout %" PRId64 " -channels %d -ar %d %s\n",
          fmt, dst_ch_layout, dst_nb_channels.nb_channels, dst_rate,
          dst_filename);

end:
  fclose(dst_file);

  if (src_data)
    av_freep(&src_data[0]);
  av_freep(&src_data);

  if (dst_data)
    av_freep(&dst_data[0]);
  av_freep(&dst_data);

  swr_free(&swr_ctx);
  return ret < 0;
}
