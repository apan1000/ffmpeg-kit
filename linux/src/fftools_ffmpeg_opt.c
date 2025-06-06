/*
 * ffmpeg option parsing
 * Copyright (c) 2018-2020 Taner Sener
 * Copyright (c) 2023-2024 ARTHENICA LTD
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

/*
 * This file is the modified version of ffmpeg_opt.c file living in ffmpeg
 * source code under the fftools folder. We manually update it each time we
 * depend on a new ffmpeg version. Below you can see the list of changes applied
 * by us to develop mobile-ffmpeg and later ffmpeg-kit libraries.
 *
 * ffmpeg-kit changes by ARTHENICA LTD
 *
 * 11.2024
 * --------------------------------------------------------
 * - FFmpeg 6.1 changes migrated
 *
 * 07.2023
 * --------------------------------------------------------
 * - FFmpeg 6.0 changes migrated
 * - include fftools_ffmpeg_mux.h added
 * - fftools header names updated
 *
 * mobile-ffmpeg / ffmpeg-kit changes by Taner Sener
 *
 * 08.2020
 * --------------------------------------------------------
 * - read_file renamed as fftools_read_file
 *
 * 01.2020
 * --------------------------------------------------------
 * - ffprobe support (added ffmpeg_ prefix to options used by ffmpeg, logs with
 * AV_LOG_INFO level migrated to use AV_LOG_STDERR)
 *
 * 12.2019
 * --------------------------------------------------------
 * - concurrent execution support ("__thread" specifier added to variables used
 * by multiple threads, static keyword removed from methods called by both
 * ffmpeg and ffprobe, options replaced with global_options, options definition
 *   deleted)
 *
 * 08.2018
 * --------------------------------------------------------
 * - fftools_ prefix added to file name and parent headers
 *
 * 07.2018
 * --------------------------------------------------------
 * - parentheses placed around assignments in condition to prevent -Wparentheses
 * warning
 */

#include "config.h"

#include <stdint.h>

#if HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include "fftools_cmdutils.h"
#include "fftools_ffmpeg.h"
#include "fftools_ffmpeg_mux.h"
#include "fftools_opt_common.h"
#include "fftools_sync_queue.h"

#include "libavformat/avformat.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"

#include "libavfilter/avfilter.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/bprint.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
#include "libavutil/fifo.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"

const char *const opt_name_codec_names[] = {
    "c", "codec", "acodec", "vcodec", "scodec", "dcodec", NULL};
const char *const opt_name_frame_rates[] = {"r", NULL};
const char *const opt_name_codec_tags[] = {"tag", "atag", "vtag", "stag", NULL};
#if FFMPEG_OPT_TOP
const char *const opt_name_top_field_first[] = {"top", NULL};
#endif

__thread HWDevice *filter_hw_device;

__thread char *vstats_filename;
__thread char *sdp_filename;

__thread float audio_drift_threshold = 0.1;
__thread float dts_delta_threshold = 10;
__thread float dts_error_threshold = 3600 * 30;

__thread enum VideoSyncMethod video_sync_method = VSYNC_AUTO;
__thread float frame_drop_threshold = 0;
__thread int do_benchmark = 0;
__thread int do_benchmark_all = 0;
__thread int do_hex_dump = 0;
__thread int do_pkt_dump = 0;
__thread int copy_ts = 0;
__thread int start_at_zero = 0;
__thread int copy_tb = -1;
__thread int debug_ts = 0;
__thread int exit_on_error = 0;
__thread int abort_on_flags = 0;
__thread int print_stats = -1;
__thread int stdin_interaction = 1;
__thread float max_error_rate = 2.0 / 3;
__thread char *filter_nbthreads = NULL;
__thread int filter_complex_nbthreads = 0;
__thread int vstats_version = 2;
__thread int auto_conversion_filters = 1;
__thread int64_t stats_period = 500000;

__thread int file_overwrite = 0;
__thread int no_file_overwrite = 0;
#if FFMPEG_OPT_PSNR
__thread int do_psnr = 0;
#endif
__thread int ignore_unknown_streams = 0;
__thread int copy_unknown_streams = 0;
__thread int recast_media = 0;

extern __thread OptionDef *ffmpeg_options;

void uninit_options(OptionsContext *o) {
    const OptionDef *po = ffmpeg_options;
    int i;

    /* all OPT_SPEC and OPT_STRING can be freed in generic way */
    while (po->name) {
        void *dst = (uint8_t *)o + po->u.off;

        if (po->flags & OPT_SPEC) {
            SpecifierOpt **so = dst;
            int i, *count = (int *)(so + 1);
            for (i = 0; i < *count; i++) {
                av_freep(&(*so)[i].specifier);
                if (po->flags & OPT_STRING)
                    av_freep(&(*so)[i].u.str);
            }
            av_freep(so);
            *count = 0;
        } else if (po->flags & OPT_OFFSET && po->flags & OPT_STRING)
            av_freep(dst);
        po++;
    }

    for (i = 0; i < o->nb_stream_maps; i++)
        av_freep(&o->stream_maps[i].linklabel);
    av_freep(&o->stream_maps);
#if FFMPEG_OPT_MAP_CHANNEL
    av_freep(&o->audio_channel_maps);
#endif
    av_freep(&o->attachments);

    av_dict_free(&o->streamid);
}

void init_options(OptionsContext *o) {
    memset(o, 0, sizeof(*o));

    o->stop_time = INT64_MAX;
    o->mux_max_delay = 0.7;
    o->start_time = AV_NOPTS_VALUE;
    o->start_time_eof = AV_NOPTS_VALUE;
    o->recording_time = INT64_MAX;
    o->limit_filesize = INT64_MAX;
    o->chapters_input_file = INT_MAX;
    o->accurate_seek = 1;
    o->thread_queue_size = -1;
    o->input_sync_ref = -1;
    o->find_stream_info = 1;
    o->shortest_buf_duration = 10.f;
}

int show_hwaccels(void *optctx, const char *opt, const char *arg) {
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;

    av_log(NULL, AV_LOG_STDERR, "Hardware acceleration methods:\n");
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
        av_log(NULL, AV_LOG_STDERR, "%s\n", av_hwdevice_get_type_name(type));
    av_log(NULL, AV_LOG_STDERR, "\n");
    return 0;
}

/* return a copy of the input with the stream specifiers removed from the keys
 */
