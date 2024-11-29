/**
* @projectName   02-encode_h265
* @brief         视频编码，从本地读取YUV数据进行H265编码
* @author        Liao Qingfu
* @date          2022-09-16
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#define ENCODE_TIME_BASE 1000   // 设置时间基数，编码需要根据时间来判断码率
#define ENCODE_FRAME_RATE 25   // 设置帧率
#define YUV_WIDTH 1280
#define YUV_HEIGH 720
void print_h265_nal_unit_type(uint8_t *data, size_t size);
int64_t get_time()
{
    return av_gettime_relative() / 1000;  // 换算成毫秒
}
static int encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt,
                  FILE *outfile)
{
    int ret;

    /* send the frame to the encoder */
    if (frame)
        printf("send frame pts:%3"PRId64"\n", frame->pts);
    /* 通过查阅代码，使用x264进行编码时，具体缓存帧是在x264源码进行，
     * 不会增加avframe对应buffer的reference*/
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

        printf("avcodec_receive_packet:pts:%3"PRId64" dts:%3"PRId64" (size:%5d)\n", pkt->pts, pkt->dts, pkt->size);
        fwrite(pkt->data, 1, pkt->size, outfile); 
		//print_h265_nal_unit_type(pkt->data, pkt->size);  // 只针对H265
    }
    return 0;
}
/**
 * @brief 提取测试文件：ffmpeg -i test_1280x720.flv -t 5 -r 25 -pix_fmt yuv420p yuv420p_1280x720.yuv
 *           参数输入: yuv420p_1280x720.yuv yuv420p_1280x720.h265 libx265
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv)
{
    char *in_yuv_file = NULL;
    char *out_h264_h265_file = NULL;
    FILE *infile = NULL;
    FILE *outfile = NULL;

    const char *codec_name = NULL;
    const AVCodec *codec = NULL;
    AVCodecContext *codec_ctx= NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int ret = 0;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <input_file out_file codec_name >, argc:%d\n",
                argv[0], argc);
        return 0;
    }
    in_yuv_file = argv[1];      // 输入YUV文件
    out_h264_h265_file = argv[2];
    codec_name = argv[3];

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


    /* 设置分辨率*/
    codec_ctx->width = YUV_WIDTH;        // 根据实际去写入
    codec_ctx->height = YUV_HEIGH;
    /* 设置time base */
    codec_ctx->time_base = (AVRational){1, ENCODE_TIME_BASE};  // 1/1000
    codec_ctx->framerate = (AVRational){ENCODE_FRAME_RATE, 1};
    /* 设置I帧间隔
     * 如果frame->pict_type设置为AV_PICTURE_TYPE_I, 则忽略gop_size的设置，一直当做I帧进行编码
     */
    codec_ctx->gop_size = 25;   // I帧间隔, H265单独设置这里不起作用

//    codec_ctx->keyint_min = 25;
    codec_ctx->max_b_frames = 0; // 如果不想包含B帧则设置为0
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    //
    if (codec->id == AV_CODEC_ID_H264) {
        // 相关的参数可以参考libx264.c的 AVOption options
        // ultrafast all encode time:2270ms
        // medium all encode time:5815ms
        // veryslow all encode time:19836ms
        ret = av_opt_set(codec_ctx->priv_data, "preset", "medium", 0);
        if(ret != 0) {
            printf("av_opt_set preset failed\n");
        }
        ret = av_opt_set(codec_ctx->priv_data, "profile", "main", 0); // 默认是high
        if(ret != 0) {
            printf("av_opt_set profile failed\n");
        }
        ret = av_opt_set(codec_ctx->priv_data, "tune","zerolatency",0); // 直播是才使用该设置
//        ret = av_opt_set(codec_ctx->priv_data, "tune","film",0); //  画质film
        if(ret != 0) {
            printf("av_opt_set tune failed\n");
        }
    }else if (codec->id == AV_CODEC_ID_H265) {
        // 相关的参数可以参考libx265.c的 AVOption options
        // ultrafast all encode time:
        // medium all encode time:
        // veryslow all encode time:
        ret = av_opt_set(codec_ctx->priv_data, "preset", "medium", 0);
        if(ret != 0) {
            printf("av_opt_set preset failed\n");
        }
        ret = av_opt_set(codec_ctx->priv_data, "profile", "main", 0); // 默认是high
        if(ret != 0) {
            printf("av_opt_set profile failed\n");
        }
        ret = av_opt_set(codec_ctx->priv_data, "tune","zerolatency",0); // 直播是才使用该设置
//        ret = av_opt_set(codec_ctx->priv_data, "tune","film",0); //  画质film
        if(ret != 0) {
            printf("av_opt_set tune failed\n");
        }
    // libx265/source\common\param.cpp
//        ret = av_opt_set(codec_ctx->priv_data, "x265-param", "--keyint=25, --bframes=2",0);
//        ret = av_opt_set(codec_ctx->priv_data, "x265-params", "--keyint=25:--frame-threads=4", 0);
//        ret = av_opt_set(codec_ctx->priv_data, "x265-params", "--keyint=25:--bframes=2", 0);
        ret = av_opt_set(codec_ctx->priv_data, "x265-params", "--keyint=25:--frame-threads=4", 0);
        if(ret != 0) {
            printf("av_opt_set x265-param failed\n");
            return -1;
        }
    } else {
        printf("no support the codec :%s\n", codec_name);
        return -1;
    }
    /*
     * 设置编码器参数
    */
    /* 设置bitrate */
    codec_ctx->bit_rate = 3000000; //3000k
