#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>

#include <libavutil/timestamp.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "segment.h"
#include "buffer.h"
#include "publisher.h"


struct ReadInfo {
    struct PublisherContext *pub;
    char *in_filename;
};

struct WriteInfo {
    struct PublisherContext *pub;
};

struct AcceptInfo {
    struct PublisherContext *pub;
    AVFormatContext *ifmt_ctx;
    const char *out_uri;
};

void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}


void *read_thread(void *arg)
{
    struct ReadInfo *info = (struct ReadInfo*) arg;
    AVFormatContext *ifmt_ctx = NULL;
    char *in_filename = info->in_filename;
    int ret;
    int i;
    int video_idx;
    int id = 0;
    struct Segment *seg = NULL;
    int64_t pts, now, start;
    AVPacket pkt;
    AVStream *in_stream;
    AVRational tb;
    tb.num = 1;
    tb.den = AV_TIME_BASE;


    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0))) {
        fprintf(stderr, "Could not open stdin\n");
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "Could not get input stream info\n");
        goto end;
    }


    printf("Finding video stream.\n");

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        printf("Checking stream %d\n", i);
        AVStream *stream = ifmt_ctx->streams[i];
        printf("Got stream\n");
        AVCodecContext *avctx = avcodec_alloc_context3(NULL);
        if (!avctx)
            return NULL;
        ret = avcodec_parameters_to_context(avctx, stream->codecpar);
        if (ret < 0) {
            return NULL;
        }
        AVCodecParameters *params = stream->codecpar;
        printf("Got params\n");
        // Segfault here ↓
        enum AVMediaType type = params->codec_type;
        //if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        printf("Got type\n");
        if (type == AVMEDIA_TYPE_VIDEO) {
            video_idx = i;
            break;
        }
    }
    start = av_gettime_relative();

    for (;;) {
        //printf("Reading packet\n");
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) {
            break;
        }
        in_stream = ifmt_ctx->streams[pkt.stream_index];
        if (pkt.pts == AV_NOPTS_VALUE) {
            pkt.pts = 0;
        }
        if (pkt.dts == AV_NOPTS_VALUE) {
            pkt.dts = 0;
        }
        pts = av_rescale_q(pkt.pts, in_stream->time_base, tb);
        now = av_gettime_relative() - start;

        //log_packet(ifmt_ctx, &pkt);
        while (pts > now) {
            usleep(1000);
            now = av_gettime_relative() - start;
        }

        if ((pkt.flags & AV_PKT_FLAG_KEY && pkt.stream_index == video_idx) || !seg) {
            if (seg) {
                char filename[100];
                segment_close(seg);
                sprintf(filename, "segment-%03d.mkv", id);
                save_segment(seg, filename);
                buffer_push_segment(info->pub->buffer, seg);
                publish(info->pub);
                printf("New segment pushed.\n");
            }
            printf("starting new segment");
            segment_init(&seg, ifmt_ctx);
            seg->id = id++;
            printf(" id = %d\n", id);
        }
        //printf("writing frame\n");
        segment_ts_append(seg, pkt.dts, pkt.pts);
        ret = av_write_frame(seg->fmt_ctx, &pkt);
        if (ret < 0) {
            printf("write frame failed\n");
        }

    }
    segment_close(seg);

end:

    avformat_close_input(&ifmt_ctx);
    printf("Freed buffer\n");


    /* close output */

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
    }

    return NULL;
}

void write_segment(struct Client *c)
{
    struct Segment *seg = buffer_peek_segment(c->buffer);
    int ret;
    int pkt_count;
    if (seg) {
        AVFormatContext *fmt_ctx;
        AVIOContext *avio_ctx;
        AVPacket pkt;
        struct AVIOContextInfo info;
        printf("Writing segment, size: %zu\n", seg->size);
        buffer_set_state(c->buffer, BUSY);
        info.buf = seg->buf;
        info.left = seg->size;

        if (!(fmt_ctx = avformat_alloc_context())) {
            ret = AVERROR(ENOMEM);
            printf("NOMEM\n");
            return;
        }

        avio_ctx = avio_alloc_context(c->avio_buffer, AV_BUFSIZE, 0, &info, &segment_read, NULL, NULL);

        fmt_ctx->pb = avio_ctx;
        ret = avformat_open_input(&fmt_ctx, NULL, seg->ifmt, NULL);
        if (ret < 0) {
            fprintf(stderr, "Could not open input\n");
            return;
        }
        ret = avformat_find_stream_info(fmt_ctx, NULL);
        if (ret < 0) {
            fprintf(stderr, "Could not find stream information\n");
            return;
        }

        for (;;) {
            ret = av_read_frame(fmt_ctx, &pkt);
            if (ret < 0) {
                break;
            }
            printf("read frame\n");
            pkt.dts = seg->ts[pkt_count];
            pkt.pts = seg->ts[pkt_count + 1];
            pkt_count += 2;
            //log_packet(fmt_ctx, &pkt);
            av_write_frame(c->ofmt_ctx, &pkt);
            printf("wrote frame to client\n");
        }

        buffer_drop_segment(c->buffer);
        buffer_set_state(c->buffer, WRITABLE);
    } else {
        buffer_set_state(c->buffer, WAIT);
    }
}

