
/**
* @projectName   08-09-encode_video_libx264_nvenc
* @brief         主要测试
*               （1）libx264对于不同preset的编码质量和编码耗时；
*               （2）h264_nvenc硬件编码不同preset的编码质量和编码耗时；
*               （3）libx264和h264_nvenc的编码耗时对比
* @author        Liao Qingfu
* @date          2020-04-16
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

/*
一定要注意编码器timebase的设置，它和码率的计算有对应的关系，因为他需要这个时间关系

*/

// H264编码设置的参数
// 宽高
// 帧率
// gop
// b帧数量
typedef struct video_config
{
    int width;  // 宽
    int height; // 高
    AVRational framerate; // 帧率
    int gop_size;  // I帧间隔
    int max_b_frames;
    enum AVPixelFormat pix_fmt; // 像素格式
    AVRational time_base; // timebase
    char *preset;
    char *tune;
    char *profile;
    int64_t rc_max_rate;  // 最大码率
    int64_t rc_min_rate;  // 最低码率
    int rc_buffer_size; // decoder bitstream buffer size
    int qmin; // minimum quantizer
    int qmax; // maximum quantizer
    int flags; // AV_CODEC_FLAG_*.比如AV_CODEC_FLAG_GLOBAL_HEADER
    int64_t bit_rate;
}VideoConfgig;

int64_t get_time()
{
    return av_gettime_relative() / 1000;  // 换算成毫秒
}
VideoConfgig *video_encode_new_default_config(const char *codec_name, int width, int height, int fps)
{
    VideoConfgig *video_config = malloc(sizeof(VideoConfgig));
    if(!video_config)
    {
        printf("malloc VideoConfgig failed\n");
        return NULL;
    }
    memset(video_config, 0, sizeof(VideoConfgig));
    video_config->gop_size = fps;
    video_config->width = width;
    video_config->height = height;
    if(strcmp(codec_name, "h264_nvenc") == 0)
        video_config->time_base = (AVRational){1, fps};   // 注意匹配, 为什么这个参数影响硬件编码呢？因为nvenc帧率设置实际是用的这个代码
    else
        video_config->time_base = (AVRational){1, 1000};  // 其他的我们用timebase 1000为单位
    video_config->framerate = (AVRational){fps, 1};
    video_config->pix_fmt = AV_PIX_FMT_YUV420P;
    video_config->max_b_frames = 0;
    video_config->bit_rate = 1*1024*1024;// ffmpeg的单位就是bps
    return video_config;
}

// 配置参数
static int video_encode_config(VideoConfgig *video_config, AVCodecContext *enc_ctx)
{
    int ret = 0;
    if(!video_config || !enc_ctx) {
        return -1;
    }
    /* 设置分辨率*/
    enc_ctx->width = video_config->width;
    enc_ctx->height = video_config->height;
    /* 设置time base */

    enc_ctx->time_base = video_config->time_base;
    enc_ctx->framerate = video_config->framerate;
    /* 设置I帧间隔
     * 如果frame->pict_type设置为AV_PICTURE_TYPE_I, 则忽略gop_size的设置，一直当做I帧进行编码
     */
    enc_ctx->gop_size = video_config->gop_size;
    enc_ctx->max_b_frames = video_config->max_b_frames;
    enc_ctx->pix_fmt = video_config->pix_fmt;
    // refs 可以设置参考帧的最大数量，这里主要是对h264_nvenc, 单路设置的时候对其编码时间没有影响，单纯设置只是对sps pps影响
//    enc_ctx->refs = 1;

    //
#if 1
    // 相关的参数可以参考libx264.c的 AVOption options
    // ultrafast all encode time:2270ms
    // medium all encode time:5815ms
    // veryslow all encode time:19836ms
    if(video_config->preset) {
        ret = av_opt_set(enc_ctx->priv_data, "preset", video_config->preset, 0);
        if(ret != 0) {
            printf("av_opt_set preset failed\n");
        }
    }
//    if(video_config->tune)
    {
        ret = av_opt_set(enc_ctx->priv_data, "tune","zerolatency", 0); // 直播是才使用该设置
        if(ret != 0) {
            printf("av_opt_set tune failed\n");
        }
    }

    if(video_config->profile) {
        ret = av_opt_set(enc_ctx->priv_data, "profile", "main", 0); // 默认是high
        if(ret != 0) {
            printf("av_opt_set profile failed\n");
        }
    }

#endif

    // 设置码率 
    enc_ctx->bit_rate = video_config->bit_rate;
//    enc_ctx->bit_rate_tolerance = enc_ctx->bit_rate*2; // 码率误差，允许的误差越大，视频越小


    return 0;
}

