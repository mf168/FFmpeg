/*
 * IIDC1394 grab interface (uses libdc1394 and libraw1394)
 * Copyright (c) 2004 Roman Shaposhnik
 * Copyright (c) 2008 Alessandro Sappia
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "avdevice.h"

#include <dc1394/dc1394.h>

#undef free

typedef struct dc1394_data {
    AVClass *class;
    dc1394_t *d;
    dc1394camera_t *camera;
    dc1394video_frame_t *frame;
    int current_frame;
    int fps;

    AVPacket packet;
} dc1394_data;

struct dc1394_frame_format {
    int width;
    int height;
    enum PixelFormat pix_fmt;
    int frame_size_id;
} dc1394_frame_formats[] = {
    { 320, 240, PIX_FMT_UYVY422, DC1394_VIDEO_MODE_320x240_YUV422 },
    { 640, 480, PIX_FMT_UYYVYY411, DC1394_VIDEO_MODE_640x480_YUV411 },
    { 640, 480, PIX_FMT_UYVY422, DC1394_VIDEO_MODE_640x480_YUV422 },
    { 0, 0, 0, 0 } /* gotta be the last one */
};

struct dc1394_frame_rate {
    int frame_rate;
    int frame_rate_id;
} dc1394_frame_rates[] = {
    {  1875, DC1394_FRAMERATE_1_875 },
    {  3750, DC1394_FRAMERATE_3_75  },
    {  7500, DC1394_FRAMERATE_7_5   },
    { 15000, DC1394_FRAMERATE_15    },
    { 30000, DC1394_FRAMERATE_30    },
    { 60000, DC1394_FRAMERATE_60    },
    {120000, DC1394_FRAMERATE_120   },
    {240000, DC1394_FRAMERATE_240    },
    { 0, 0 } /* gotta be the last one */
};