AVDictionary *strip_specifiers(const AVDictionary *dict) {
    const AVDictionaryEntry *e = NULL;
    AVDictionary *ret = NULL;

    while ((e = av_dict_iterate(dict, e))) {
        char *p = strchr(e->key, ':');

        if (p)
            *p = 0;
        av_dict_set(&ret, e->key, e->value, 0);
        if (p)
            *p = ':';
    }
    return ret;
}

int parse_and_set_vsync(const char *arg, int *vsync_var, int file_idx,
                        int st_idx, int is_global) {
    if (!av_strcasecmp(arg, "cfr"))
        *vsync_var = VSYNC_CFR;
    else if (!av_strcasecmp(arg, "vfr"))
        *vsync_var = VSYNC_VFR;
    else if (!av_strcasecmp(arg, "passthrough"))
        *vsync_var = VSYNC_PASSTHROUGH;
    else if (!av_strcasecmp(arg, "drop"))
        *vsync_var = VSYNC_DROP;
    else if (!is_global && !av_strcasecmp(arg, "auto"))
        *vsync_var = VSYNC_AUTO;
    else if (!is_global) {
        av_log(NULL, AV_LOG_FATAL,
               "Invalid value %s specified for fps_mode of #%d:%d.\n", arg,
               file_idx, st_idx);
        return AVERROR(EINVAL);
    }

    if (is_global && *vsync_var == VSYNC_AUTO) {
        int ret;
        double num;

        ret = parse_number("vsync", arg, OPT_INT, VSYNC_AUTO, VSYNC_VFR, &num);
        if (ret < 0)
            return ret;

        video_sync_method = num;
        av_log(NULL, AV_LOG_WARNING,
               "Passing a number to -vsync is deprecated,"
               " use a string argument as described in the manual.\n");
    }
    return 0;
}

/* Correct input file start times based on enabled streams */
void correct_input_start_times(void) {
    for (int i = 0; i < nb_input_files; i++) {
        InputFile *ifile = input_files[i];
        AVFormatContext *is = ifile->ctx;
        int64_t new_start_time = INT64_MAX, diff, abs_start_seek;

        ifile->start_time_effective = is->start_time;

        if (is->start_time == AV_NOPTS_VALUE ||
            !(is->iformat->flags & AVFMT_TS_DISCONT))
            continue;

        for (int j = 0; j < is->nb_streams; j++) {
            AVStream *st = is->streams[j];
            if (st->discard == AVDISCARD_ALL ||
                st->start_time == AV_NOPTS_VALUE)
                continue;
            new_start_time = FFMIN(
                new_start_time,
                av_rescale_q(st->start_time, st->time_base, AV_TIME_BASE_Q));
        }

        diff = new_start_time - is->start_time;
        if (diff) {
            av_log(NULL, AV_LOG_VERBOSE,
                   "Correcting start time of Input #%d by %" PRId64 " us.\n", i,
                   diff);
            ifile->start_time_effective = new_start_time;
            if (copy_ts && start_at_zero)
                ifile->ts_offset = -new_start_time;
            else if (!copy_ts) {
                abs_start_seek =
                    is->start_time + ((ifile->start_time != AV_NOPTS_VALUE)
                                          ? ifile->start_time
                                          : 0);
                ifile->ts_offset = abs_start_seek > new_start_time
                                       ? -abs_start_seek
                                       : -new_start_time;
            } else if (copy_ts)
                ifile->ts_offset = 0;

            ifile->ts_offset += ifile->input_ts_offset;
        }
    }
}

int apply_sync_offsets(void) {
    for (int i = 0; i < nb_input_files; i++) {
        InputFile *ref, *self = input_files[i];
        int64_t adjustment;
        int64_t self_start_time, ref_start_time, self_seek_start,
            ref_seek_start;
        int start_times_set = 1;

        if (self->input_sync_ref == -1 || self->input_sync_ref == i)
            continue;
        if (self->input_sync_ref >= nb_input_files ||
            self->input_sync_ref < -1) {
            av_log(NULL, AV_LOG_FATAL,
                   "-isync for input %d references non-existent input %d.\n", i,
                   self->input_sync_ref);
            return AVERROR(EINVAL);
        }

        if (copy_ts && !start_at_zero) {
            av_log(
                NULL, AV_LOG_FATAL,
                "Use of -isync requires that start_at_zero be set if copyts is "
                "set.\n");
            return AVERROR(EINVAL);
        }

        ref = input_files[self->input_sync_ref];
        if (ref->input_sync_ref != -1 &&
            ref->input_sync_ref != self->input_sync_ref) {
            av_log(NULL, AV_LOG_ERROR,
                   "-isync for input %d references a resynced input %d. Sync "
                   "not set.\n",
                   i, self->input_sync_ref);
            continue;
        }

        if (self->ctx->start_time_realtime != AV_NOPTS_VALUE &&
            ref->ctx->start_time_realtime != AV_NOPTS_VALUE) {
            self_start_time = self->ctx->start_time_realtime;
            ref_start_time = ref->ctx->start_time_realtime;
        } else if (self->start_time_effective != AV_NOPTS_VALUE &&
                   ref->start_time_effective != AV_NOPTS_VALUE) {
            self_start_time = self->start_time_effective;
            ref_start_time = ref->start_time_effective;
        } else {
            start_times_set = 0;
        }

        if (start_times_set) {
            self_seek_start =
                self->start_time == AV_NOPTS_VALUE ? 0 : self->start_time;
            ref_seek_start =
                ref->start_time == AV_NOPTS_VALUE ? 0 : ref->start_time;

            adjustment = (self_start_time - ref_start_time) +
                         !copy_ts * (self_seek_start - ref_seek_start) +
                         ref->input_ts_offset;

            self->ts_offset += adjustment;

            av_log(NULL, AV_LOG_INFO,
                   "Adjusted ts offset for Input #%d by %" PRId64
                   " us to sync with Input #%d.\n",
                   i, adjustment, self->input_sync_ref);
        } else {
            av_log(
                NULL, AV_LOG_INFO,
                "Unable to identify start times for Inputs #%d and %d both. No "
                "sync adjustment made.\n",
                i, self->input_sync_ref);
        }
    }

    return 0;
}