void *accept_thread(void *arg)
{
    struct AcceptInfo *info = (struct AcceptInfo*) arg;
    const char *out_uri = info->out_uri;
    AVIOContext *client;
    AVIOContext *server = NULL;
    AVFormatContext *ofmt_ctx;
    AVOutputFormat *ofmt;
    AVDictionary *options = NULL;
    AVDictionary *mkvoptions = NULL;
    AVStream *in_stream, *out_stream;
    AVCodecContext *codec_ctx;
    int ret, i, reply_code;

    if ((ret = av_dict_set(&options, "listen", "2", 0)) < 0) {
        fprintf(stderr, "Failed to set listen mode for server: %s\n", av_err2str(ret));
        return NULL;
    }

    if ((ret = av_dict_set_int(&options, "listen_timeout", 500, 0)) < 0) {
        fprintf(stderr, "Failed to set listen timeout for server: %s\n", av_err2str(ret));
        return NULL;
    }

    if ((ret = av_dict_set_int(&options, "timeout", 20000, 0)) < 0) {
        fprintf(stderr, "Failed to set listen timeout for server: %s\n", av_err2str(ret));
        return NULL;
    }

    if ((ret = avio_open2(&server, out_uri, AVIO_FLAG_WRITE, NULL, &options)) < 0) {
        fprintf(stderr, "Failed to open server: %s\n", av_err2str(ret));
        return NULL;
    }

    for (;;) {
/*        if (buffer->eos)
            break; */
        reply_code = 200;
        printf("Accepting new clients...\n");
        client = NULL;
        if ((ret = avio_accept(server, &client)) < 0) {
            printf("Error or timeout\n");
            printf("ret: %d\n", ret);
            continue;
        }
        printf("No error or timeout\n");
        printf("ret: %d\n", ret);


        // Append client to client list
        client->seekable = 0;
        if ((ret = av_dict_set(&mkvoptions, "live", "1", 0)) < 0) {
            fprintf(stderr, "Failed to set live mode for matroska: %s\n", av_err2str(ret));
            return NULL;
        }
        if (publisher_reserve_client(info->pub)) {
            printf("No more slots free\n");
            reply_code = 503;
        }
        if ((ret = av_opt_set_int(client, "reply_code", reply_code, AV_OPT_SEARCH_CHILDREN)) < 0) {
            av_log(client, AV_LOG_ERROR, "Failed to set reply_code: %s.\n", av_err2str(ret));
            continue;
        }
        while ((ret = avio_handshake(client)) > 0);
        if (ret < 0) {
            avio_close(client);
            continue;
        }

        if (reply_code == 503) {
            avio_close(client);
            continue;
        }

        avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", NULL);

        if (!ofmt_ctx) {
            fprintf(stderr, "Could not create output context\n");
            continue;
        }
        ofmt_ctx->flags |= AVFMT_FLAG_GENPTS;
        ofmt = ofmt_ctx->oformat;
        ofmt->flags &= AVFMT_NOFILE;

        for (i = 0; i < info->ifmt_ctx->nb_streams; i++)
        {
            in_stream = info->ifmt_ctx->streams[i];
            codec_ctx = avcodec_alloc_context3(NULL);
            avcodec_parameters_to_context(codec_ctx, in_stream->codecpar);
            out_stream = avformat_new_stream(ofmt_ctx, codec_ctx->codec);
            //avcodec_parameters_to_context(out_stream->codec, in_stream->codecpar);
            if (!out_stream) {
                fprintf(stdout, "Failed allocating output stream\n");
                continue;
            }
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
                continue;
            }
            av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
            printf("Allocated output stream.\n");
            /*out_stream->codec->codec_tag = 0;
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; */
        }

        ofmt_ctx->pb = client;
        ret = avformat_write_header(ofmt_ctx, &mkvoptions);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when opening output file\n");
            continue;
        }
        publisher_add_client(info->pub, ofmt_ctx);
        printf("Accepted new client!\n");

    }

    return NULL;
}


void *write_thread(void *arg)
{
    struct WriteInfo *info = (struct WriteInfo*) arg;
    int i;
    struct Client *c;
    for (;;) {
        usleep(500000);
        printf("Checking clients\n");
        for (i = 0; i < MAX_CLIENTS; i++) {
            client_print(c);
            c = &info->pub->subscribers[i];
            switch(c->buffer->state) {
            case WRITABLE:
                write_segment(c);
            default:
                continue;
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    struct ReadInfo rinfo;
    struct AcceptInfo ainfo;
    struct WriteInfo winfo;
    struct PublisherContext *pub;
    int ret;
    pthread_t r_thread, a_thread, w_thread;

    AVFormatContext *ifmt_ctx = NULL;

    av_register_all();
    avformat_network_init();

    publisher_init(&pub);
    rinfo.in_filename = argv[1];
    rinfo.pub = pub;

    if ((ret = avformat_open_input(&ifmt_ctx, argv[1], 0, 0))) {
        fprintf(stderr, "Could not open stdin\n");
        return 1;
    }

    ainfo.out_uri = "http://127.0.0.1:8080";
    ainfo.ifmt_ctx = ifmt_ctx;
    ainfo.pub = pub;

    winfo.pub = pub;

    //pthread_create(&a_thread, NULL, accept_thread, &ainfo);
    pthread_create(&w_thread, NULL, write_thread, &winfo);
    pthread_create(&r_thread, NULL, read_thread, &rinfo);
    //write_thread(&winfo);
    accept_thread(&ainfo);
    //read_thread(&rinfo);

    publisher_free(pub);
    free(pub->buffer);
    free(pub);
    return 0;

}