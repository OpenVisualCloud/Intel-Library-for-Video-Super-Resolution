/*
 * Intel Library for Video Super Resolution ffmpeg plugin
 *
 * Copyright (c) 2023 Intel Corporation
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
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "raisr/RaisrHandler.h"
#include "raisr/RaisrDefaults.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "internal.h"
#include "opencl.h"
#include "libavutil/pixdesc.h"
#include "video.h"

#define MIN_RATIO 1
#define MAX_RATIO 2
#define DEFAULT_RATIO 2

typedef struct RaisrOpenCLContext {
    OpenCLFilterContext ocf;

    int initialised;
    float ratio;
    int bits;
    char *filterfolder;
    BlendingMode blending;
    int passes;
    int mode;
    RangeType range;
    enum AVPixelFormat sw_format;
    int evenoutput;
} RaisrOpenCLContext;


static int raisr_opencl_init(AVFilterContext *avctx)
{
    RaisrOpenCLContext *ctx = avctx->priv;
    RNLERRORTYPE err;

    err = RNLHandler_SetOpenCLContext(ctx->ocf.hwctx->context, ctx->ocf.hwctx->device_id, 0, 0);
    if (err != RNLErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "RNLHandler_SetExternalOpenCLContext failed\n");
        return AVERROR(ENAVAIL);
    }

    err = RNLHandler_Init(ctx->filterfolder, ctx->ratio, ctx->bits, ctx->range, 1,
                          OpenCLExternal, ctx->passes, ctx->mode);
    if (err != RNLErrorNone) {
        av_log(avctx, AV_LOG_ERROR, "RNLInit failed\n");
        return AVERROR(ENAVAIL);
    }
    return 0;
}

static int raisr_opencl_filter_frame(AVFilterLink *inlink, AVFrame *input)
{
    AVFilterContext    *avctx = inlink->dst;
    AVFilterLink     *outlink = avctx->outputs[0];
    RaisrOpenCLContext *ctx = avctx->priv;
    AVFrame *output = NULL;
    const AVPixFmtDescriptor *desc;
    int err, wsub, hsub;
    int nb_planes = 0;
    VideoDataType vdt_in[3] = { 0 };
    VideoDataType vdt_out[3] = { 0 };

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(input->format),
           input->width, input->height, input->pts);

    if (!input->hw_frames_ctx)
        return AVERROR(EINVAL);

    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    desc = av_pix_fmt_desc_get(ctx->sw_format);
    if (!desc) {
        err = AVERROR(EINVAL);
        goto fail;
    }

    for(int p = 0; p < desc->nb_components; p++)
        if (desc->comp[p].plane > nb_planes)
            nb_planes = desc->comp[p].plane;

    for (int p = 0; p <= nb_planes; p++) {
        wsub = p ? 1 << desc->log2_chroma_w : 1;
        hsub = p ? 1 << desc->log2_chroma_h : 1;
        vdt_in[p].pData = input->data[p];
        vdt_in[p].width = input->width / wsub;
        vdt_in[p].height = input->height / hsub;
        vdt_in[p].step = input->linesize[p];
        vdt_in[p].bitShift = desc->comp[p].shift;
        // fill in the output video data type structure
        vdt_out[p].pData = output->data[p];
        vdt_out[p].width = output->width / wsub;
        vdt_out[p].height = output->height / hsub;
        vdt_out[p].step = output->linesize[p];
        vdt_out[p].bitShift = desc->comp[p].shift;
    }

    if (!ctx->initialised) {
        err = RNLHandler_SetRes(&vdt_in[0], &vdt_in[1], &vdt_in[2],
                                &vdt_out[0], &vdt_out[1], &vdt_out[2]);
        if (err != RNLErrorNone) {
            av_log(ctx, AV_LOG_ERROR, "RNLHandler_SetRes error\n");
            return AVERROR(ENOMEM);
        }
        ctx->initialised = 1;
    }

    err = RNLHandler_Process(&vdt_in[0], &vdt_in[1], &vdt_in[2],
                             &vdt_out[0], &vdt_out[1], &vdt_out[2],
                             ctx->blending);
    if (err != RNLErrorNone) {
        av_log(ctx, AV_LOG_ERROR, "RNLHandler_Process error\n");
        return AVERROR(ENOMEM);
    }

    err = av_frame_copy_props(output, input);
    if (err < 0)
        goto fail;

    av_frame_free(&input);

    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);

    return ff_filter_frame(outlink, output);

fail:
    av_frame_free(&input);
    av_frame_free(&output);
    return err;
}

static int raisr_filter_config_input(AVFilterLink *inlink)
{
    AVHWFramesContext *input_frames;
    int err;

    input_frames = (AVHWFramesContext*)inlink->hw_frames_ctx->data;
    if (input_frames->format != AV_PIX_FMT_OPENCL)
        return AVERROR(EINVAL);

    if (input_frames->sw_format != AV_PIX_FMT_NV12 &&
        input_frames->sw_format != AV_PIX_FMT_YUV420P &&
        input_frames->sw_format != AV_PIX_FMT_P010)
        return AVERROR(EINVAL);

    err = ff_opencl_filter_config_input(inlink);
    if (err < 0)
        return err;

    return 0;
}

static int raisr_opencl_config_output(AVFilterLink *outlink)
{
    AVFilterContext *avctx = outlink->src;
    AVFilterLink *inlink = avctx->inputs[0];
    RaisrOpenCLContext *ctx = avctx->priv;
    AVHWFramesContext *input_frames;
    const AVPixFmtDescriptor *desc;
    int err;

    err = raisr_opencl_init(avctx);
    if (err < 0)
        return err;

    input_frames = (AVHWFramesContext*)inlink->hw_frames_ctx->data;
    ctx->sw_format = (enum AVPixelFormat)input_frames->sw_format;
    desc = av_pix_fmt_desc_get(ctx->sw_format);
    if (desc && desc->comp[0].depth != ctx->bits) {
        av_log(ctx, AV_LOG_ERROR, "input pixel doesn't match model's bitdepth\n");
        return AVERROR(EINVAL);
    }

    ctx->ocf.output_width = inlink->w * ctx->ratio;
    ctx->ocf.output_height = inlink->h * ctx->ratio;
    if (ctx->evenoutput == 1) {
        ctx->ocf.output_width -= ctx->ocf.output_width % 2;
        ctx->ocf.output_height -= ctx->ocf.output_height % 2;
    }

    err = ff_opencl_filter_config_output(outlink);
    if (err < 0)
        return err;

    return 0;
}

static av_cold void raisr_opencl_uninit(AVFilterContext *avctx)
{
    RNLHandler_Deinit();
    ff_opencl_filter_uninit(avctx);
}

#define OFFSET(x) offsetof(RaisrOpenCLContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption raisr_opencl_options[] = {
    {"ratio", "ratio of the upscaling, between 1 and 2", OFFSET(ratio),
	 AV_OPT_TYPE_FLOAT, {.dbl = DEFAULT_RATIO}, MIN_RATIO, MAX_RATIO, FLAGS},
    {"bits", "bit depth", OFFSET(bits), AV_OPT_TYPE_INT, {.i64 = 8}, 8, 10, FLAGS},
    {"range", "input color range", OFFSET(range), AV_OPT_TYPE_INT, {.i64 = VideoRange}, VideoRange, FullRange, FLAGS, "range"},
        { "video", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = VideoRange  },   INT_MIN, INT_MAX, FLAGS, "range" },
        { "full",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = FullRange  },    INT_MIN, INT_MAX, FLAGS, "range" },
    {"filterfolder", "absolute filter folder path", OFFSET(filterfolder), AV_OPT_TYPE_STRING, {.str = "filters_2x/filters_lowres"}, 0, 0, FLAGS},
    {"blending", "CT blending mode (1: Randomness, 2: CountOfBitsChanged)",
      OFFSET(blending), AV_OPT_TYPE_INT, {.i64 = CountOfBitsChanged}, Randomness, CountOfBitsChanged, FLAGS, "blending"},
        { "Randomness",         NULL, 0, AV_OPT_TYPE_CONST, { .i64 = Randomness  },            INT_MIN, INT_MAX, FLAGS, "blending" },
        { "CountOfBitsChanged", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = CountOfBitsChanged   },   INT_MIN, INT_MAX, FLAGS, "blending" },
    {"passes", "passes to run (1: one pass, 2: two pass)", OFFSET(passes), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 2, FLAGS},
    {"mode", "mode for two pass (1: upscale in 1st pass, 2: upscale in 2nd pass)", OFFSET(mode), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 2, FLAGS},
    {"evenoutput", "make output size as even number (0: ignore, 1: subtract 1px if needed)", OFFSET(evenoutput), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS},
    {NULL}
};

AVFILTER_DEFINE_CLASS(raisr_opencl);

static const AVFilterPad raisr_opencl_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = &raisr_opencl_filter_frame,
        .config_props = &raisr_filter_config_input,
    }
};

static const AVFilterPad raisr_opencl_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = &raisr_opencl_config_output,
    }
};

const AVFilter ff_vf_raisr_opencl = {
    .name           = "raisr_opencl",
    .description    = NULL_IF_CONFIG_SMALL("Raisr"),
    .priv_size      = sizeof(RaisrOpenCLContext),
    .priv_class     = &raisr_opencl_class,
    .init           = &ff_opencl_filter_init,
    .uninit         = &raisr_opencl_uninit,
    FILTER_INPUTS(raisr_opencl_inputs),
    FILTER_OUTPUTS(raisr_opencl_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_OPENCL),
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