int opt_filter_threads(void *optctx, const char *opt, const char *arg) {
    av_free(filter_nbthreads);
    filter_nbthreads = av_strdup(arg);
    return 0;
}

int opt_abort_on(void *optctx, const char *opt, const char *arg) {
    static const AVOption opts[] = {
        {"abort_on",
         NULL,
         0,
         AV_OPT_TYPE_FLAGS,
         {.i64 = 0},
         INT64_MIN,
         (double)INT64_MAX,
         .unit = "flags"},
        {"empty_output",
         NULL,
         0,
         AV_OPT_TYPE_CONST,
         {.i64 = ABORT_ON_FLAG_EMPTY_OUTPUT},
         .unit = "flags"},
        {"empty_output_stream",
         NULL,
         0,
         AV_OPT_TYPE_CONST,
         {.i64 = ABORT_ON_FLAG_EMPTY_OUTPUT_STREAM},
         .unit = "flags"},
        {NULL},
    };
    static const AVClass class = {
        .class_name = "",
        .item_name = av_default_item_name,
        .option = opts,
        .version = LIBAVUTIL_VERSION_INT,
    };
    const AVClass *pclass = &class;

    return av_opt_eval_flags(&pclass, &opts[0], arg, &abort_on_flags);
}

int opt_stats_period(void *optctx, const char *opt, const char *arg) {
    int64_t user_stats_period;
    int ret = av_parse_time(&user_stats_period, arg, 1);
    if (ret < 0)
        return ret;

    if (user_stats_period <= 0) {
        av_log(NULL, AV_LOG_ERROR, "stats_period %s must be positive.\n", arg);
        return AVERROR(EINVAL);
    }

    stats_period = user_stats_period;
    av_log(NULL, AV_LOG_INFO, "ffmpeg stats and -progress period set to %s.\n",
           arg);

    return 0;
}

int opt_audio_codec(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "codec:a", arg, ffmpeg_options);
}

int opt_video_codec(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "codec:v", arg, ffmpeg_options);
}

int opt_subtitle_codec(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "codec:s", arg, ffmpeg_options);
}

int opt_data_codec(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "codec:d", arg, ffmpeg_options);
}