static const AVOption options[] = {
#if HAVE_LIBDC1394_1
    { "channel", "", offsetof(dc1394_data, channel), FF_OPT_TYPE_INT, {.dbl = 0}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
#endif
    { NULL },
};

static const AVClass libdc1394_class = {
    .class_name = "libdc1394 indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


static inline int dc1394_read_common(AVFormatContext *c, AVFormatParameters *ap,
                                     struct dc1394_frame_format **select_fmt, struct dc1394_frame_rate **select_fps)
{
    dc1394_data* dc1394 = c->priv_data;
    AVStream* vst;
    struct dc1394_frame_format *fmt;
    struct dc1394_frame_rate *fps;
    enum PixelFormat pix_fmt = ap->pix_fmt == PIX_FMT_NONE ? PIX_FMT_UYVY422 : ap->pix_fmt; /* defaults */
    int width                = !ap->width ? 320 : ap->width;
    int height               = !ap->height ? 240 : ap->height;
    int frame_rate           = !ap->time_base.num ? 30000 : av_rescale(1000, ap->time_base.den, ap->time_base.num);

    for (fmt = dc1394_frame_formats; fmt->width; fmt++)
         if (fmt->pix_fmt == pix_fmt && fmt->width == width && fmt->height == height)
             break;

    for (fps = dc1394_frame_rates; fps->frame_rate; fps++)
         if (fps->frame_rate == frame_rate)
             break;

    if (!fps->frame_rate || !fmt->width) {
        av_log(c, AV_LOG_ERROR, "Can't find matching camera format for %s, %dx%d@%d:1000fps\n", avcodec_get_pix_fmt_name(pix_fmt),
                                                                                                width, height, frame_rate);
        goto out;
    }

    /* create a video stream */
    vst = av_new_stream(c, 0);
    if (!vst)
        goto out;
    av_set_pts_info(vst, 64, 1, 1000);
    vst->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    vst->codec->codec_id = CODEC_ID_RAWVIDEO;
    vst->codec->time_base.den = fps->frame_rate;
    vst->codec->time_base.num = 1000;
    vst->codec->width = fmt->width;
    vst->codec->height = fmt->height;
    vst->codec->pix_fmt = fmt->pix_fmt;

    /* packet init */
    av_init_packet(&dc1394->packet);
    dc1394->packet.size = avpicture_get_size(fmt->pix_fmt, fmt->width, fmt->height);
    dc1394->packet.stream_index = vst->index;
    dc1394->packet.flags |= AV_PKT_FLAG_KEY;

    dc1394->current_frame = 0;
    dc1394->fps = fps->frame_rate;

    vst->codec->bit_rate = av_rescale(dc1394->packet.size * 8, fps->frame_rate, 1000);
    *select_fps = fps;
    *select_fmt = fmt;
    return 0;
out:
    return -1;
}

static int dc1394_read_header(AVFormatContext *c, AVFormatParameters * ap)
{
    dc1394_data* dc1394 = c->priv_data;
    dc1394camera_list_t *list;
    int res, i;
    struct dc1394_frame_format *fmt = NULL;
    struct dc1394_frame_rate *fps = NULL;

    if (dc1394_read_common(c,ap,&fmt,&fps) != 0)
       return -1;

    /* Now let us prep the hardware. */
    dc1394->d = dc1394_new();
    dc1394_camera_enumerate (dc1394->d, &list);
    if ( !list || list->num == 0) {
        av_log(c, AV_LOG_ERROR, "Unable to look for an IIDC camera\n\n");
        goto out;
    }

    /* FIXME: To select a specific camera I need to search in list its guid */
    dc1394->camera = dc1394_camera_new (dc1394->d, list->ids[0].guid);
    if (list->num > 1) {
        av_log(c, AV_LOG_INFO, "Working with the first camera found\n");
    }

    /* Freeing list of cameras */
    dc1394_camera_free_list (list);

    /* Select MAX Speed possible from the cam */
    if (dc1394->camera->bmode_capable>0) {
       dc1394_video_set_operation_mode(dc1394->camera, DC1394_OPERATION_MODE_1394B);
       i = DC1394_ISO_SPEED_800;
    } else {
       i = DC1394_ISO_SPEED_400;
    }

    for (res = DC1394_FAILURE; i >= DC1394_ISO_SPEED_MIN && res != DC1394_SUCCESS; i--) {
            res=dc1394_video_set_iso_speed(dc1394->camera, i);
    }
    if (res != DC1394_SUCCESS) {
        av_log(c, AV_LOG_ERROR, "Couldn't set ISO Speed\n");
        goto out_camera;
    }

    if (dc1394_video_set_mode(dc1394->camera, fmt->frame_size_id) != DC1394_SUCCESS) {
        av_log(c, AV_LOG_ERROR, "Couldn't set video format\n");
        goto out_camera;
    }

    if (dc1394_video_set_framerate(dc1394->camera,fps->frame_rate_id) != DC1394_SUCCESS) {
        av_log(c, AV_LOG_ERROR, "Couldn't set framerate %d \n",fps->frame_rate);
        goto out_camera;
    }
    if (dc1394_capture_setup(dc1394->camera, 10, DC1394_CAPTURE_FLAGS_DEFAULT)!=DC1394_SUCCESS) {
        av_log(c, AV_LOG_ERROR, "Cannot setup camera \n");
        goto out_camera;
    }

    if (dc1394_video_set_transmission(dc1394->camera, DC1394_ON) !=DC1394_SUCCESS) {
        av_log(c, AV_LOG_ERROR, "Cannot start capture\n");
        goto out_camera;
    }
    return 0;

out_camera:
    dc1394_capture_stop(dc1394->camera);
    dc1394_video_set_transmission(dc1394->camera, DC1394_OFF);
    dc1394_camera_free (dc1394->camera);
out:
    dc1394_free(dc1394->d);
    return -1;
}

static int dc1394_read_packet(AVFormatContext *c, AVPacket *pkt)
{
    struct dc1394_data *dc1394 = c->priv_data;
    int res;

    /* discard stale frame */
    if (dc1394->current_frame++) {
        if (dc1394_capture_enqueue(dc1394->camera, dc1394->frame) != DC1394_SUCCESS)
            av_log(c, AV_LOG_ERROR, "failed to release %d frame\n", dc1394->current_frame);
    }

    res = dc1394_capture_dequeue(dc1394->camera, DC1394_CAPTURE_POLICY_WAIT, &dc1394->frame);
    if (res == DC1394_SUCCESS) {
        dc1394->packet.data = (uint8_t *)(dc1394->frame->image);
        dc1394->packet.pts = (dc1394->current_frame  * 1000000) / (dc1394->fps);
        res = dc1394->frame->image_bytes;
    } else {
        av_log(c, AV_LOG_ERROR, "DMA capture failed\n");
        dc1394->packet.data = NULL;
        res = -1;
    }

    *pkt = dc1394->packet;
    return res;
}

static int dc1394_close(AVFormatContext * context)
{
    struct dc1394_data *dc1394 = context->priv_data;

    dc1394_video_set_transmission(dc1394->camera, DC1394_OFF);
    dc1394_capture_stop(dc1394->camera);
    dc1394_camera_free(dc1394->camera);
    dc1394_free(dc1394->d);

    return 0;
}

AVInputFormat ff_libdc1394_demuxer = {
    .name           = "libdc1394",
    .long_name      = NULL_IF_CONFIG_SMALL("dc1394 A/V grab"),
    .priv_data_size = sizeof(struct dc1394_data),
    .read_header    = dc1394_read_header,
    .read_packet    = dc1394_read_packet,
    .read_close     = dc1394_close,
    .flags          = AVFMT_NOFILE
    .priv_class     = &libdc1394_class,
};