static int encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt,
                  FILE *outfile, int *bytes_count, int write_enable)
{
    int ret;

    /* send the frame to the encoder */

//    if(frame)
//        printf("Send frame %3"PRId64"\n", frame->pts);
    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0)
    {
        fprintf(stderr, "Error sending a frame for encoding\n");
        return -1;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error encoding audio frame\n");
            return -1;
        }
//        printf("pkt  pts:%3"PRId64", dts: %3"PRId64"\n",pkt->pts, pkt->dts);
        *bytes_count += pkt->size;
        if(write_enable)
            fwrite(pkt->data, 1, pkt->size, outfile);
    }
    return 0;
}

char *alloc_and_copy_string(char *str)
{
    if(!str) {
        printf("str is null\n");
        return NULL;
    }
    char *alloc_str = malloc(strlen(str) + 1);
    strcpy(alloc_str, str);
}

int video_encode_preset_test(const char *codec_name, char *in_yuv_file, char *out_h264_file, char *preset, int write_enable)
{
    FILE *infile = NULL;
    FILE *outfile = NULL;


    const AVCodec *codec = NULL;
    AVCodecContext *codec_ctx= NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int ret = 0;
    int bytes_count = 0;
    int frame_count = 0;


    /* 查找指定的编码器 */
    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        fprintf(stderr, "Codec '%s' not found\n", codec_name);
        exit(1);
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    VideoConfgig *video_config = video_encode_new_default_config(codec_name, 1280, 720, 25);
    video_config->preset = alloc_and_copy_string(preset);

    video_encode_config(video_config, codec_ctx);  // 设置
    /* 将codec_ctx和codec进行绑定 */
    ret = avcodec_open2(codec_ctx, codec,NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
        exit(1);
    }
    printf("thread_count: %d, thread_type:%d\n", codec_ctx->thread_count, codec_ctx->thread_type);
    // 打开输入和输出文件
    infile = fopen(in_yuv_file, "rb");
    if (!infile) {
        fprintf(stderr, "Could not open %s\n", in_yuv_file);
        exit(1);
    }
    outfile = fopen(out_h264_file, "wb");
    if (!outfile) {
        fprintf(stderr, "Could not open %s\n", out_h264_file);
        exit(1);
    }

    // 分配pkt和frame
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    // 为frame分配buffer
    frame->format = codec_ctx->pix_fmt;
    frame->width  = codec_ctx->width;
    frame->height = codec_ctx->height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        exit(1);
    }
    // 计算出每一帧的数据 像素格式 * 宽 * 高
    // 1382400
    int frame_bytes = av_image_get_buffer_size(frame->format, frame->width,
                                               frame->height, 1);
//    printf("frame_bytes %d\n", frame_bytes);
    uint8_t *yuv_buf = (uint8_t *)malloc(frame_bytes);
    if(!yuv_buf) {
        printf("yuv_buf malloc failed\n");
        return 1;
    }
    int64_t begin_time = get_time();        // 起始时间
    int64_t all_begin_time = get_time();
    int64_t all_end_time = all_begin_time;
    int64_t cur_pts = 0;

    for (;;) {
        memset(yuv_buf, 0, frame_bytes);
        size_t read_bytes = fread(yuv_buf, 1, frame_bytes, infile);
        if(read_bytes <= 0) {
//            printf("read file finish\n");
            break;
        }
        frame_count++; // 帧数加一
        /* 确保该frame可写, 如果编码器内部保持了内存参考计数，则需要重新拷贝一个备份
            目的是新写入的数据和编码器保存的数据不能产生冲突
        */
        ret = av_frame_make_writable(frame);
        if(ret != 0) {
            printf("av_frame_make_writable failed, ret = %d\n", ret);
            break;
        }
        int need_size = av_image_fill_arrays(frame->data, frame->linesize, yuv_buf,
                                             frame->format,
                                             frame->width, frame->height, 1);
        if(need_size != frame_bytes) {
            printf("av_image_fill_arrays failed, need_size:%d, frame_bytes:%d\n",
                   need_size, frame_bytes);
            break;
        }
        if(strcmp(codec_name, "h264_nvenc") == 0)
            cur_pts += 1;  // 25/25
        else
            cur_pts += 40; // 1000/25

        // 设置pts
        frame->pts = cur_pts;       // 使用采样率作为pts的单位，具体换算成秒 pts*1/采样率
        begin_time = get_time();
        ret = encode(codec_ctx, frame, pkt, outfile, &bytes_count, write_enable);
        if(ret < 0) {
            printf("encode failed\n");
            break;
        }
    }

    /* 冲刷编码器 */
    encode(codec_ctx, NULL, pkt, outfile, &bytes_count, write_enable);
    all_end_time = get_time();
    float bps = (bytes_count * 8.0) /(frame_count/25.0)/1024; // 按25帧每秒计算
    printf("%s %s encode time:%ldms\n", codec_name, preset, all_end_time - all_begin_time);
    printf("%s %s encode bps:%0.2fkbps\n", codec_name, preset, bps);
    // 关闭文件
    fclose(infile);
    fclose(outfile);

    // 释放内存
    if(yuv_buf) {
        free(yuv_buf);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    if(video_config) {
        if(video_config->preset)
            free(video_config->preset);
        free(video_config);
    }

    return 0;
}
/**
 * @brief 提取测试文件：ffmpeg -i big_buck_bunny_720p_10mb.mp4 -t 10 -r 25 -pix_fmt yuv420p 1280x720_25fps_10s.yuv
 *       输入参数，输入yuv文件的路径，比如 1280x720_25fps_10s.yuv 1280x720_25fps_10s 1
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv)
{
    char *in_yuv_file = NULL;
    char out_h264_file[128]={0};
    int write_enable = 1;
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input_file out_file write_enable>, argc:%d\n",
                argv[0], argc);
        return 0;
    }
    in_yuv_file = argv[1];      // 输入YUV文件
    if(argc >= 4) {
        write_enable = atoi(argv[3]);
    }

    //    const char *codec_name = "libx264";
//        const char *codec_name = "h264_nvenc";
   /* 测试每种preset的耗时
    ultrafast
    superfast
    veryfast
    faster
    fast
    medium
    slow
    slower
    veryslow
    placebo
    */