//    codec_ctx->rc_max_rate = 3000000;
//    codec_ctx->rc_min_rate = 3000000;
//    codec_ctx->rc_buffer_size = 2000000;
//    codec_ctx->thread_count = 4;  // 开了多线程后也会导致帧输出延迟, 需要缓存thread_count帧后再编程。
//    codec_ctx->thread_type = FF_THREAD_FRAME; // 并 设置为FF_THREAD_FRAME
    /* 对于H264 AV_CODEC_FLAG_GLOBAL_HEADER  设置则只包含I帧，此时sps pps需要从codec_ctx->extradata读取
     *  不设置则每个I帧都带 sps pps sei
     */
//    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // 存本地文件时不要去设置

    /* 将codec_ctx和codec进行绑定 */
    ret = avcodec_open2(codec_ctx, codec, NULL);
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
    outfile = fopen(out_h264_h265_file, "wb");
    if (!outfile) {
        fprintf(stderr, "Could not open %s\n", out_h264_h265_file);
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
    printf("frame_bytes %d\n", frame_bytes);
    uint8_t *yuv_buf = (uint8_t *)malloc(frame_bytes);
    if(!yuv_buf) {
        printf("yuv_buf malloc failed\n");
        return 1;
    }
    int64_t begin_time = get_time();
    int64_t end_time = begin_time;
    int64_t all_begin_time = get_time();
    int64_t all_end_time = all_begin_time;
    int64_t pts = 0;
    printf("start enode\n");
    for (;;) {
        memset(yuv_buf, 0, frame_bytes);
        size_t read_bytes = fread(yuv_buf, 1, frame_bytes, infile);
        if(read_bytes <= 0) {
            printf("read file finish\n");
            break;
        }
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
         pts += (ENCODE_TIME_BASE/ENCODE_FRAME_RATE);
        // 设置pts
        frame->pts = pts;       // 使用采样率作为pts的单位，具体换算成秒 pts*1/采样率
        begin_time = get_time();
        ret = encode(codec_ctx, frame, pkt, outfile);
        end_time = get_time();
        printf("encode time:%ldms\n", end_time - begin_time);
        if(ret < 0) {
            printf("encode failed\n");
            break;
        }
    }

    /* 冲刷编码器 */
    encode(codec_ctx, NULL, pkt, outfile);
    all_end_time = get_time();
    printf("all encode time:%ldms\n", all_end_time - all_begin_time);
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

    printf("main finish, please enter Enter and exit\n");
    getchar();
    return 0;
}

void print_h265_nal_unit_type(uint8_t *data, size_t size)
{
    int i = 0;
    while (i+3 < size ) {
        if(data[i] == 0 && data[i+1]==0 && data[i+2] == 0 && data[i+3] == 1 ) {
            i += 4;
            printf("%02x nal_type:%d, pos:%d\n", data[i],(data[i]&0x7e)>>1, i);
            continue;
        }
        if(data[i] == 0 && data[i+1]==0 && data[i+2] == 1) {
            i += 3;
            printf("%02x nal_type:%d, pos:%d\n",data[i], (data[i]&0x7e)>>1, i);
            continue;
        }
        i++;
    }
}