int opt_map(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    StreamMap *m = NULL;
    int i, negative = 0, file_idx, disabled = 0;
    int ret;
    char *map, *p;
    char *allow_unused;

    if (*arg == '-') {
        negative = 1;
        arg++;
    }
    map = av_strdup(arg);
    if (!map)
        return AVERROR(ENOMEM);

#if FFMPEG_OPT_MAP_SYNC
    {
        /* parse sync stream first, just pick first matching stream */
        char *sync = strchr(map, ',');

        if (sync) {
            *sync = 0;
            av_log(
                NULL, AV_LOG_WARNING,
                "Specifying a sync stream is deprecated and has no effect\n");
        }
    }
#endif

    if (map[0] == '[') {
        /* this mapping refers to lavfi output */
        const char *c = map + 1;

        ret = GROW_ARRAY(o->stream_maps, o->nb_stream_maps);
        if (ret < 0)
            goto fail;

        m = &o->stream_maps[o->nb_stream_maps - 1];
        m->linklabel = av_get_token(&c, "]");
        if (!m->linklabel) {
            av_log(NULL, AV_LOG_ERROR, "Invalid output link label: %s.\n", map);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    } else {
        if ((allow_unused = strchr(map, '?')))
            *allow_unused = 0;
        file_idx = strtol(map, &p, 0);
        if (file_idx >= nb_input_files || file_idx < 0) {
            av_log(NULL, AV_LOG_FATAL, "Invalid input file index: %d.\n",
                   file_idx);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (negative)
            /* disable some already defined maps */
            for (i = 0; i < o->nb_stream_maps; i++) {
                m = &o->stream_maps[i];
                if (file_idx == m->file_index &&
                    check_stream_specifier(input_files[m->file_index]->ctx,
                                           input_files[m->file_index]
                                               ->ctx->streams[m->stream_index],
                                           *p == ':' ? p + 1 : p) > 0)
                    m->disabled = 1;
            }
        else
            for (i = 0; i < input_files[file_idx]->nb_streams; i++) {
                if (check_stream_specifier(
                        input_files[file_idx]->ctx,
                        input_files[file_idx]->ctx->streams[i],
                        *p == ':' ? p + 1 : p) <= 0)
                    continue;
                if (input_files[file_idx]->streams[i]->user_set_discard ==
                    AVDISCARD_ALL) {
                    disabled = 1;
                    continue;
                }
                ret = GROW_ARRAY(o->stream_maps, o->nb_stream_maps);
                if (ret < 0)
                    goto fail;

                m = &o->stream_maps[o->nb_stream_maps - 1];

                m->file_index = file_idx;
                m->stream_index = i;
            }
    }

    if (!m) {
        if (allow_unused) {
            av_log(NULL, AV_LOG_VERBOSE,
                   "Stream map '%s' matches no streams; ignoring.\n", arg);
        } else if (disabled) {
            av_log(NULL, AV_LOG_FATAL,
                   "Stream map '%s' matches disabled streams.\n"
                   "To ignore this, add a trailing '?' to the map.\n",
                   arg);
            ret = AVERROR(EINVAL);
            goto fail;
        } else {
            av_log(NULL, AV_LOG_FATAL,
                   "Stream map '%s' matches no streams.\n"
                   "To ignore this, add a trailing '?' to the map.\n",
                   arg);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }
    ret = 0;
fail:
    av_freep(&map);
    return ret;
}

int opt_attach(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    int ret = GROW_ARRAY(o->attachments, o->nb_attachments);
    if (ret < 0)
        return ret;

    o->attachments[o->nb_attachments - 1] = arg;
    return 0;
}

#if FFMPEG_OPT_MAP_CHANNEL
int opt_map_channel(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    int n, ret;
    AVStream *st;
    AudioChannelMap *m;
    char *allow_unused;
    char *mapchan;

    av_log(NULL, AV_LOG_WARNING,
           "The -%s option is deprecated and will be removed. "
           "It can be replaced by the 'pan' filter, or in some cases by "
           "combinations of 'channelsplit', 'channelmap', 'amerge' filters.\n",
           opt);

    mapchan = av_strdup(arg);
    if (!mapchan)
        return AVERROR(ENOMEM);

    ret = GROW_ARRAY(o->audio_channel_maps, o->nb_audio_channel_maps);
    if (ret < 0)
        goto end;

    m = &o->audio_channel_maps[o->nb_audio_channel_maps - 1];

    /* muted channel syntax */
    n = sscanf(arg, "%d:%d.%d", &m->channel_idx, &m->ofile_idx,
               &m->ostream_idx);
    if ((n == 1 || n == 3) && m->channel_idx == -1) {
        m->file_idx = m->stream_idx = -1;
        if (n == 1)
            m->ofile_idx = m->ostream_idx = -1;
        av_free(mapchan);
        return 0;
    }

    /* normal syntax */
    n = sscanf(arg, "%d.%d.%d:%d.%d", &m->file_idx, &m->stream_idx,
               &m->channel_idx, &m->ofile_idx, &m->ostream_idx);

    if (n != 3 && n != 5) {
        av_log(NULL, AV_LOG_FATAL,
               "Syntax error, mapchan usage: "
               "[file.stream.channel|-1][:syncfile:syncstream]\n");
        goto fail;
    }

    if (n != 5) // only file.stream.channel specified
        m->ofile_idx = m->ostream_idx = -1;

    /* check input */
    if (m->file_idx < 0 || m->file_idx >= nb_input_files) {
        av_log(NULL, AV_LOG_FATAL, "mapchan: invalid input file index: %d\n",
               m->file_idx);
        goto fail;
    }
    if (m->stream_idx < 0 ||
        m->stream_idx >= input_files[m->file_idx]->nb_streams) {
        av_log(NULL, AV_LOG_FATAL,
               "mapchan: invalid input file stream index #%d.%d\n", m->file_idx,
               m->stream_idx);
        goto fail;
    }
    st = input_files[m->file_idx]->ctx->streams[m->stream_idx];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(NULL, AV_LOG_FATAL,
               "mapchan: stream #%d.%d is not an audio stream.\n", m->file_idx,
               m->stream_idx);
        goto fail;
    }
    /* allow trailing ? to map_channel */
    if ((allow_unused = strchr(mapchan, '?')))
        *allow_unused = 0;
    if (m->channel_idx < 0 ||
        m->channel_idx >= st->codecpar->ch_layout.nb_channels ||
        input_files[m->file_idx]->streams[m->stream_idx]->user_set_discard ==
            AVDISCARD_ALL) {
        if (allow_unused) {
            av_log(NULL, AV_LOG_VERBOSE,
                   "mapchan: invalid audio channel #%d.%d.%d\n", m->file_idx,
                   m->stream_idx, m->channel_idx);
        } else {
            av_log(NULL, AV_LOG_FATAL,
                   "mapchan: invalid audio channel #%d.%d.%d\n"
                   "To ignore this, add a trailing '?' to the map_channel.\n",
                   m->file_idx, m->stream_idx, m->channel_idx);
            goto fail;
        }
    }
    ret = 0;
end:
    av_free(mapchan);
    return ret;
fail:
    ret = AVERROR(EINVAL);
    goto end;
}
#endif

int opt_sdp_file(void *optctx, const char *opt, const char *arg) {
    av_free(sdp_filename);
    sdp_filename = av_strdup(arg);
    return 0;
}

#if CONFIG_VAAPI
int opt_vaapi_device(void *optctx, const char *opt, const char *arg) {
    const char *prefix = "vaapi:";
    char *tmp;
    int err;
    tmp = av_asprintf("%s%s", prefix, arg);
    if (!tmp)
        return AVERROR(ENOMEM);
    err = hw_device_init_from_string(tmp, NULL);
    av_free(tmp);
    return err;
}
#endif

#if CONFIG_QSV
int opt_qsv_device(void *optctx, const char *opt, const char *arg) {
    const char *prefix = "qsv=__qsv_device:hw_any,child_device=";
    int err;
    char *tmp = av_asprintf("%s%s", prefix, arg);

    if (!tmp)
        return AVERROR(ENOMEM);

    err = hw_device_init_from_string(tmp, NULL);
    av_free(tmp);

    return err;
}
#endif

int opt_init_hw_device(void *optctx, const char *opt, const char *arg) {
    if (!strcmp(arg, "list")) {
        enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
        av_log(NULL, AV_LOG_STDERR, "Supported hardware device types:\n");
        while ((type = av_hwdevice_iterate_types(type)) !=
               AV_HWDEVICE_TYPE_NONE)
            av_log(NULL, AV_LOG_STDERR, "%s\n",
                   av_hwdevice_get_type_name(type));
        av_log(NULL, AV_LOG_STDERR, "\n");
        return AVERROR_EXIT;
    } else {
        return hw_device_init_from_string(arg, NULL);
    }
}

int opt_filter_hw_device(void *optctx, const char *opt, const char *arg) {
    if (filter_hw_device) {
        av_log(NULL, AV_LOG_ERROR, "Only one filter device can be used.\n");
        return AVERROR(EINVAL);
    }
    filter_hw_device = hw_device_get_by_name(arg);
    if (!filter_hw_device) {
        av_log(NULL, AV_LOG_ERROR, "Invalid filter device %s.\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

int opt_recording_timestamp(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    char buf[128];
    int64_t recording_timestamp;
    int ret;
    struct tm time;

    ret = av_parse_time(&recording_timestamp, arg, 0);
    if (ret < 0)
        return ret;

    recording_timestamp /= 1e6;
    time = *gmtime((time_t *)&recording_timestamp);
    if (!strftime(buf, sizeof(buf), "creation_time=%Y-%m-%dT%H:%M:%S%z", &time))
        return -1;
    parse_option(o, "metadata", buf, ffmpeg_options);

    av_log(NULL, AV_LOG_WARNING,
           "%s is deprecated, set the 'creation_time' metadata "
           "tag instead.\n",
           opt);
    return 0;
}

int find_codec(void *logctx, const char *name, enum AVMediaType type,
               int encoder, const AVCodec **pcodec) {
    const AVCodecDescriptor *desc;
    const char *codec_string = encoder ? "encoder" : "decoder";
    const AVCodec *codec;

    codec = encoder ? avcodec_find_encoder_by_name(name)
                    : avcodec_find_decoder_by_name(name);

    if (!codec && (desc = avcodec_descriptor_get_by_name(name))) {
        codec = encoder ? avcodec_find_encoder(desc->id)
                        : avcodec_find_decoder(desc->id);
        if (codec)
            av_log(logctx, AV_LOG_VERBOSE, "Matched %s '%s' for codec '%s'.\n",
                   codec_string, codec->name, desc->name);
    }

    if (!codec) {
        av_log(logctx, AV_LOG_FATAL, "Unknown %s '%s'\n", codec_string, name);
        return encoder ? AVERROR_ENCODER_NOT_FOUND : AVERROR_DECODER_NOT_FOUND;
    }
    if (codec->type != type && !recast_media) {
        av_log(logctx, AV_LOG_FATAL, "Invalid %s type '%s'\n", codec_string,
               name);
        return AVERROR(EINVAL);
    }

    *pcodec = codec;
    return 0;
    ;
}

int assert_file_overwrite(const char *filename) {
    const char *proto_name = avio_find_protocol_name(filename);

    if (file_overwrite && no_file_overwrite) {
        av_log(NULL, AV_LOG_STDERR,
               "Error, both -y and -n supplied. Exiting.\n");
        return AVERROR(EINVAL);
    }

    if (!file_overwrite) {
        if (proto_name && !strcmp(proto_name, "file") &&
            avio_check(filename, 0) == 0) {
            if (stdin_interaction && !no_file_overwrite) {
                av_log(NULL, AV_LOG_STDERR,
                       "File '%s' already exists. Overwrite? [y/N] ", filename);
                term_exit();
                signal(SIGINT, SIG_DFL);
                if (!read_yesno()) {
                    av_log(NULL, AV_LOG_FATAL, "Not overwriting - exiting\n");
                    return AVERROR_EXIT;
                }
                term_init();
            } else {
                av_log(NULL, AV_LOG_FATAL,
                       "File '%s' already exists. Exiting.\n", filename);
                return AVERROR_EXIT;
            }
        }
    }

    if (proto_name && !strcmp(proto_name, "file")) {
        for (int i = 0; i < nb_input_files; i++) {
            InputFile *file = input_files[i];
            if (file->ctx->iformat->flags & AVFMT_NOFILE)
                continue;
            if (!strcmp(filename, file->ctx->url)) {
                av_log(NULL, AV_LOG_FATAL,
                       "Output %s same as Input #%d - exiting\n", filename, i);
                av_log(NULL, AV_LOG_WARNING,
                       "FFmpeg cannot edit existing files in-place.\n");
                return AVERROR(EINVAL);
            }
        }
    }

    return 0;
}

/* read file contents into a string */
char *file_read(const char *filename) {
    AVIOContext *pb = NULL;
    int ret = avio_open(&pb, filename, AVIO_FLAG_READ);
    AVBPrint bprint;
    char *str;

    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error opening file %s.\n", filename);
        return NULL;
    }

    av_bprint_init(&bprint, 0, AV_BPRINT_SIZE_UNLIMITED);
    ret = avio_read_to_bprint(pb, &bprint, SIZE_MAX);
    avio_closep(&pb);
    if (ret < 0) {
        av_bprint_finalize(&bprint, NULL);
        return NULL;
    }
    ret = av_bprint_finalize(&bprint, &str);
    if (ret < 0)
        return NULL;
    return str;
}

/* arg format is "output-stream-index:streamid-value". */
int opt_streamid(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    char *p;
    char idx_str[16];

    av_strlcpy(idx_str, arg, sizeof(idx_str));
    p = strchr(idx_str, ':');
    if (!p) {
        av_log(NULL, AV_LOG_FATAL,
               "Invalid value '%s' for option '%s', required syntax is "
               "'index:value'\n",
               arg, opt);
        return AVERROR(EINVAL);
    }
    *p++ = '\0';

    return av_dict_set(&o->streamid, idx_str, p, 0);
}

int init_complex_filters(void) {
    int i, ret = 0;

    for (i = 0; i < nb_filtergraphs; i++) {
        ret = init_complex_filtergraph(filtergraphs[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int opt_target(void *optctx, const char *opt, const char *arg) {
    const OptionDef *options = ffmpeg_options;
    OptionsContext *o = optctx;
    enum { PAL, NTSC, FILM, UNKNOWN } norm = UNKNOWN;
    static const char *const frame_rates[] = {"25", "30000/1001", "24000/1001"};

    if (!strncmp(arg, "pal-", 4)) {
        norm = PAL;
        arg += 4;
    } else if (!strncmp(arg, "ntsc-", 5)) {
        norm = NTSC;
        arg += 5;
    } else if (!strncmp(arg, "film-", 5)) {
        norm = FILM;
        arg += 5;
    } else {
        /* Try to determine PAL/NTSC by peeking in the input files */
        if (nb_input_files) {
            int i, j;
            for (j = 0; j < nb_input_files; j++) {
                for (i = 0; i < input_files[j]->nb_streams; i++) {
                    AVStream *st = input_files[j]->ctx->streams[i];
                    int64_t fr;
                    if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                        continue;
                    fr = st->time_base.den * 1000LL / st->time_base.num;
                    if (fr == 25000) {
                        norm = PAL;
                        break;
                    } else if ((fr == 29970) || (fr == 23976)) {
                        norm = NTSC;
                        break;
                    }
                }
                if (norm != UNKNOWN)
                    break;
            }
        }
        if (norm != UNKNOWN)
            av_log(NULL, AV_LOG_INFO, "Assuming %s for target.\n",
                   norm == PAL ? "PAL" : "NTSC");
    }

    if (norm == UNKNOWN) {
        av_log(NULL, AV_LOG_FATAL,
               "Could not determine norm (PAL/NTSC/NTSC-Film) for target.\n");
        av_log(NULL, AV_LOG_FATAL,
               "Please prefix target with \"pal-\", \"ntsc-\" or \"film-\",\n");
        av_log(NULL, AV_LOG_FATAL, "or set a framerate with \"-r xxx\".\n");
        return AVERROR(EINVAL);
    }

    if (!strcmp(arg, "vcd")) {
        opt_video_codec(o, "c:v", "mpeg1video");
        opt_audio_codec(o, "c:a", "mp2");
        parse_option(o, "f", "vcd", options);

        parse_option(o, "s", norm == PAL ? "352x288" : "352x240", options);
        parse_option(o, "r", frame_rates[norm], options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "1150000");
        opt_default(NULL, "maxrate:v", "1150000");
        opt_default(NULL, "minrate:v", "1150000");
        opt_default(NULL, "bufsize:v", "327680"); // 40*1024*8;

        opt_default(NULL, "b:a", "224000");
        parse_option(o, "ar", "44100", options);
        parse_option(o, "ac", "2", options);

        opt_default(NULL, "packetsize", "2324");
        opt_default(NULL, "muxrate", "1411200"); // 2352 * 75 * 8;

        /* We have to offset the PTS, so that it is consistent with the SCR.
           SCR starts at 36000, but the first two packs contain only padding
           and the first pack from the other stream, respectively, may also have
           been written before.
           So the real data starts at SCR 36000+3*1200. */
        o->mux_preload = (36000 + 3 * 1200) / 90000.0; // 0.44
    } else if (!strcmp(arg, "svcd")) {

        opt_video_codec(o, "c:v", "mpeg2video");
        opt_audio_codec(o, "c:a", "mp2");
        parse_option(o, "f", "svcd", options);

        parse_option(o, "s", norm == PAL ? "480x576" : "480x480", options);
        parse_option(o, "r", frame_rates[norm], options);
        parse_option(o, "pix_fmt", "yuv420p", options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "2040000");
        opt_default(NULL, "maxrate:v", "2516000");
        opt_default(NULL, "minrate:v", "0");       // 1145000;
        opt_default(NULL, "bufsize:v", "1835008"); // 224*1024*8;
        opt_default(NULL, "scan_offset", "1");

        opt_default(NULL, "b:a", "224000");
        parse_option(o, "ar", "44100", options);

        opt_default(NULL, "packetsize", "2324");

    } else if (!strcmp(arg, "dvd")) {

        opt_video_codec(o, "c:v", "mpeg2video");
        opt_audio_codec(o, "c:a", "ac3");
        parse_option(o, "f", "dvd", options);

        parse_option(o, "s", norm == PAL ? "720x576" : "720x480", options);
        parse_option(o, "r", frame_rates[norm], options);
        parse_option(o, "pix_fmt", "yuv420p", options);
        opt_default(NULL, "g", norm == PAL ? "15" : "18");

        opt_default(NULL, "b:v", "6000000");
        opt_default(NULL, "maxrate:v", "9000000");
        opt_default(NULL, "minrate:v", "0");       // 1500000;
        opt_default(NULL, "bufsize:v", "1835008"); // 224*1024*8;

        opt_default(
            NULL, "packetsize",
            "2048"); // from www.mpucoder.com: DVD sectors contain 2048
                     // bytes of data, this is also the size of one pack.
        opt_default(NULL, "muxrate",
                    "10080000"); // from mplex project: data_rate = 1260000.
                                 // mux_rate = data_rate * 8

        opt_default(NULL, "b:a", "448000");
        parse_option(o, "ar", "48000", options);

    } else if (!strncmp(arg, "dv", 2)) {

        parse_option(o, "f", "dv", options);

        parse_option(o, "s", norm == PAL ? "720x576" : "720x480", options);
        parse_option(o, "pix_fmt",
                     !strncmp(arg, "dv50", 4) ? "yuv422p"
                     : norm == PAL            ? "yuv420p"
                                              : "yuv411p",
                     options);
        parse_option(o, "r", frame_rates[norm], options);

        parse_option(o, "ar", "48000", options);
        parse_option(o, "ac", "2", options);

    } else {
        av_log(NULL, AV_LOG_ERROR, "Unknown target: %s\n", arg);
        return AVERROR(EINVAL);
    }

    av_dict_copy(&o->g->codec_opts, codec_opts, AV_DICT_DONT_OVERWRITE);
    av_dict_copy(&o->g->format_opts, format_opts, AV_DICT_DONT_OVERWRITE);

    return 0;
}

int opt_vstats_file(void *optctx, const char *opt, const char *arg) {
    av_free(vstats_filename);
    vstats_filename = av_strdup(arg);
    return 0;
}

int opt_vstats(void *optctx, const char *opt, const char *arg) {
    char filename[40];
    time_t today2 = time(NULL);
    struct tm *today = localtime(&today2);

    if (!today) { // maybe tomorrow
        av_log(NULL, AV_LOG_FATAL, "Unable to get current time: %s\n",
               strerror(errno));
        return AVERROR(errno);
    }

    snprintf(filename, sizeof(filename), "vstats_%02d%02d%02d.log",
             today->tm_hour, today->tm_min, today->tm_sec);
    return opt_vstats_file(NULL, opt, filename);
}

int opt_video_frames(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "frames:v", arg, ffmpeg_options);
}

int opt_audio_frames(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "frames:a", arg, ffmpeg_options);
}

int opt_data_frames(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "frames:d", arg, ffmpeg_options);
}

int opt_default_new(OptionsContext *o, const char *opt, const char *arg) {
    int ret;
    AVDictionary *cbak = codec_opts;
    AVDictionary *fbak = format_opts;
    codec_opts = NULL;
    format_opts = NULL;

    ret = opt_default(NULL, opt, arg);

    av_dict_copy(&o->g->codec_opts, codec_opts, 0);
    av_dict_copy(&o->g->format_opts, format_opts, 0);
    av_dict_free(&codec_opts);
    av_dict_free(&format_opts);
    codec_opts = cbak;
    format_opts = fbak;

    return ret;
}

int opt_preset(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    FILE *f = NULL;
    char filename[1000], line[1000], tmp_line[1000];
    const char *codec_name = NULL;
    int ret = 0;

    tmp_line[0] = *opt;
    tmp_line[1] = 0;
    MATCH_PER_TYPE_OPT(codec_names, str, codec_name, NULL, tmp_line);

    if (!(f = get_preset_file(filename, sizeof(filename), arg, *opt == 'f',
                              codec_name))) {
        if (!strncmp(arg, "libx264-lossless", strlen("libx264-lossless"))) {
            av_log(NULL, AV_LOG_FATAL, "Please use -preset <speed> -qp 0\n");
        } else
            av_log(NULL, AV_LOG_FATAL, "File for preset '%s' not found\n", arg);
        return AVERROR(ENOENT);
    }

    while (fgets(line, sizeof(line), f)) {
        char *key = tmp_line, *value, *endptr;

        if (strcspn(line, "#\n\r") == 0)
            continue;
        av_strlcpy(tmp_line, line, sizeof(tmp_line));
        if (!av_strtok(key, "=", &value) ||
            !av_strtok(value, "\r\n", &endptr)) {
            av_log(NULL, AV_LOG_FATAL, "%s: Invalid syntax: '%s'\n", filename,
                   line);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        av_log(NULL, AV_LOG_DEBUG, "ffpreset[%s]: set '%s' = '%s'\n", filename,
               key, value);

        if (!strcmp(key, "acodec"))
            opt_audio_codec(o, key, value);
        else if (!strcmp(key, "vcodec"))
            opt_video_codec(o, key, value);
        else if (!strcmp(key, "scodec"))
            opt_subtitle_codec(o, key, value);
        else if (!strcmp(key, "dcodec"))
            opt_data_codec(o, key, value);
        else if (opt_default_new(o, key, value) < 0) {
            av_log(
                NULL, AV_LOG_FATAL,
                "%s: Invalid option or argument: '%s', parsed as '%s' = '%s'\n",
                filename, line, key, value);
            ret = AVERROR(EINVAL);
            goto fail;
        }
    }

fail:
    fclose(f);

    return ret;
}

int opt_old2new(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    int ret;
    char *s = av_asprintf("%s:%c", opt + 1, *opt);
    if (!s)
        return AVERROR(ENOMEM);
    ret = parse_option(o, s, arg, ffmpeg_options);
    av_free(s);
    return ret;
}

int opt_bitrate(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;

    if (!strcmp(opt, "ab")) {
        av_dict_set(&o->g->codec_opts, "b:a", arg, 0);
        return 0;
    } else if (!strcmp(opt, "b")) {
        av_log(NULL, AV_LOG_WARNING,
               "Please use -b:a or -b:v, -b is ambiguous\n");
        av_dict_set(&o->g->codec_opts, "b:v", arg, 0);
        return 0;
    }
    av_dict_set(&o->g->codec_opts, opt, arg, 0);
    return 0;
}

int opt_qscale(void *optctx, const char *opt, const char *arg) {
    OptionDef *options = ffmpeg_options;
    OptionsContext *o = optctx;
    char *s;
    int ret;
    if (!strcmp(opt, "qscale")) {
        av_log(NULL, AV_LOG_WARNING,
               "Please use -q:a or -q:v, -qscale is ambiguous\n");
        return parse_option(o, "q:v", arg, options);
    }
    s = av_asprintf("q%s", opt + 6);
    if (!s)
        return AVERROR(ENOMEM);
    ret = parse_option(o, s, arg, options);
    av_free(s);
    return ret;
}

int opt_profile(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    if (!strcmp(opt, "profile")) {
        av_log(NULL, AV_LOG_WARNING,
               "Please use -profile:a or -profile:v, -profile is ambiguous\n");
        av_dict_set(&o->g->codec_opts, "profile:v", arg, 0);
        return 0;
    }
    av_dict_set(&o->g->codec_opts, opt, arg, 0);
    return 0;
}

int opt_video_filters(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "filter:v", arg, ffmpeg_options);
}

int opt_audio_filters(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "filter:a", arg, ffmpeg_options);
}

int opt_vsync(void *optctx, const char *opt, const char *arg) {
    av_log(NULL, AV_LOG_WARNING, "-vsync is deprecated. Use -fps_mode\n");
    return parse_and_set_vsync(arg, &video_sync_method, -1, -1, 1);
}

int opt_timecode(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    int ret;
    char *tcr = av_asprintf("timecode=%s", arg);
    if (!tcr)
        return AVERROR(ENOMEM);
    ret = parse_option(o, "metadata:g", tcr, ffmpeg_options);
    if (ret >= 0)
        ret = av_dict_set(&o->g->codec_opts, "gop_timecode", arg, 0);
    av_free(tcr);
    return ret;
}

int opt_audio_qscale(void *optctx, const char *opt, const char *arg) {
    OptionsContext *o = optctx;
    return parse_option(o, "q:a", arg, ffmpeg_options);
}

int opt_filter_complex(void *optctx, const char *opt, const char *arg) {
    char *graph_desc = av_strdup(arg);
    if (!graph_desc)
        return AVERROR(ENOMEM);

    return fg_create(NULL, graph_desc);
}

int opt_filter_complex_script(void *optctx, const char *opt, const char *arg) {
    char *graph_desc = file_read(arg);
    if (!graph_desc)
        return AVERROR(EINVAL);

    return fg_create(NULL, graph_desc);
}

void show_help_default_ffmpeg(const char *opt, const char *arg) {
    OptionDef *options = ffmpeg_options;
    /* per-file options have at least one of those set */
    const int per_file = OPT_SPEC | OPT_OFFSET | OPT_PERFILE;
    int show_advanced = 0, show_avoptions = 0;

    if (opt && *opt) {
        if (!strcmp(opt, "long"))
            show_advanced = 1;
        else if (!strcmp(opt, "full"))
            show_advanced = show_avoptions = 1;
        else
            av_log(NULL, AV_LOG_ERROR, "Unknown help option '%s'.\n", opt);
    }

    show_usage();

    av_log(NULL, AV_LOG_STDERR,
           "Getting help:\n"
           "    -h      -- print basic options\n"
           "    -h long -- print more options\n"
           "    -h full -- print all options (including all format and codec "
           "specific options, very long)\n"
           "    -h type=name -- print all options for the named "
           "decoder/encoder/demuxer/muxer/filter/bsf/protocol\n"
           "    See man %s for detailed description of the options.\n"
           "\n",
           program_name);

    show_help_options(
        options, "Print help / information / capabilities:", OPT_EXIT, 0, 0);

    show_help_options(options,
                      "Global options (affect whole program "
                      "instead of just one file):",
                      0, per_file | OPT_EXIT | OPT_EXPERT, 0);
    if (show_advanced)
        show_help_options(options, "Advanced global options:", OPT_EXPERT,
                          per_file | OPT_EXIT, 0);

    show_help_options(
        options, "Per-file main options:", 0,
        OPT_EXPERT | OPT_AUDIO | OPT_VIDEO | OPT_SUBTITLE | OPT_EXIT, per_file);
    if (show_advanced)
        show_help_options(options, "Advanced per-file options:", OPT_EXPERT,
                          OPT_AUDIO | OPT_VIDEO | OPT_SUBTITLE, per_file);

    show_help_options(options, "Video options:", OPT_VIDEO,
                      OPT_EXPERT | OPT_AUDIO, 0);
    if (show_advanced)
        show_help_options(options,
                          "Advanced Video options:", OPT_EXPERT | OPT_VIDEO,
                          OPT_AUDIO, 0);

    show_help_options(options, "Audio options:", OPT_AUDIO,
                      OPT_EXPERT | OPT_VIDEO, 0);
    if (show_advanced)
        show_help_options(options,
                          "Advanced Audio options:", OPT_EXPERT | OPT_AUDIO,
                          OPT_VIDEO, 0);
    show_help_options(options, "Subtitle options:", OPT_SUBTITLE, 0, 0);
    av_log(NULL, AV_LOG_STDERR, "\n");

    if (show_avoptions) {
        int flags = AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_ENCODING_PARAM;
        show_help_children(avcodec_get_class(), flags);
        show_help_children(avformat_get_class(), flags);
#if CONFIG_SWSCALE
        show_help_children(sws_get_class(), flags);
#endif
#if CONFIG_SWRESAMPLE
        show_help_children(swr_get_class(), AV_OPT_FLAG_AUDIO_PARAM);
#endif
        show_help_children(avfilter_get_class(),
                           AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM |
                               AV_OPT_FLAG_FILTERING_PARAM);
        show_help_children(av_bsf_get_class(), AV_OPT_FLAG_VIDEO_PARAM |
                                                   AV_OPT_FLAG_AUDIO_PARAM |
                                                   AV_OPT_FLAG_BSF_PARAM);
    }
}

void show_usage(void) {
    av_log(NULL, AV_LOG_INFO, "Hyper fast Audio and Video encoder\n");
    av_log(NULL, AV_LOG_INFO,
           "usage: %s [options] [[infile options] -i infile]... {[outfile "
           "options] outfile}...\n",
           program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

enum OptGroup {
    GROUP_OUTFILE,
    GROUP_INFILE,
};

static const OptionGroupDef groups[] = {
    [GROUP_OUTFILE] = {"output url", NULL, OPT_OUTPUT},
    [GROUP_INFILE] = {"input url", "i", OPT_INPUT},
};

int open_files(OptionGroupList *l, const char *inout,
               int (*open_file)(const OptionsContext *, const char *)) {
    int i, ret;

    for (i = 0; i < l->nb_groups; i++) {
        OptionGroup *g = &l->groups[i];
        OptionsContext o;

        init_options(&o);
        o.g = g;

        ret = parse_optgroup(&o, g);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Error parsing options for %s file "
                   "%s.\n",
                   inout, g->arg);
            uninit_options(&o);
            return ret;
        }

        av_log(NULL, AV_LOG_DEBUG, "Opening an %s file: %s.\n", inout, g->arg);
        ret = open_file(&o, g->arg);
        uninit_options(&o);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error opening %s file %s.\n", inout,
                   g->arg);
            return ret;
        }
        av_log(NULL, AV_LOG_DEBUG, "Successfully opened the file.\n");
    }

    return 0;
}

int ffmpeg_parse_options(int argc, char **argv) {
    OptionParseContext octx;
    const char *errmsg = NULL;
    int ret;

    memset(&octx, 0, sizeof(octx));

    /* split the commandline into an internal representation */
    ret = split_commandline(&octx, argc, argv, ffmpeg_options, groups,
                            FF_ARRAY_ELEMS(groups));
    if (ret < 0) {
        errmsg = "splitting the argument list";
        goto fail;
    }

    /* apply global options */
    ret = parse_optgroup(NULL, &octx.global_opts);
    if (ret < 0) {
        errmsg = "parsing global options";
        goto fail;
    }

    /* configure terminal and setup signal handlers */
    term_init();

    /* open input files */
    ret = open_files(&octx.groups[GROUP_INFILE], "input", ifile_open);
    if (ret < 0) {
        errmsg = "opening input files";
        goto fail;
    }

    /* create the complex filtergraphs */
    ret = init_complex_filters();
    if (ret < 0) {
        errmsg = "initializing complex filters";
        goto fail;
    }

    /* open output files */
    ret = open_files(&octx.groups[GROUP_OUTFILE], "output", of_open);
    if (ret < 0) {
        errmsg = "opening output files";
        goto fail;
    }

    correct_input_start_times();

    ret = apply_sync_offsets();
    if (ret < 0)
        goto fail;

    ret = check_filter_outputs();
    if (ret < 0)
        goto fail;

fail:
    uninit_parse_context(&octx);
    if (ret < 0 && ret != AVERROR_EXIT) {
        av_log(NULL, AV_LOG_FATAL, "Error %s: %s\n", errmsg ? errmsg : "",
               av_err2str(ret));
    }
    return ret;
}

int opt_progress(void *optctx, const char *opt, const char *arg) {
    AVIOContext *avio = NULL;
    int ret;

    if (!strcmp(arg, "-"))
        arg = "pipe:";
    ret = avio_open2(&avio, arg, AVIO_FLAG_WRITE, &int_cb, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open progress URL \"%s\": %s\n",
               arg, av_err2str(ret));
        return ret;
    }
    progress_avio = avio;
    return 0;
}

int opt_timelimit(void *optctx, const char *opt, const char *arg) {
#if HAVE_SETRLIMIT
    int ret;
    double lim;
    struct rlimit rl;

    ret = parse_number(opt, arg, OPT_INT64, 0, INT_MAX, &lim);
    if (ret < 0)
        return ret;

    rl = (struct rlimit){lim, lim + 1};
    if (setrlimit(RLIMIT_CPU, &rl))
        perror("setrlimit");
#else
    av_log(NULL, AV_LOG_WARNING, "-%s not implemented on this OS\n", opt);
#endif
    return 0;
}

#if FFMPEG_OPT_QPHIST
int opt_qphist(void *optctx, const char *opt, const char *arg) {
    av_log(NULL, AV_LOG_WARNING, "Option -%s is deprecated and has no effect\n",
           opt);
    return 0;
}
#endif

#if FFMPEG_OPT_ADRIFT_THRESHOLD
int opt_adrift_threshold(void *optctx, const char *opt, const char *arg) {
    av_log(NULL, AV_LOG_WARNING, "Option -%s is deprecated and has no effect\n",
           opt);
    return 0;
}
#endif