#if 1
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "ultrafast", "libx264");
//    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "ultrafast", write_enable);
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "ultrafast", "h264_nvenc");
//    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "ultrafast", write_enable); // nvenc不支持

//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "superfast", "libx264");
//    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "superfast", write_enable);
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "superfast", "h264_nvenc");
//    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "superfast", write_enable);  // nvenc不支持


//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "faster", "libx264");
//    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "faster", write_enable);
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "faster", "h264_nvenc");
//    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "faster", write_enable);   // nvenc不支持

    printf("\n\n--fast--------\n");
    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "fast", "libx264");
    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "fast", write_enable);
    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "fast", "h264_nvenc");
    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "fast", write_enable);


    printf("\n\n--medium--------\n");
    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "medium", "libx264");
    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "medium", write_enable);
    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "medium", "h264_nvenc");
    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "medium", write_enable);

    printf("\n\n--slow--------\n");
    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "slow", "libx264");
    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "slow", write_enable);
    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "slow", "h264_nvenc");
    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "slow", write_enable);

//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "slower", "libx264");
//    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "slower", write_enable);
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "slower", "h264_nvenc");
//    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "slower", write_enable);   // nvenc不支持


//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "veryslow", "libx264");
//    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "veryslow", write_enable);
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "veryslow", "h264_nvenc");
//    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "veryslow", write_enable);  // nvenc不支持


//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "placebo", "libx264");
//    video_encode_preset_test("libx264", in_yuv_file, out_h264_file, "placebo", write_enable);
//    sprintf(out_h264_file, "%s_%s_%s.h264",  argv[2], "placebo", "h264_nvenc");
//    video_encode_preset_test("h264_nvenc", in_yuv_file, out_h264_file, "placebo", write_enable);// nvenc不支持
    #endif
}

#if 0
libx264 ultrafast encode time:306ms
libx264 ultrafast encode bps:1027.18kbps

h264_nvenc ultrafast encode time:278ms
h264_nvenc ultrafast encode bps:906.10kbps


libx264 superfast encode time:381ms
libx264 superfast encode bps:1084.67kbps

h264_nvenc superfast encode time:226ms
h264_nvenc superfast encode bps:906.10kbps


libx264 faster encode time:800ms
libx264 faster encode bps:928.86kbps

h264_nvenc faster encode time:219ms
h264_nvenc faster encode bps:906.10kbps

h264_nvenc fast encode time:223ms
h264_nvenc fast encode bps:923.33kbps

libx264 medium encode time:1075ms
libx264 medium encode bps:928.75kbps

thread_count: 1, thread_type:3
h264_nvenc medium encode time:279ms
h264_nvenc medium encode bps:906.10kbps


thread_count: 0, thread_type:0
libx264 slow encode time:1852ms
libx264 slow encode bps:928.24kbps

thread_count: 1, thread_type:3
h264_nvenc slow encode time:456ms
h264_nvenc slow encode bps:921.65kbps

libx264 slower encode time:3017ms
libx264 slower encode bps:925.25kbps

thread_count: 1, thread_type:3
h264_nvenc slower encode time:276ms
h264_nvenc slower encode bps:906.10kbps


libx264 veryslow encode time:4748ms
libx264 veryslow encode bps:920.88kbps

h264_nvenc veryslow encode time:289ms
h264_nvenc veryslow encode bps:906.10kbps


libx264 placebo encode time:17834ms
libx264 placebo encode bps:923.30kbps

thread_count: 1, thread_type:3
h264_nvenc placebo encode time:282ms
h264_nvenc placebo encode bps:906.10kbps

#endif
