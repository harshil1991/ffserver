#include "myffserver.h"

const char program_name[] = "ffserver";
static const char *my_program_name;
static FFServerConfig config = {
    .nb_max_http_connections = 2000,
    .nb_max_connections = 5,
    .max_bandwidth = 1000,
    .use_defaults = 1,
};
static AVLFG random_state;
static int need_to_start_children;
typedef struct RTSPActionServerSetup {
    uint32_t ipaddr;
    char transport_option[512];
} RTSPActionServerSetup;

void ffserver_free_child_args(void *argsp)
{
    int i;
    char **args;
    if (!argsp)
        return;
    args = *(char ***)argsp;
    if (!args)
        return;
    for (i = 0; i < MAX_CHILD_ARGS; i++)
        av_free(args[i]);
    av_freep(argsp);
}

static void handle_child_exit(int sig)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    }

    need_to_start_children = 1;
}

void ffserver_get_arg(char *buf, int buf_size, const char **pp)
{
    const char *p;
    char *q;
    int quote = 0;

    p = *pp;
    q = buf;

    while (av_isspace(*p)) p++;

    if (*p == '\"' || *p == '\'')
        quote = *p++;

    while (*p != '\0') {
        if (quote && *p == quote || !quote && av_isspace(*p))
            break;
        if ((q - buf) < buf_size - 1)
            *q++ = *p;
        p++;
    }

    *q = '\0';
    if (quote && *p == quote)
        p++;
    *pp = p;
}
/* resolve host with also IP address parsing */
static int resolve_host(struct in_addr *sin_addr, const char *hostname)
{

    if (!ff_inet_aton(hostname, sin_addr)) {
#if HAVE_GETADDRINFO
        struct addrinfo *ai, *cur;
        struct addrinfo hints = { 0 };
        hints.ai_family = AF_INET;
        if (getaddrinfo(hostname, NULL, &hints, &ai))
            return -1;
        /* getaddrinfo returns a linked list of addrinfo structs.
         * Even if we set ai_family = AF_INET above, make sure
         * that the returned one actually is of the correct type. */
        for (cur = ai; cur; cur = cur->ai_next) {
            if (cur->ai_family == AF_INET) {
                *sin_addr = ((struct sockaddr_in *)cur->ai_addr)->sin_addr;
                freeaddrinfo(ai);
                return 0;
            }
        }
        freeaddrinfo(ai);
        return -1;
#else
        struct hostent *hp;
        hp = gethostbyname(hostname);
        if (!hp)
            return -1;
        memcpy(sin_addr, hp->h_addr_list[0], sizeof(struct in_addr));
#endif
    }
    return 0;
}

static int ffserver_set_int_param(int *dest, const char *value, int factor,
                                  int min, int max, FFServerConfig *config,
                                  const char *error_msg, ...)
{
    int tmp;
    char *tailp;
    if (!value || !value[0])
        goto error;
    errno = 0;
    tmp = strtol(value, &tailp, 0);
    if (tmp < min || tmp > max)
        goto error;
    if (factor) {
        if (tmp == INT_MIN || FFABS(tmp) > INT_MAX / FFABS(factor))
            goto error;
        tmp *= factor;
    }
    if (tailp[0] || errno)
        goto error;
    if (dest)
        *dest = tmp;
    return 0;
  error:
    if (config) {
        va_list vl;
        va_start(vl, error_msg);
        vreport_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                &config->errors, error_msg, vl);
        va_end(vl);
    }
    return AVERROR(EINVAL);
}


static AVOutputFormat *ffserver_guess_format(const char *short_name,
                                             const char *filename,
                                             const char *mime_type)
{
    AVOutputFormat *fmt = av_guess_format(short_name, filename, mime_type);

    if (fmt) {
        AVOutputFormat *stream_fmt;
        char stream_format_name[64];

        snprintf(stream_format_name, sizeof(stream_format_name), "%s_stream",
                fmt->name);
        stream_fmt = av_guess_format(stream_format_name, NULL, NULL);

        if (stream_fmt)
            fmt = stream_fmt;
    }

    return fmt;
}

static int ffserver_set_codec(AVCodecContext *ctx, const char *codec_name,
                              FFServerConfig *config)
{
    int ret;
    AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec || codec->type != ctx->codec_type) {
        report_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                            &config->errors,
                            "Invalid codec name: '%s'\n", codec_name);
        return 0;
    }
    if (ctx->codec_id == AV_CODEC_ID_NONE && !ctx->priv_data) {
        if ((ret = avcodec_get_context_defaults3(ctx, codec)) < 0)
            return ret;
        ctx->codec = codec;
    }
    if (ctx->codec_id != codec->id)
        report_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                            &config->errors,
                            "Inconsistent configuration: trying to set '%s' "
                            "codec option, but '%s' codec is used previously\n",
                            codec_name, avcodec_get_name(ctx->codec_id));
    return 0;
}

static int ffserver_save_avoption(const char *opt, const char *arg, int type,
                                  FFServerConfig *config)
{
    static int hinted = 0;
    int ret = 0;
    AVDictionaryEntry *e;
    const AVOption *o = NULL;
    const char *option = NULL;
    const char *codec_name = NULL;
    char buff[1024];
    AVCodecContext *ctx;
    AVDictionary **dict;
    enum AVCodecID guessed_codec_id;

    switch (type) {
    case AV_OPT_FLAG_VIDEO_PARAM:
        ctx = config->dummy_vctx;
        dict = &config->video_opts;
        guessed_codec_id = config->guessed_video_codec_id != AV_CODEC_ID_NONE ?
                           config->guessed_video_codec_id : AV_CODEC_ID_H264;
        break;
    case AV_OPT_FLAG_AUDIO_PARAM:
        ctx = config->dummy_actx;
        dict = &config->audio_opts;
        guessed_codec_id = config->guessed_audio_codec_id != AV_CODEC_ID_NONE ?
                           config->guessed_audio_codec_id : AV_CODEC_ID_AAC;
        break;
    default:
        av_assert0(0);
    }

    if (strchr(opt, ':')) {
        //explicit private option
        snprintf(buff, sizeof(buff), "%s", opt);
        codec_name = buff;
        if(!(option = strchr(buff, ':'))){
            report_config_error(config->filename, config->line_num,
                                AV_LOG_ERROR, &config->errors,
                                "Syntax error. Unmatched ':'\n");
            return -1;

        }
        buff[option - buff] = '\0';
        option++;
        if ((ret = ffserver_set_codec(ctx, codec_name, config)) < 0)
            return ret;
        if (!ctx->codec || !ctx->priv_data)
            return -1;
    } else {
        option = opt;
    }

    o = av_opt_find(ctx, option, NULL, type | AV_OPT_FLAG_ENCODING_PARAM,
                    AV_OPT_SEARCH_CHILDREN);
    if (!o &&
        (!strcmp(option, "time_base")  || !strcmp(option, "pixel_format") ||
         !strcmp(option, "video_size") || !strcmp(option, "codec_tag")))
        o = av_opt_find(ctx, option, NULL, 0, 0);
    if (!o) {
        report_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                            &config->errors, "Option not found: '%s'\n", opt);
        if (!hinted && ctx->codec_id == AV_CODEC_ID_NONE) {
            hinted = 1;
            report_config_error(config->filename, config->line_num,
                                AV_LOG_ERROR, NULL, "If '%s' is a codec private"
                                "option, then prefix it with codec name, for "
                                "example '%s:%s %s' or define codec earlier.\n",
                                opt, avcodec_get_name(guessed_codec_id) ,opt,
                                arg);
        }
    } else if ((ret = av_opt_set(ctx, option, arg, AV_OPT_SEARCH_CHILDREN)) < 0) {
        report_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                &config->errors, "Invalid value for option %s (%s): %s\n", opt,
                arg, av_err2str(ret));
    } else if ((e = av_dict_get(*dict, option, NULL, 0))) {
        if ((o->type == AV_OPT_TYPE_FLAGS) && arg &&
            (arg[0] == '+' || arg[0] == '-'))
            return av_dict_set(dict, option, arg, AV_DICT_APPEND);
        report_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                            &config->errors, "Redeclaring value of option '%s'."
                            "Previous value was: '%s'.\n", opt, e->value);
    } else if (av_dict_set(dict, option, arg, 0) < 0) {
        return AVERROR(ENOMEM);
    }
    return 0;
}



FILE *get_preset_file(char *filename, size_t filename_size,
                      const char *preset_name, int is_path,
                      const char *codec_name)
{
    FILE *f = NULL;
    int i;
    const char *base[3] = { getenv("FFMPEG_DATADIR"),
                            getenv("HOME"),
                            FFMPEG_DATADIR, };

    if (is_path) {
        av_strlcpy(filename, preset_name, filename_size);
        f = fopen(filename, "r");
    } else {
#ifdef _WIN32
        char datadir[MAX_PATH], *ls;
        base[2] = NULL;

        if (GetModuleFileNameA(GetModuleHandleA(NULL), datadir, sizeof(datadir) - 1))
        {
            for (ls = datadir; ls < datadir + strlen(datadir); ls++)
                if (*ls == '\\') *ls = '/';

            if (ls = strrchr(datadir, '/'))
            {
                *ls = 0;
                strncat(datadir, "/ffpresets",  sizeof(datadir) - 1 - strlen(datadir));
                base[2] = datadir;
            }
        }
#endif
        for (i = 0; i < 3 && !f; i++) {
            if (!base[i])
                continue;
            snprintf(filename, filename_size, "%s%s/%s.ffpreset", base[i],
                     i != 1 ? "" : "/.ffmpeg", preset_name);
            f = fopen(filename, "r");
            if (!f && codec_name) {
                snprintf(filename, filename_size,
                         "%s%s/%s-%s.ffpreset",
                         base[i], i != 1 ? "" : "/.ffmpeg", codec_name,
                         preset_name);
                f = fopen(filename, "r");
            }
        }
    }

    return f;
}

static int ffserver_opt_preset(const char *arg, int type, FFServerConfig *config)
{
    FILE *f=NULL;
    char filename[1000], tmp[1000], tmp2[1000], line[1000];
    int ret = 0;
    AVCodecContext *avctx;
    const AVCodec *codec;

    switch(type) {
    case AV_OPT_FLAG_AUDIO_PARAM:
        avctx = config->dummy_actx;
        break;
    case AV_OPT_FLAG_VIDEO_PARAM:
        avctx = config->dummy_vctx;
        break;
    default:
        av_assert0(0);
    }
    codec = avcodec_find_encoder(avctx->codec_id);

    if (!(f = get_preset_file(filename, sizeof(filename), arg, 0,
                              codec ? codec->name : NULL))) {
        av_log(NULL, AV_LOG_ERROR, "File for preset '%s' not found\n", arg);
        return AVERROR(EINVAL);
    }

    while(!feof(f)){
        int e= fscanf(f, "%999[^\n]\n", line) - 1;
        if(line[0] == '#' && !e)
            continue;
        e|= sscanf(line, "%999[^=]=%999[^\n]\n", tmp, tmp2) - 2;
        if(e){
            av_log(NULL, AV_LOG_ERROR, "%s: Invalid syntax: '%s'\n", filename,
                   line);
            ret = AVERROR(EINVAL);
            break;
        }
        if (!strcmp(tmp, "acodec") && avctx->codec_type == AVMEDIA_TYPE_AUDIO ||
            !strcmp(tmp, "vcodec") && avctx->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            if (ffserver_set_codec(avctx, tmp2, config) < 0)
                break;
        } else if (!strcmp(tmp, "scodec")) {
            av_log(NULL, AV_LOG_ERROR, "Subtitles preset found.\n");
            ret = AVERROR(EINVAL);
            break;
        } else if (ffserver_save_avoption(tmp, tmp2, type, config) < 0)
            break;
    }

    fclose(f);

    return ret;
}

static int ffserver_save_avoption_int(const char *opt, int64_t arg,
                                      int type, FFServerConfig *config)
{
    char buf[22];
    snprintf(buf, sizeof(buf), "%"PRId64, arg);
    return ffserver_save_avoption(opt, buf, type, config);
}

static int ffserver_set_float_param(float *dest, const char *value,
        float factor, float min, float max,
        FFServerConfig *config,
        const char *error_msg, ...)
{
    double tmp;
    char *tailp;
    if (!value || !value[0])
        goto error;
    errno = 0;
    tmp = strtod(value, &tailp);
    if (tmp < min || tmp > max)
        goto error;
    if (factor)
        tmp *= factor;
    if (tailp[0] || errno)
        goto error;
    if (dest)
        *dest = tmp;
    return 0;
error:
    if (config) {
        va_list vl;
        va_start(vl, error_msg);
        vreport_config_error(config->filename, config->line_num, AV_LOG_ERROR,
                &config->errors, error_msg, vl);
        va_end(vl);
    }
    return AVERROR(EINVAL);
}

static int ffserver_parse_config_global(FFServerConfig *config, const char *cmd,
                                        const char **p)
{
    int val;
    char arg[1024];
    if (!av_strcasecmp(cmd, "Port") || !av_strcasecmp(cmd, "HTTPPort")) {
        if (!av_strcasecmp(cmd, "Port"))
            WARNING("Port option is deprecated. Use HTTPPort instead.\n");
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, 1, 65535, config,
                "Invalid port: %s\n", arg);
        if (val < 1024)
            WARNING("Trying to use IETF assigned system port: '%d'\n", val);
        config->http_addr.sin_port = htons(val);
    } else if (!av_strcasecmp(cmd, "HTTPBindAddress") ||
               !av_strcasecmp(cmd, "BindAddress")) {
        if (!av_strcasecmp(cmd, "BindAddress"))
            WARNING("BindAddress option is deprecated. Use HTTPBindAddress "
                    "instead.\n");
        ffserver_get_arg(arg, sizeof(arg), p);
        if (resolve_host(&config->http_addr.sin_addr, arg))
            ERROR("Invalid host/IP address: '%s'\n", arg);
    } else if (!av_strcasecmp(cmd, "NoDaemon")) {
        WARNING("NoDaemon option has no effect. You should remove it.\n");
    } else if (!av_strcasecmp(cmd, "RTSPPort")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, 1, 65535, config,
                "Invalid port: %s\n", arg);
        config->rtsp_addr.sin_port = htons(val);
    } else if (!av_strcasecmp(cmd, "RTSPBindAddress")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (resolve_host(&config->rtsp_addr.sin_addr, arg))
            ERROR("Invalid host/IP address: %s\n", arg);
    } else if (!av_strcasecmp(cmd, "MaxHTTPConnections")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, 1, 65535, config,
                "Invalid MaxHTTPConnections: %s\n", arg);
        config->nb_max_http_connections = val;
        if (config->nb_max_connections > config->nb_max_http_connections) {
            ERROR("Inconsistent configuration: MaxClients(%d) > "
                  "MaxHTTPConnections(%d)\n", config->nb_max_connections,
                  config->nb_max_http_connections);
        }
    } else if (!av_strcasecmp(cmd, "MaxClients")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, 1, 65535, config,
                "Invalid MaxClients: '%s'\n", arg);
        config->nb_max_connections = val;
        if (config->nb_max_connections > config->nb_max_http_connections) {
            ERROR("Inconsistent configuration: MaxClients(%d) > "
                  "MaxHTTPConnections(%d)\n", config->nb_max_connections,
                  config->nb_max_http_connections);
        }
    } else if (!av_strcasecmp(cmd, "MaxBandwidth")) {
        int64_t llval;
        char *tailp;
        ffserver_get_arg(arg, sizeof(arg), p);
        errno = 0;
        llval = strtoll(arg, &tailp, 10);
        if (llval < 10 || llval > 10000000 || tailp[0] || errno)
            ERROR("Invalid MaxBandwidth: '%s'\n", arg);
        else
            config->max_bandwidth = llval;
    } else if (!av_strcasecmp(cmd, "CustomLog")) {
        if (!config->debug) {
            ffserver_get_arg(config->logfilename, sizeof(config->logfilename),
                             p);
        }
    } else if (!av_strcasecmp(cmd, "LoadModule")) {
        ERROR("Loadable modules are no longer supported\n");
    } else if (!av_strcasecmp(cmd, "NoDefaults")) {
        config->use_defaults = 0;
    } else if (!av_strcasecmp(cmd, "UseDefaults")) {
        config->use_defaults = 1;
    } else
        ERROR("Incorrect keyword: '%s'\n", cmd);
    return 0;
}

static void ffserver_parse_acl_row(FFServerStream *stream, FFServerStream* feed,
                            FFServerIPAddressACL *ext_acl,
                            const char *p, const char *filename, int line_num)
{
    char arg[1024];
    FFServerIPAddressACL acl;
    FFServerIPAddressACL *nacl;
    FFServerIPAddressACL **naclp;

    ffserver_get_arg(arg, sizeof(arg), &p);
    if (av_strcasecmp(arg, "allow") == 0)
        acl.action = IP_ALLOW;
    else if (av_strcasecmp(arg, "deny") == 0)
        acl.action = IP_DENY;
    else {
        fprintf(stderr, "%s:%d: ACL action '%s' should be ALLOW or DENY.\n",
                filename, line_num, arg);
        goto bail;
    }

    ffserver_get_arg(arg, sizeof(arg), &p);

    if (resolve_host(&acl.first, arg)) {
        fprintf(stderr,
                "%s:%d: ACL refers to invalid host or IP address '%s'\n",
                filename, line_num, arg);
        goto bail;
    }

    acl.last = acl.first;

    ffserver_get_arg(arg, sizeof(arg), &p);

    if (arg[0]) {
        if (resolve_host(&acl.last, arg)) {
            fprintf(stderr,
                    "%s:%d: ACL refers to invalid host or IP address '%s'\n",
                    filename, line_num, arg);
            goto bail;
        }
    }

    nacl = av_mallocz(sizeof(*nacl));
    if (!nacl) {
        fprintf(stderr, "Failed to allocate FFServerIPAddressACL\n");
        goto bail;
    }

    naclp = 0;

    acl.next = 0;
    *nacl = acl;

    if (stream)
        naclp = &stream->acl;
    else if (feed)
        naclp = &feed->acl;
    else if (ext_acl)
        naclp = &ext_acl;
    else
        fprintf(stderr, "%s:%d: ACL found not in <Stream> or <Feed>\n",
                filename, line_num);

    if (naclp) {
        while (*naclp)
            naclp = &(*naclp)->next;

        *naclp = nacl;
    } else
        av_free(nacl);

bail:
  return;

}

/* add a codec and set the default parameters */
static void add_codec(FFServerStream *stream, AVCodecContext *av,
                      FFServerConfig *config)
{
    LayeredAVStream *st;
    AVDictionary **opts, *recommended = NULL;
    char *enc_config;

    if(stream->nb_streams >= FF_ARRAY_ELEMS(stream->streams))
        return;

    opts = av->codec_type == AVMEDIA_TYPE_AUDIO ?
           &config->audio_opts : &config->video_opts;
    av_dict_copy(&recommended, *opts, 0);
    av_opt_set_dict2(av->priv_data, opts, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_dict2(av, opts, AV_OPT_SEARCH_CHILDREN);

    if (av_dict_count(*opts))
        av_log(NULL, AV_LOG_WARNING,
               "Something is wrong, %d options are not set!\n",
               av_dict_count(*opts));

    if (!config->stream_use_defaults) {
        switch(av->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            if (av->bit_rate == 0)
                report_config_error(config->filename, config->line_num,
                                    AV_LOG_ERROR, &config->errors,
                                    "audio bit rate is not set\n");
            if (av->sample_rate == 0)
                report_config_error(config->filename, config->line_num,
                                    AV_LOG_ERROR, &config->errors,
                                    "audio sample rate is not set\n");
            break;
        case AVMEDIA_TYPE_VIDEO:
            if (av->width == 0 || av->height == 0)
                report_config_error(config->filename, config->line_num,
                                    AV_LOG_ERROR, &config->errors,
                                    "video size is not set\n");
            break;
        default:
            av_assert0(0);
        }
        goto done;
    }

    /* stream_use_defaults = true */

    /* compute default parameters */
    switch(av->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        if (!av_dict_get(recommended, "b", NULL, 0)) {
            av->bit_rate = 64000;
            av_dict_set_int(&recommended, "b", av->bit_rate, 0);
            WARNING("Setting default value for audio bit rate = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->bit_rate);
        }
        if (!av_dict_get(recommended, "ar", NULL, 0)) {
            av->sample_rate = 22050;
            av_dict_set_int(&recommended, "ar", av->sample_rate, 0);
            WARNING("Setting default value for audio sample rate = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->sample_rate);
        }
        if (!av_dict_get(recommended, "ac", NULL, 0)) {
            av->channels = 1;
            av_dict_set_int(&recommended, "ac", av->channels, 0);
            WARNING("Setting default value for audio channel count = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->channels);
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        if (!av_dict_get(recommended, "b", NULL, 0)) {
            av->bit_rate = 64000;
            av_dict_set_int(&recommended, "b", av->bit_rate, 0);
            WARNING("Setting default value for video bit rate = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->bit_rate);
        }
        if (!av_dict_get(recommended, "time_base", NULL, 0)){
            av->time_base.den = 5;
            av->time_base.num = 1;
            av_dict_set(&recommended, "time_base", "1/5", 0);
            WARNING("Setting default value for video frame rate = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->time_base.den);
        }
        if (!av_dict_get(recommended, "video_size", NULL, 0)) {
            av->width = 160;
            av->height = 128;
            av_dict_set(&recommended, "video_size", "160x128", 0);
            WARNING("Setting default value for video size = %dx%d. "
                    "Use NoDefaults to disable it.\n",
                    av->width, av->height);
        }
        /* Bitrate tolerance is less for streaming */
        if (!av_dict_get(recommended, "bt", NULL, 0)) {
            av->bit_rate_tolerance = FFMAX(av->bit_rate / 4,
                      (int64_t)av->bit_rate*av->time_base.num/av->time_base.den);
            av_dict_set_int(&recommended, "bt", av->bit_rate_tolerance, 0);
            WARNING("Setting default value for video bit rate tolerance = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->bit_rate_tolerance);
        }

        if (!av_dict_get(recommended, "rc_eq", NULL, 0)) {
            av->rc_eq = av_strdup("tex^qComp");
            av_dict_set(&recommended, "rc_eq", "tex^qComp", 0);
            WARNING("Setting default value for video rate control equation = "
                    "%s. Use NoDefaults to disable it.\n",
                    av->rc_eq);
        }
        if (!av_dict_get(recommended, "maxrate", NULL, 0)) {
            av->rc_max_rate = av->bit_rate * 2;
            av_dict_set_int(&recommended, "maxrate", av->rc_max_rate, 0);
            WARNING("Setting default value for video max rate = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->rc_max_rate);
        }

        if (av->rc_max_rate && !av_dict_get(recommended, "bufsize", NULL, 0)) {
            av->rc_buffer_size = av->rc_max_rate;
            av_dict_set_int(&recommended, "bufsize", av->rc_buffer_size, 0);
            WARNING("Setting default value for video buffer size = %d. "
                    "Use NoDefaults to disable it.\n",
                    av->rc_buffer_size);
        }
        break;
    default:
        abort();
    }

done:
    st = av_mallocz(sizeof(*st));
    if (!st)
        return;
    av_dict_get_string(recommended, &enc_config, '=', ',');
    av_dict_free(&recommended);
    st->recommended_encoder_configuration = enc_config;
    st->codec = av;
    st->codecpar = avcodec_parameters_alloc();
    avcodec_parameters_from_context(st->codecpar, av);
    stream->streams[stream->nb_streams++] = st;
}

static int ffserver_parse_config_stream(FFServerConfig *config, const char *cmd,
                                        const char **p,
                                        FFServerStream **pstream)
{
    char arg[1024], arg2[1024];
    FFServerStream *stream;
    int val;

    av_assert0(pstream);
    stream = *pstream;

    if (!av_strcasecmp(cmd, "<Stream")) {
        char *q;
        FFServerStream *s;
        stream = av_mallocz(sizeof(FFServerStream));
        if (!stream)
            return AVERROR(ENOMEM);
        config->dummy_actx = avcodec_alloc_context3(NULL);
        config->dummy_vctx = avcodec_alloc_context3(NULL);
        if (!config->dummy_vctx || !config->dummy_actx) {
            av_free(stream);
            avcodec_free_context(&config->dummy_vctx);
            avcodec_free_context(&config->dummy_actx);
            return AVERROR(ENOMEM);
        }
        config->dummy_actx->codec_type = AVMEDIA_TYPE_AUDIO;
        config->dummy_vctx->codec_type = AVMEDIA_TYPE_VIDEO;
        ffserver_get_arg(stream->filename, sizeof(stream->filename), p);
        q = strrchr(stream->filename, '>');
        if (q)
            *q = '\0';

        for (s = config->first_stream; s; s = s->next) {
            if (!strcmp(stream->filename, s->filename))
                ERROR("Stream '%s' already registered\n", s->filename);
        }

        stream->fmt = ffserver_guess_format(NULL, stream->filename, NULL);
        if (stream->fmt) {
            config->guessed_audio_codec_id = stream->fmt->audio_codec;
            config->guessed_video_codec_id = stream->fmt->video_codec;
        } else {
            config->guessed_audio_codec_id = AV_CODEC_ID_NONE;
            config->guessed_video_codec_id = AV_CODEC_ID_NONE;
        }
        config->stream_use_defaults = config->use_defaults;
        *pstream = stream;
        return 0;
    }
    av_assert0(stream);
    if (!av_strcasecmp(cmd, "Feed")) {
        FFServerStream *sfeed;
        ffserver_get_arg(arg, sizeof(arg), p);
        sfeed = config->first_feed;
        while (sfeed) {
            if (!strcmp(sfeed->filename, arg))
                break;
            sfeed = sfeed->next_feed;
        }
        if (!sfeed)
            ERROR("Feed with name '%s' for stream '%s' is not defined\n", arg,
                    stream->filename);
        else
            stream->feed = sfeed;
    } else if (!av_strcasecmp(cmd, "Format")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (!strcmp(arg, "status")) {
            stream->stream_type = STREAM_TYPE_STATUS;
            stream->fmt = NULL;
        } else {
            stream->stream_type = STREAM_TYPE_LIVE;
            /* JPEG cannot be used here, so use single frame MJPEG */
            if (!strcmp(arg, "jpeg")) {
                strcpy(arg, "singlejpeg");
                stream->single_frame=1;
            }
            stream->fmt = ffserver_guess_format(arg, NULL, NULL);
            if (!stream->fmt)
                ERROR("Unknown Format: '%s'\n", arg);
        }
        if (stream->fmt) {
            config->guessed_audio_codec_id = stream->fmt->audio_codec;
            config->guessed_video_codec_id = stream->fmt->video_codec;
        }
    } else if (!av_strcasecmp(cmd, "InputFormat")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        stream->ifmt = av_find_input_format(arg);
        if (!stream->ifmt)
            ERROR("Unknown input format: '%s'\n", arg);
    } else if (!av_strcasecmp(cmd, "FaviconURL")) {
        if (stream->stream_type == STREAM_TYPE_STATUS)
            ffserver_get_arg(stream->feed_filename,
                    sizeof(stream->feed_filename), p);
        else
            ERROR("FaviconURL only permitted for status streams\n");
    } else if (!av_strcasecmp(cmd, "Author")    ||
               !av_strcasecmp(cmd, "Comment")   ||
               !av_strcasecmp(cmd, "Copyright") ||
               !av_strcasecmp(cmd, "Title")) {
        char key[32];
        int i;
        ffserver_get_arg(arg, sizeof(arg), p);
        for (i = 0; i < strlen(cmd); i++)
            key[i] = av_tolower(cmd[i]);
        key[i] = 0;
        WARNING("Deprecated '%s' option in configuration file. Use "
                "'Metadata %s VALUE' instead.\n", cmd, key);
        if (av_dict_set(&stream->metadata, key, arg, 0) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "Metadata")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_get_arg(arg2, sizeof(arg2), p);
        if (av_dict_set(&stream->metadata, arg, arg2, 0) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "Preroll")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        stream->prebuffer = atof(arg) * 1000;
    } else if (!av_strcasecmp(cmd, "StartSendOnKey")) {
        stream->send_on_key = 1;
    } else if (!av_strcasecmp(cmd, "AudioCodec")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_codec(config->dummy_actx, arg, config);
    } else if (!av_strcasecmp(cmd, "VideoCodec")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_codec(config->dummy_vctx, arg, config);
    } else if (!av_strcasecmp(cmd, "MaxTime")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        stream->max_time = atof(arg) * 1000;
    } else if (!av_strcasecmp(cmd, "AudioBitRate")) {
        float f;
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_float_param(&f, arg, 1000, -FLT_MAX, FLT_MAX, config,
                "Invalid %s: '%s'\n", cmd, arg);
        if (ffserver_save_avoption_int("b", (int64_t)lrintf(f),
                                       AV_OPT_FLAG_AUDIO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "AudioChannels")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("ac", arg, AV_OPT_FLAG_AUDIO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "AudioSampleRate")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("ar", arg, AV_OPT_FLAG_AUDIO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoBitRateRange")) {
        int minrate, maxrate;
        char *dash;
        ffserver_get_arg(arg, sizeof(arg), p);
        dash = strchr(arg, '-');
        if (dash) {
            *dash = '\0';
            dash++;
            if (ffserver_set_int_param(&minrate, arg,  1000, 0, INT_MAX, config, "Invalid %s: '%s'", cmd, arg) >= 0 &&
                ffserver_set_int_param(&maxrate, dash, 1000, 0, INT_MAX, config, "Invalid %s: '%s'", cmd, arg) >= 0) {
                if (ffserver_save_avoption_int("minrate", minrate, AV_OPT_FLAG_VIDEO_PARAM, config) < 0 ||
                    ffserver_save_avoption_int("maxrate", maxrate, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
                goto nomem;
            }
        } else
            ERROR("Incorrect format for VideoBitRateRange. It should be "
                  "<min>-<max>: '%s'.\n", arg);
    } else if (!av_strcasecmp(cmd, "Debug")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("debug", arg, AV_OPT_FLAG_AUDIO_PARAM, config) < 0 ||
            ffserver_save_avoption("debug", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "Strict")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("strict", arg, AV_OPT_FLAG_AUDIO_PARAM, config) < 0 ||
            ffserver_save_avoption("strict", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoBufferSize")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 8*1024, 0, INT_MAX, config,
                "Invalid %s: '%s'", cmd, arg);
        if (ffserver_save_avoption_int("bufsize", val, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoBitRateTolerance")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 1000, INT_MIN, INT_MAX, config,
                               "Invalid %s: '%s'", cmd, arg);
        if (ffserver_save_avoption_int("bt", val, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoBitRate")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 1000, INT_MIN, INT_MAX, config,
                               "Invalid %s: '%s'", cmd, arg);
        if (ffserver_save_avoption_int("b", val, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
           goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoSize")) {
        int ret, w, h;
        ffserver_get_arg(arg, sizeof(arg), p);
        ret = av_parse_video_size(&w, &h, arg);
        if (ret < 0)
            ERROR("Invalid video size '%s'\n", arg);
        else {
            if (w % 2 || h % 2)
                WARNING("Image size is not a multiple of 2\n");
            if (ffserver_save_avoption("video_size", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
                goto nomem;
        }
    } else if (!av_strcasecmp(cmd, "VideoFrameRate")) {
        ffserver_get_arg(&arg[2], sizeof(arg) - 2, p);
        arg[0] = '1'; arg[1] = '/';
        if (ffserver_save_avoption("time_base", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "PixelFormat")) {
        enum AVPixelFormat pix_fmt;
        ffserver_get_arg(arg, sizeof(arg), p);
        pix_fmt = av_get_pix_fmt(arg);
        if (pix_fmt == AV_PIX_FMT_NONE)
            ERROR("Unknown pixel format: '%s'\n", arg);
        else if (ffserver_save_avoption("pixel_format", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoGopSize")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("g", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoIntraOnly")) {
        if (ffserver_save_avoption("g", "1", AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoHighQuality")) {
        if (ffserver_save_avoption("mbd", "+bits", AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "Video4MotionVector")) {
        if (ffserver_save_avoption("mbd", "+bits",  AV_OPT_FLAG_VIDEO_PARAM, config) < 0 || //FIXME remove
            ffserver_save_avoption("flags", "+mv4", AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "AVOptionVideo") ||
               !av_strcasecmp(cmd, "AVOptionAudio")) {
        int ret;
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_get_arg(arg2, sizeof(arg2), p);
        if (!av_strcasecmp(cmd, "AVOptionVideo"))
            ret = ffserver_save_avoption(arg, arg2, AV_OPT_FLAG_VIDEO_PARAM,
                                         config);
        else
            ret = ffserver_save_avoption(arg, arg2, AV_OPT_FLAG_AUDIO_PARAM,
                                         config);
        if (ret < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "AVPresetVideo") ||
               !av_strcasecmp(cmd, "AVPresetAudio")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (!av_strcasecmp(cmd, "AVPresetVideo"))
            ffserver_opt_preset(arg, AV_OPT_FLAG_VIDEO_PARAM, config);
        else
            ffserver_opt_preset(arg, AV_OPT_FLAG_AUDIO_PARAM, config);
    } else if (!av_strcasecmp(cmd, "VideoTag")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (strlen(arg) == 4 &&
            ffserver_save_avoption_int("codec_tag",
                                       MKTAG(arg[0], arg[1], arg[2], arg[3]),
                                       AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "BitExact")) {
        config->bitexact = 1;
        if (ffserver_save_avoption("flags", "+bitexact", AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "DctFastint")) {
        if (ffserver_save_avoption("dct", "fastint", AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "IdctSimple")) {
        if (ffserver_save_avoption("idct", "simple", AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "Qscale")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, INT_MIN, INT_MAX, config,
                               "Invalid Qscale: '%s'\n", arg);
        if (ffserver_save_avoption("flags", "+qscale", AV_OPT_FLAG_VIDEO_PARAM, config) < 0 ||
            ffserver_save_avoption_int("global_quality", FF_QP2LAMBDA * val,
                                       AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoQDiff")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("qdiff", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoQMax")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("qmax", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "VideoQMin")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("qmin", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "LumiMask")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("lumi_mask", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "DarkMask")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (ffserver_save_avoption("dark_mask", arg, AV_OPT_FLAG_VIDEO_PARAM, config) < 0)
            goto nomem;
    } else if (!av_strcasecmp(cmd, "NoVideo")) {
        config->no_video = 1;
    } else if (!av_strcasecmp(cmd, "NoAudio")) {
        config->no_audio = 1;
    } else if (!av_strcasecmp(cmd, "ACL")) {
        ffserver_parse_acl_row(stream, NULL, NULL, *p, config->filename,
                config->line_num);
    } else if (!av_strcasecmp(cmd, "DynamicACL")) {
        ffserver_get_arg(stream->dynamic_acl, sizeof(stream->dynamic_acl), p);
    } else if (!av_strcasecmp(cmd, "RTSPOption")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        av_freep(&stream->rtsp_option);
        stream->rtsp_option = av_strdup(arg);
    } else if (!av_strcasecmp(cmd, "MulticastAddress")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        if (resolve_host(&stream->multicast_ip, arg))
            ERROR("Invalid host/IP address: '%s'\n", arg);
        stream->is_multicast = 1;
        stream->loop = 1; /* default is looping */
    } else if (!av_strcasecmp(cmd, "MulticastPort")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, 1, 65535, config,
                "Invalid MulticastPort: '%s'\n", arg);
        stream->multicast_port = val;
    } else if (!av_strcasecmp(cmd, "MulticastTTL")) {
        ffserver_get_arg(arg, sizeof(arg), p);
        ffserver_set_int_param(&val, arg, 0, INT_MIN, INT_MAX, config,
                "Invalid MulticastTTL: '%s'\n", arg);
        stream->multicast_ttl = val;
    } else if (!av_strcasecmp(cmd, "NoLoop")) {
        stream->loop = 0;
    } else if (!av_strcasecmp(cmd, "</Stream>")) {
        config->stream_use_defaults &= 1;
        if (stream->feed && stream->fmt && strcmp(stream->fmt->name, "ffm")) {
            if (config->dummy_actx->codec_id == AV_CODEC_ID_NONE)
                config->dummy_actx->codec_id = config->guessed_audio_codec_id;
            if (!config->no_audio &&
                config->dummy_actx->codec_id != AV_CODEC_ID_NONE) {
                AVCodecContext *audio_enc = avcodec_alloc_context3(avcodec_find_encoder(config->dummy_actx->codec_id));
                add_codec(stream, audio_enc, config);
            }
            if (config->dummy_vctx->codec_id == AV_CODEC_ID_NONE)
                config->dummy_vctx->codec_id = config->guessed_video_codec_id;
            if (!config->no_video &&
                config->dummy_vctx->codec_id != AV_CODEC_ID_NONE) {
                AVCodecContext *video_enc = avcodec_alloc_context3(avcodec_find_encoder(config->dummy_vctx->codec_id));
                add_codec(stream, video_enc, config);
            }
        }
        av_dict_free(&config->video_opts);
        av_dict_free(&config->audio_opts);
        avcodec_free_context(&config->dummy_vctx);
        avcodec_free_context(&config->dummy_actx);
        config->no_video = 0;
        config->no_audio = 0;
        *pstream = NULL;
    } else if (!av_strcasecmp(cmd, "File") ||
               !av_strcasecmp(cmd, "ReadOnlyFile")) {
        ffserver_get_arg(stream->feed_filename, sizeof(stream->feed_filename),
                p);
    } else if (!av_strcasecmp(cmd, "UseDefaults")) {
        if (config->stream_use_defaults > 1)
            WARNING("Multiple UseDefaults/NoDefaults entries.\n");
        config->stream_use_defaults = 3;
    } else if (!av_strcasecmp(cmd, "NoDefaults")) {
        if (config->stream_use_defaults > 1)
            WARNING("Multiple UseDefaults/NoDefaults entries.\n");
        config->stream_use_defaults = 2;
    } else {
        ERROR("Invalid entry '%s' inside <Stream></Stream>\n", cmd);
    }
    return 0;
  nomem:
    av_log(NULL, AV_LOG_ERROR, "Out of memory. Aborting.\n");
    av_dict_free(&config->video_opts);
    av_dict_free(&config->audio_opts);
    avcodec_free_context(&config->dummy_vctx);
    avcodec_free_context(&config->dummy_actx);
    return AVERROR(ENOMEM);
}

int ffserver_parse_ffconfig(const char *filename, FFServerConfig *config)
{
    FILE *f;
    char line[1024];
    char cmd[64];
    const char *p;
    FFServerStream **last_stream, *stream = NULL;
    int ret = 0;

    av_assert0(config);

    f = fopen(filename, "r");
    if (!f) {
        ret = AVERROR(errno);
        av_log(NULL, AV_LOG_ERROR,
                "Could not open the configuration file '%s'\n", filename);
        return ret;
    }

    config->first_stream = NULL;
    config->errors = config->warnings = 0;

    last_stream = &config->first_stream;

    config->line_num = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        config->line_num++;
        p = line;
        while (av_isspace(*p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        ffserver_get_arg(cmd, sizeof(cmd), &p);

        if (stream || !av_strcasecmp(cmd, "<Stream")) {
            int opening = !av_strcasecmp(cmd, "<Stream");
            if (opening && (stream)) {
                ERROR("Already in a tag\n");
            } else {
                ret = ffserver_parse_config_stream(config, cmd, &p, &stream);
                if (ret < 0)
                    break;
                if (opening) {
                    /* add in stream list */
                    *last_stream = stream;
                    last_stream = &stream->next;
                }
            }
        }  else {
            ffserver_parse_config_global(config, cmd, &p);
        }
    }
    if (stream)
        ERROR("Missing closing </%s> tag\n",
                stream ? "Stream" : "Redirect");

    fclose(f);
    if (ret < 0)
        return ret;
    if (config->errors)
        return AVERROR(EINVAL);
    else
        return 0;
}

static int read_random(uint32_t *dst, const char *file)
{
#if HAVE_UNISTD_H
    int fd = avpriv_open(file, O_RDONLY);
    int err = -1;

    if (fd == -1)
        return -1;
    err = read(fd, dst, sizeof(*dst));
    close(fd);

    return err;
#else
    return -1;
#endif
}

uint32_t av_get_random_seed(void)
{
    uint32_t seed;

    if (read_random(&seed, "/dev/urandom") == sizeof(seed))
        return seed;
    if (read_random(&seed, "/dev/random")  == sizeof(seed))
        return seed;
    return 123456564;
}

static char *ctime1(char *buf2, size_t buf_size)
{
    time_t ti;
    char *p;

    ti = time(NULL);
    p = ctime(&ti);
    if (!p || !*p) {
        *buf2 = '\0';
        return buf2;
    }
    av_strlcpy(buf2, p, buf_size);
    p = buf2 + strlen(buf2) - 1;
    if (*p == '\n')
        *p = '\0';
    return buf2;
}

static void http_vlog(const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
    char buf[32];

    if (!logfile)
        return;

    if (print_prefix) {
        ctime1(buf, sizeof(buf));
        fprintf(logfile, "%s ", buf);
    }
    print_prefix = strstr(fmt, "\n") != NULL;
    vfprintf(logfile, fmt, vargs);
    fflush(logfile);
}

static void http_log(const char *fmt, ...)
{
    va_list vargs;
    va_start(vargs, fmt);
    http_vlog(fmt, vargs);
    va_end(vargs);
}

static void http_av_log(void *ptr, int level, const char *fmt, va_list vargs)
{
    static int print_prefix = 1;
    AVClass *avc = ptr ? *(AVClass**)ptr : NULL;
    if (level > av_log_get_level())
        return;
    if (print_prefix && avc)
        http_log("[%s @ %p]", avc->item_name(ptr), ptr);
    print_prefix = strstr(fmt, "\n") != NULL;
    http_vlog(fmt, vargs);
}

/* FIXME: This code should use avformat_new_stream() */
static LayeredAVStream *add_av_stream1(FFServerStream *stream,
                                AVCodecContext *codec, int copy)
{
    LayeredAVStream *fst;

    if(stream->nb_streams >= FF_ARRAY_ELEMS(stream->streams))
        return NULL;

    fst = av_mallocz(sizeof(*fst));
    if (!fst)
        return NULL;
    if (copy) {
        fst->codec = avcodec_alloc_context3(codec->codec);
        if (!fst->codec) {
            av_free(fst);
            return NULL;
        }
        avcodec_copy_context(fst->codec, codec);
    } else
        /* live streams must use the actual feed's codec since it may be
         * updated later to carry extradata needed by them.
         */
        fst->codec = codec;

    //NOTE we previously allocated internal & internal->avctx, these seemed uneeded though
    fst->codecpar = avcodec_parameters_alloc();
    fst->index = stream->nb_streams;
    fst->time_base = codec->time_base;
    fst->pts_wrap_bits = 33;
    fst->sample_aspect_ratio = codec->sample_aspect_ratio;
    stream->streams[stream->nb_streams++] = fst;
    return fst;
}

static void remove_stream(FFServerStream *stream)
{
    FFServerStream **ps;
    ps = &config.first_stream;
    while (*ps) {
        if (*ps == stream)
            *ps = (*ps)->next;
        else
            ps = &(*ps)->next;
    }
}

/* compute the needed AVStream for each file */
static void build_file_streams(void)
{
    FFServerStream *stream;
    AVFormatContext *infile;
    int i, ret;

    /* gather all streams */
    for(stream = config.first_stream; stream; stream = stream->next) {
        infile = NULL;

        if (stream->stream_type != STREAM_TYPE_LIVE || stream->feed)
            continue;

        /* the stream comes from a file */
        /* try to open the file */
        /* open stream */


        /* specific case: if transport stream output to RTP,
         * we use a raw transport stream reader */
        if (stream->fmt && !strcmp(stream->fmt->name, "rtp"))
            av_dict_set(&stream->in_opts, "mpeg2ts_compute_pcr", "1", 0);

        if (!stream->feed_filename[0]) {
            http_log("Unspecified feed file for stream '%s'\n",
                     stream->filename);
            goto fail;
        }

        http_log("Opening feed file '%s' for stream '%s'\n",
                 stream->feed_filename, stream->filename);

        ret = avformat_open_input(&infile, stream->feed_filename,
                                  stream->ifmt, &stream->in_opts);
        if (ret < 0) {
            http_log("Could not open '%s': %s\n", stream->feed_filename,
                     av_err2str(ret));
            /* remove stream (no need to spend more time on it) */
        fail:
            remove_stream(stream);
        } else {
            /* find all the AVStreams inside and reference them in
             * 'stream' */
            if (avformat_find_stream_info(infile, NULL) < 0) {
                http_log("Could not find codec parameters from '%s'\n",
                         stream->feed_filename);
                avformat_close_input(&infile);
                goto fail;
            }

            for(i=0;i<infile->nb_streams;i++)
                add_av_stream1(stream, infile->streams[i]->codec, 1);

            avformat_close_input(&infile);
        }
    }
}

/* compute the bandwidth used by each stream */
static void compute_bandwidth(void)
{
    unsigned bandwidth;
    int i;
    FFServerStream *stream;

    for(stream = config.first_stream; stream; stream = stream->next) {
        bandwidth = 0;
        for(i=0;i<stream->nb_streams;i++) {
            LayeredAVStream *st = stream->streams[i];
            switch(st->codec->codec_type) {
            case AVMEDIA_TYPE_AUDIO:
            case AVMEDIA_TYPE_VIDEO:
                bandwidth += st->codec->bit_rate;
                break;
            default:
                break;
            }
        }
        stream->bandwidth = (bandwidth + 999) / 1000;
    }
}

/* open a listening socket */
static int socket_open_listen(struct sockaddr_in *my_addr)
{
    int server_fd, tmp;

    server_fd = socket(AF_INET,SOCK_STREAM,0);
    if (server_fd < 0) {
        perror ("socket");
        return -1;
    }

    tmp = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &tmp, sizeof(tmp)))
        av_log(NULL, AV_LOG_WARNING, "setsockopt SO_REUSEADDR failed\n");

    my_addr->sin_family = AF_INET;
    if (bind (server_fd, (struct sockaddr *) my_addr, sizeof (*my_addr)) < 0) {
        char bindmsg[32];
        snprintf(bindmsg, sizeof(bindmsg), "bind(port %d)",
                 ntohs(my_addr->sin_port));
        perror (bindmsg);
        goto fail;
    }

    if (listen (server_fd, 5) < 0) {
        perror ("listen");
        goto fail;
    }

    if (ff_socket_nonblock(server_fd, 1) < 0)
        av_log(NULL, AV_LOG_WARNING, "ff_socket_nonblock failed\n");

    return server_fd;

fail:
    closesocket(server_fd);
    return -1;
}

/********************************************************************/
/* RTP handling */

static HTTPContext *rtp_new_connection(struct sockaddr_in *from_addr,
                                       FFServerStream *stream,
                                       const char *session_id,
                                       enum RTSPLowerTransport rtp_protocol)
{
    HTTPContext *c = NULL;
    const char *proto_str;

    /* XXX: should output a warning page when coming
     * close to the connection limit */
    if (nb_connections >= config.nb_max_connections)
        goto fail;

    /* add a new connection */
    c = av_mallocz(sizeof(HTTPContext));
    if (!c)
        goto fail;

    c->fd = -1;
    c->poll_entry = NULL;
    c->from_addr = *from_addr;
    c->buffer_size = IOBUFFER_INIT_SIZE;
    c->buffer = av_malloc(c->buffer_size);
    if (!c->buffer)
        goto fail;
    nb_connections++;
    c->stream = stream;
    av_strlcpy(c->session_id, session_id, sizeof(c->session_id));
    c->state = HTTPSTATE_READY;
    c->is_packetized = 1;
    c->rtp_protocol = rtp_protocol;

    /* protocol is shown in statistics */
    switch(c->rtp_protocol) {
    case RTSP_LOWER_TRANSPORT_UDP_MULTICAST:
        proto_str = "MCAST";
        break;
    case RTSP_LOWER_TRANSPORT_UDP:
        proto_str = "UDP";
        break;
    case RTSP_LOWER_TRANSPORT_TCP:
        proto_str = "TCP";
        break;
    default:
        proto_str = "???";
        break;
    }
    av_strlcpy(c->protocol, "RTP/", sizeof(c->protocol));
    av_strlcat(c->protocol, proto_str, sizeof(c->protocol));

    current_bandwidth += stream->bandwidth;

    c->next = first_http_ctx;
    first_http_ctx = c;
    return c;

 fail:
    if (c) {
        av_freep(&c->buffer);
        av_free(c);
    }
    return NULL;
}

static int open_input_stream(HTTPContext *c, const char *info)
{
    char buf[128];
    char input_filename[1024];
    AVFormatContext *s = NULL;
    int buf_size, i, ret;
    int64_t stream_pos;

    /* find file name */
    if (c->stream->feed) {
        strcpy(input_filename, c->stream->feed->feed_filename);
        buf_size = FFM_PACKET_SIZE;
        /* compute position (absolute time) */
        if (av_find_info_tag(buf, sizeof(buf), "date", info)) {
            if ((ret = av_parse_time(&stream_pos, buf, 0)) < 0) {
                http_log("Invalid date specification '%s' for stream\n", buf);
                return ret;
            }
        } else if (av_find_info_tag(buf, sizeof(buf), "buffer", info)) {
            int prebuffer = strtol(buf, 0, 10);
            stream_pos = av_gettime() - prebuffer * (int64_t)1000000;
        } else
            stream_pos = av_gettime() - c->stream->prebuffer * (int64_t)1000;
    } else {
        strcpy(input_filename, c->stream->feed_filename);
        buf_size = 0;
        /* compute position (relative time) */
        if (av_find_info_tag(buf, sizeof(buf), "date", info)) {
            if ((ret = av_parse_time(&stream_pos, buf, 1)) < 0) {
                http_log("Invalid date specification '%s' for stream\n", buf);
                return ret;
            }
        } else
            stream_pos = 0;
    }
    if (!input_filename[0]) {
        http_log("No filename was specified for stream\n");
        return AVERROR(EINVAL);
    }

    /* open stream */
    ret = avformat_open_input(&s, input_filename, c->stream->ifmt,
                              &c->stream->in_opts);
    if (ret < 0) {
        http_log("Could not open input '%s': %s\n",
                 input_filename, av_err2str(ret));
        return ret;
    }

    /* set buffer size */
    if (buf_size > 0) {
        ret = ffio_set_buf_size(s->pb, buf_size);
        if (ret < 0) {
            http_log("Failed to set buffer size\n");
            return ret;
        }
    }

    s->flags |= AVFMT_FLAG_GENPTS;
    c->fmt_in = s;
    if (strcmp(s->iformat->name, "ffm") &&
        (ret = avformat_find_stream_info(c->fmt_in, NULL)) < 0) {
        http_log("Could not find stream info for input '%s'\n", input_filename);
        avformat_close_input(&s);
        return ret;
    }

    /* choose stream as clock source (we favor the video stream if
     * present) for packet sending */
    c->pts_stream_index = 0;
    for(i=0;i<c->stream->nb_streams;i++) {
        if (c->pts_stream_index == 0 &&
            c->stream->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            c->pts_stream_index = i;
        }
    }

    if (c->fmt_in->iformat->read_seek)
        av_seek_frame(c->fmt_in, -1, stream_pos, 0);
    /* set the start time (needed for maxtime and RTP packet timing) */
    c->start_time = cur_time;
    c->first_pts = AV_NOPTS_VALUE;
    return 0;
}

static void unlayer_stream(AVStream *st, LayeredAVStream *lst)
{
    avcodec_free_context(&st->codec);
    avcodec_parameters_free(&st->codecpar);
#define COPY(a) st->a = lst->a;
    COPY(index)
    COPY(id)
    COPY(codec)
    COPY(codecpar)
    COPY(time_base)
    COPY(pts_wrap_bits)
    COPY(sample_aspect_ratio)
    COPY(recommended_encoder_configuration)
}

/**
 * add a new RTP stream in an RTP connection (used in RTSP SETUP
 * command). If RTP/TCP protocol is used, TCP connection 'rtsp_c' is
 * used.
 */
static int rtp_new_av_stream(HTTPContext *c,
                             int stream_index, struct sockaddr_in *dest_addr,
                             HTTPContext *rtsp_c)
{
    AVFormatContext *ctx;
    AVStream *st;
    char *ipaddr;
    URLContext *h = NULL;
    uint8_t *dummy_buf;
    int max_packet_size;
    void *st_internal;

    /* now we can open the relevant output stream */
    ctx = avformat_alloc_context();
    if (!ctx)
        return -1;
    ctx->oformat = av_guess_format("rtp", NULL, NULL);

    st = avformat_new_stream(ctx, NULL);
    if (!st)
        goto fail;

    st_internal = st->internal;

    if (!c->stream->feed ||
        c->stream->feed == c->stream)
        unlayer_stream(st, c->stream->streams[stream_index]);
    else
        unlayer_stream(st,
               c->stream->feed->streams[c->stream->feed_streams[stream_index]]);
    av_assert0(st->priv_data == NULL);
    av_assert0(st->internal == st_internal);

    /* build destination RTP address */
    ipaddr = inet_ntoa(dest_addr->sin_addr);

    switch(c->rtp_protocol) {
    case RTSP_LOWER_TRANSPORT_UDP:
    case RTSP_LOWER_TRANSPORT_UDP_MULTICAST:
        /* RTP/UDP case */

        /* XXX: also pass as parameter to function ? */
        if (c->stream->is_multicast) {
            int ttl;
            ttl = c->stream->multicast_ttl;
            if (!ttl)
                ttl = 16;
            snprintf(ctx->filename, sizeof(ctx->filename),
                     "rtp://%s:%d?multicast=1&ttl=%d",
                     ipaddr, ntohs(dest_addr->sin_port), ttl);
        } else {
            snprintf(ctx->filename, sizeof(ctx->filename),
                     "rtp://%s:%d", ipaddr, ntohs(dest_addr->sin_port));
        }

        if (ffurl_open(&h, ctx->filename, AVIO_FLAG_WRITE, NULL, NULL) < 0)
            goto fail;
        c->rtp_handles[stream_index] = h;
        max_packet_size = h->max_packet_size;
        break;
    case RTSP_LOWER_TRANSPORT_TCP:
        /* RTP/TCP case */
        c->rtsp_c = rtsp_c;
        max_packet_size = RTSP_TCP_MAX_PACKET_SIZE;
        break;
    default:
        goto fail;
    }

    http_log("%s:%d - - \"PLAY %s/streamid=%d %s\"\n",
             ipaddr, ntohs(dest_addr->sin_port),
             c->stream->filename, stream_index, c->protocol);

    /* normally, no packets should be output here, but the packet size may
     * be checked */
    if (ffio_open_dyn_packet_buf(&ctx->pb, max_packet_size) < 0)
        /* XXX: close stream */
        goto fail;

    if (avformat_write_header(ctx, NULL) < 0) {
    fail:
        if (h)
            ffurl_close(h);
        av_free(st);
        av_free(ctx);
        return -1;
    }
    avio_close_dyn_buf(ctx->pb, &dummy_buf);
    ctx->pb = NULL;
    av_free(dummy_buf);

    c->rtp_ctx[stream_index] = ctx;
    return 0;
}

/* start all multicast streams */
static void start_multicast(void)
{
    FFServerStream *stream;
    char session_id[32];
    HTTPContext *rtp_c;
    struct sockaddr_in dest_addr = {0};
    int default_port, stream_index;
    unsigned int random0, random1;

    default_port = 6000;
    for(stream = config.first_stream; stream; stream = stream->next) {

        if (!stream->is_multicast)
            continue;

        random0 = av_lfg_get(&random_state);
        random1 = av_lfg_get(&random_state);

        /* open the RTP connection */
        snprintf(session_id, sizeof(session_id), "%08x%08x", random0, random1);

        /* choose a port if none given */
        if (stream->multicast_port == 0) {
            stream->multicast_port = default_port;
            default_port += 100;
        }

        dest_addr.sin_family = AF_INET;
        dest_addr.sin_addr = stream->multicast_ip;
        dest_addr.sin_port = htons(stream->multicast_port);

        rtp_c = rtp_new_connection(&dest_addr, stream, session_id,
                                   RTSP_LOWER_TRANSPORT_UDP_MULTICAST);
        if (!rtp_c)
            continue;

        if (open_input_stream(rtp_c, "") < 0) {
            http_log("Could not open input stream for stream '%s'\n",
                     stream->filename);
            continue;
        }

        /* open each RTP stream */
        for(stream_index = 0; stream_index < stream->nb_streams;
            stream_index++) {
            dest_addr.sin_port = htons(stream->multicast_port +
                                       2 * stream_index);
            if (rtp_new_av_stream(rtp_c, stream_index, &dest_addr, NULL) >= 0)
                continue;

            http_log("Could not open output stream '%s/streamid=%d'\n",
                     stream->filename, stream_index);
            exit(1);
        }

        rtp_c->state = HTTPSTATE_SEND_DATA;
    }
}

static void get_word(char *buf, int buf_size, const char **pp)
{   
    const char *p;
    char *q;

#define SPACE_CHARS " \t\r\n"

    p = *pp;
    p += strspn(p, SPACE_CHARS);
    q = buf;
    while (!av_isspace(*p) && *p != '\0') {
        if ((q - buf) < buf_size - 1)
            *q++ = *p;
        p++;
    }
    if (buf_size > 0)
        *q = '\0';
    *pp = p;
}   

/**
 * compute the real filename of a file by matching it without its
 * extensions to all the stream's filenames
 */
static void compute_real_filename(char *filename, int max_size)
{
    char file1[1024];
    char file2[1024];
    char *p;
    FFServerStream *stream;

    av_strlcpy(file1, filename, sizeof(file1));
    p = strrchr(file1, '.');
    if (p)
        *p = '\0';
    for(stream = config.first_stream; stream; stream = stream->next) {
        av_strlcpy(file2, stream->filename, sizeof(file2));
        p = strrchr(file2, '.');
        if (p)
            *p = '\0';
        if (!strcmp(file1, file2)) {
            av_strlcpy(filename, stream->filename, max_size);
            break;
        }
    }
}


static FFServerIPAddressACL* parse_dynamic_acl(FFServerStream *stream,
                                               HTTPContext *c)
{
    FILE* f;
    char line[1024];
    char  cmd[1024];
    FFServerIPAddressACL *acl = NULL;
    int line_num = 0;
    const char *p;

    f = fopen(stream->dynamic_acl, "r");
    if (!f) {
        perror(stream->dynamic_acl);
        return NULL;
    }

    acl = av_mallocz(sizeof(FFServerIPAddressACL));
    if (!acl) {
        fclose(f);
        return NULL;
    }

    /* Build ACL */
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        p = line;
        while (av_isspace(*p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;
        ffserver_get_arg(cmd, sizeof(cmd), &p);

        if (!av_strcasecmp(cmd, "ACL"))
            ffserver_parse_acl_row(NULL, NULL, acl, p, stream->dynamic_acl,
                                   line_num);
    }
    fclose(f);
    return acl;
}


static void free_acl_list(FFServerIPAddressACL *in_acl)
{
    FFServerIPAddressACL *pacl, *pacl2;

    pacl = in_acl;
    while(pacl) {
        pacl2 = pacl;
        pacl = pacl->next;
        av_freep(pacl2);
    }
}

static int validate_acl_list(FFServerIPAddressACL *in_acl, HTTPContext *c)
{
    enum FFServerIPAddressAction last_action = IP_DENY;
    FFServerIPAddressACL *acl;
    struct in_addr *src = &c->from_addr.sin_addr;
    unsigned long src_addr = src->s_addr;

    for (acl = in_acl; acl; acl = acl->next) {
        if (src_addr >= acl->first.s_addr && src_addr <= acl->last.s_addr)
            return (acl->action == IP_ALLOW) ? 1 : 0;
        last_action = acl->action;
    }

    /* Nothing matched, so return not the last action */
    return (last_action == IP_DENY) ? 1 : 0;
}

static int validate_acl(FFServerStream *stream, HTTPContext *c)
{
    int ret = 0;
    FFServerIPAddressACL *acl;

    /* if stream->acl is null validate_acl_list will return 1 */
    ret = validate_acl_list(stream->acl, c);

    if (stream->dynamic_acl[0]) {
        acl = parse_dynamic_acl(stream, c);
        ret = validate_acl_list(acl, c);
        free_acl_list(acl);
    }

    return ret;
}

static int extract_rates(char *rates, int ratelen, const char *request)
{
    const char *p;

    for (p = request; *p && *p != '\r' && *p != '\n'; ) {
        if (av_strncasecmp(p, "Pragma:", 7) == 0) {
            const char *q = p + 7;

            while (*q && *q != '\n' && av_isspace(*q))
                q++;

            if (av_strncasecmp(q, "stream-switch-entry=", 20) == 0) {
                int stream_no;
                int rate_no;

                q += 20;

                memset(rates, 0xff, ratelen);

                while (1) {
                    while (*q && *q != '\n' && *q != ':')
                        q++;

                    if (sscanf(q, ":%d:%d", &stream_no, &rate_no) != 2)
                        break;

                    stream_no--;
                    if (stream_no < ratelen && stream_no >= 0)
                        rates[stream_no] = rate_no;

                    while (*q && *q != '\n' && !av_isspace(*q))
                        q++;
                }

                return 1;
            }
        }
        p = strchr(p, '\n');
        if (!p)
            break;

        p++;
    }

    return 0;
}

static int modify_current_stream(HTTPContext *c, char *rates)
{
    int i;
    FFServerStream *req = c->stream;
    int action_required = 0;

    /* Not much we can do for a feed */
    if (!req->feed)
        return 0;
#if 0
    for (i = 0; i < req->nb_streams; i++) {
        AVCodecParameters *codec = req->streams[i]->codecpar;

        switch(rates[i]) {
            case 0:
                c->switch_feed_streams[i] = req->feed_streams[i];
                break;
            case 1:
                c->switch_feed_streams[i] = find_stream_in_feed(req->feed, codec, codec->bit_rate / 2);
                break;
            case 2:
                /* Wants off or slow */
                c->switch_feed_streams[i] = find_stream_in_feed(req->feed, codec, codec->bit_rate / 4);
#ifdef WANTS_OFF
                /* This doesn't work well when it turns off the only stream! */
                c->switch_feed_streams[i] = -2;
                c->feed_streams[i] = -2;
#endif
                break;
        }

        if (c->switch_feed_streams[i] >= 0 &&
            c->switch_feed_streams[i] != c->feed_streams[i]) {
            action_required = 1;
        }
    }
#endif
    return action_required;
}

static int64_t ffm_read_write_index(int fd)
{
    uint8_t buf[8];

    if (lseek(fd, 8, SEEK_SET) < 0)
        return AVERROR(EIO);
    if (read(fd, buf, 8) != 8)
        return AVERROR(EIO);
    return AV_RB64(buf);
}

static int ffm_write_write_index(int fd, int64_t pos)
{
    uint8_t buf[8];
    int i;

    for(i=0;i<8;i++)
        buf[i] = (pos >> (56 - i * 8)) & 0xff;
    if (lseek(fd, 8, SEEK_SET) < 0)
        goto bail_eio;
    if (write(fd, buf, 8) != 8)
        goto bail_eio;

    return 8;

bail_eio:
    return AVERROR(EIO);
}

static int http_start_receive_data(HTTPContext *c)
{
    int fd;
    int ret;
    int64_t ret64;

    if (c->stream->feed_opened) {
        http_log("Stream feed '%s' was not opened\n",
                 c->stream->feed_filename);
        return AVERROR(EINVAL);
    }

    /* Don't permit writing to this one */
    if (c->stream->readonly) {
        http_log("Cannot write to read-only file '%s'\n",
                 c->stream->feed_filename);
        return AVERROR(EINVAL);
    }

    /* open feed */
    fd = open(c->stream->feed_filename, O_RDWR);
    if (fd < 0) {
        ret = AVERROR(errno);
        http_log("Could not open feed file '%s': %s\n",
                 c->stream->feed_filename, strerror(errno));
        return ret;
    }
    c->feed_fd = fd;

    if (c->stream->truncate) {
        /* truncate feed file */
        ffm_write_write_index(c->feed_fd, FFM_PACKET_SIZE);
        http_log("Truncating feed file '%s'\n", c->stream->feed_filename);
        if (ftruncate(c->feed_fd, FFM_PACKET_SIZE) < 0) {
            ret = AVERROR(errno);
            http_log("Error truncating feed file '%s': %s\n",
                     c->stream->feed_filename, strerror(errno));
            return ret;
        }
    } else {
        ret64 = ffm_read_write_index(fd);
        if (ret64 < 0) {
            http_log("Error reading write index from feed file '%s': %s\n",
                     c->stream->feed_filename, strerror(errno));
            return ret64;
        }
        c->stream->feed_write_index = ret64;
    }

    c->stream->feed_write_index = FFMAX(ffm_read_write_index(fd),
                                        FFM_PACKET_SIZE);
    c->stream->feed_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    /* init buffer input */
    c->buffer_ptr = c->buffer;
    c->buffer_end = c->buffer + FFM_PACKET_SIZE;
    c->stream->feed_opened = 1;
    c->chunked_encoding = !!av_stristr(c->buffer, "Transfer-Encoding: chunked");
    return 0;
}


static inline void cp_html_entity (char *buffer, const char *entity) {
    if (!buffer || !entity)
        return;
    while (*entity)
        *buffer++ = *entity++;
}

/**
 * Substitutes known conflicting chars on a text string with
 * their corresponding HTML entities.
 *
 * Returns the number of bytes in the 'encoded' representation
 * not including the terminating NUL.
 */
static size_t htmlencode (const char *src, char **dest) {
    const char *amp = "&amp;";
    const char *lt  = "&lt;";
    const char *gt  = "&gt;";
    const char *start;
    char *tmp;
    size_t final_size = 0;

    if (!src)
        return 0;

    start = src;

    /* Compute needed dest size */
    while (*src != '\0') {
        switch(*src) {
            case 38: /* & */
                final_size += 5;
                break;
            case 60: /* < */
            case 62: /* > */
                final_size += 4;
                break;
            default:
                final_size++;
        }
        src++;
    }

    src = start;
    *dest = av_mallocz(final_size + 1);
    if (!*dest)
        return 0;

    /* Build dest */
    tmp = *dest;
    while (*src != '\0') {
        switch(*src) {
            case 38: /* & */
                cp_html_entity (tmp, amp);
                tmp += 5;
                break;
            case 60: /* < */
                cp_html_entity (tmp, lt);
                tmp += 4;
                break;
            case 62: /* > */
                cp_html_entity (tmp, gt);
                tmp += 4;
                break;
            default:
                *tmp = *src;
                tmp += 1;
        }
        src++;
    }
    *tmp = '\0';

    return final_size;
}

static int prepare_sdp_description(FFServerStream *stream, uint8_t **pbuffer,
                                   struct in_addr my_ip)
{
    AVFormatContext *avc;
    AVOutputFormat *rtp_format = av_guess_format("rtp", NULL, NULL);
    AVDictionaryEntry *entry = av_dict_get(stream->metadata, "title", NULL, 0);
    int i;

    *pbuffer = NULL;

    avc =  avformat_alloc_context();
    if (!avc || !rtp_format)
        return -1;

    avc->oformat = rtp_format;
    av_dict_set(&avc->metadata, "title",
                entry ? entry->value : "No Title", 0);
    if (stream->is_multicast) {
        snprintf(avc->filename, 1024, "rtp://%s:%d?multicast=1?ttl=%d",
                 inet_ntoa(stream->multicast_ip),
                 stream->multicast_port, stream->multicast_ttl);
    } else
        snprintf(avc->filename, 1024, "rtp://0.0.0.0");

    for(i = 0; i < stream->nb_streams; i++) {
        AVStream *st = avformat_new_stream(avc, NULL);
        if (!st)
            goto sdp_done;
        avcodec_parameters_from_context(stream->streams[i]->codecpar, stream->streams[i]->codec);
        unlayer_stream(st, stream->streams[i]);
    }
#define PBUFFER_SIZE 2048
    *pbuffer = av_mallocz(PBUFFER_SIZE);
    if (!*pbuffer)
        goto sdp_done;
    av_sdp_create(&avc, 1, *pbuffer, PBUFFER_SIZE);

 sdp_done:
    av_freep(&avc->streams);
    av_dict_free(&avc->metadata);
    av_free(avc);

    return *pbuffer ? strlen(*pbuffer) : AVERROR(ENOMEM);
}

static void fmt_bytecount(AVIOContext *pb, int64_t count)
{
    static const char suffix[] = " kMGTP";
    const char *s;

    for (s = suffix; count >= 100000 && s[1]; count /= 1000, s++);

    avio_printf(pb, "%"PRId64"%c", count, *s);
}

static inline void print_stream_params(AVIOContext *pb, FFServerStream *stream)
{
    int i, stream_no;
    const char *type = "unknown";
    char parameters[64];
    LayeredAVStream *st;
    AVCodec *codec;

    stream_no = stream->nb_streams;

    avio_printf(pb, "<table><tr><th>Stream<th>"
                    "type<th>kbit/s<th>codec<th>"
                    "Parameters\n");

    for (i = 0; i < stream_no; i++) {
        st = stream->streams[i];
        codec = avcodec_find_encoder(st->codecpar->codec_id);

        parameters[0] = 0;

        switch(st->codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            type = "audio";
            snprintf(parameters, sizeof(parameters), "%d channel(s), %d Hz",
                     st->codecpar->channels, st->codecpar->sample_rate);
            break;
        case AVMEDIA_TYPE_VIDEO:
            type = "video";
            snprintf(parameters, sizeof(parameters),
                     "%dx%d, q=%d-%d, fps=%d", st->codecpar->width,
                     st->codecpar->height, st->codec->qmin, st->codec->qmax,
                     st->time_base.den / st->time_base.num);
            break;
        default:
            abort();
        }

        avio_printf(pb, "<tr><td>%d<td>%s<td>%"PRId64
                        "<td>%s<td>%s\n",
                    i, type, st->codecpar->bit_rate/1000,
                    codec ? codec->name : "", parameters);
     }

     avio_printf(pb, "</table>\n");
}

static void clean_html(char *clean, int clean_len, char *dirty)
{
    int i, o;

    for (o = i = 0; o+10 < clean_len && dirty[i];) {
        int len = strspn(dirty+i, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ$-_.+!*(),?/ :;%");
        if (len) {
            if (o + len >= clean_len)
                break;
            memcpy(clean + o, dirty + i, len);
            i += len;
            o += len;
        } else {
            int c = dirty[i++];
            switch (c) {
            case  '&': av_strlcat(clean+o, "&amp;"  , clean_len - o); break;
            case  '<': av_strlcat(clean+o, "&lt;"   , clean_len - o); break;
            case  '>': av_strlcat(clean+o, "&gt;"   , clean_len - o); break;
            case '\'': av_strlcat(clean+o, "&apos;" , clean_len - o); break;
            case '\"': av_strlcat(clean+o, "&quot;" , clean_len - o); break;
            default:   av_strlcat(clean+o, "&#9785;", clean_len - o); break;
            }
            o += strlen(clean+o);
        }
    }
    clean[o] = 0;
}

/* In bytes per second */
static int compute_datarate(DataRateData *drd, int64_t count)
{
    if (cur_time == drd->time1)
        return 0;

    return ((count - drd->count1) * 1000) / (cur_time - drd->time1);
}

static void ffm_set_write_index(AVFormatContext *s, int64_t pos,
                                int64_t file_size)
{
    av_opt_set_int(s, "server_attached", 1, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(s, "ffm_write_index", pos, AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(s, "ffm_file_size", file_size, AV_OPT_SEARCH_CHILDREN);
}

/* return the estimated time (in us) at which the current packet must be sent */
static int64_t get_packet_send_clock(HTTPContext *c)
{
    int bytes_left, bytes_sent, frame_bytes;

    frame_bytes = c->cur_frame_bytes;
    if (frame_bytes <= 0)
        return c->cur_pts;

    bytes_left = c->buffer_end - c->buffer_ptr;
    bytes_sent = frame_bytes - bytes_left;
    return c->cur_pts + (c->cur_frame_duration * bytes_sent) / frame_bytes;
}

/* return the server clock (in us) */
static int64_t get_server_clock(HTTPContext *c)
{
    /* compute current pts value from system time */
    return (cur_time - c->start_time) * 1000;
}

static void update_datarate(DataRateData *drd, int64_t count)
{
    if (!drd->time1 && !drd->count1) {
        drd->time1 = drd->time2 = cur_time;
        drd->count1 = drd->count2 = count;
    } else if (cur_time - drd->time2 > 5000) {
        drd->time1 = drd->time2;
        drd->count1 = drd->count2;
        drd->time2 = cur_time;
        drd->count2 = count;
    }
}

/* start waiting for a new HTTP/RTSP request */
static void start_wait_request(HTTPContext *c, int is_rtsp)
{
    c->buffer_ptr = c->buffer;
    c->buffer_end = c->buffer + c->buffer_size - 1; /* leave room for '\0' */

    c->state = is_rtsp ? RTSPSTATE_WAIT_REQUEST : HTTPSTATE_WAIT_REQUEST;
    c->timeout = cur_time +
                 (is_rtsp ? RTSP_REQUEST_TIMEOUT : HTTP_REQUEST_TIMEOUT);
}

static void http_send_too_busy_reply(int fd)
{
    char buffer[400];
    int len = snprintf(buffer, sizeof(buffer),
                       "HTTP/1.0 503 Server too busy\r\n"
                       "Content-type: text/html\r\n"
                       "\r\n"
                       "<!DOCTYPE html>\n"
                       "<html><head><title>Too busy</title></head><body>\r\n"
                       "<p>The server is too busy to serve your request at "
                       "this time.</p>\r\n"
                       "<p>The number of current connections is %u, and this "
                       "exceeds the limit of %u.</p>\r\n"
                       "</body></html>\r\n",
                       nb_connections, config.nb_max_connections);
    av_assert0(len < sizeof(buffer));
    if (send(fd, buffer, len, 0) < len)
        av_log(NULL, AV_LOG_WARNING,
               "Could not send too-busy reply, send() failed\n");
}

static void new_connection(int server_fd, int is_rtsp)
{
    struct sockaddr_in from_addr;
    socklen_t len;
    int fd;
    HTTPContext *c = NULL;

    len = sizeof(from_addr);
    fd = accept(server_fd, (struct sockaddr *)&from_addr,
                &len);
    if (fd < 0) {
        http_log("error during accept %s\n", strerror(errno));
        return;
    }
    if (ff_socket_nonblock(fd, 1) < 0)
        av_log(NULL, AV_LOG_WARNING, "ff_socket_nonblock failed\n");

    if (nb_connections >= config.nb_max_connections) {
        http_send_too_busy_reply(fd);
        goto fail;
    }

    /* add a new connection */
    c = av_mallocz(sizeof(HTTPContext));
    if (!c)
        goto fail;

    c->fd = fd;
    c->poll_entry = NULL;
    c->from_addr = from_addr;
    c->buffer_size = IOBUFFER_INIT_SIZE;
    c->buffer = av_malloc(c->buffer_size);
    if (!c->buffer)
        goto fail;

    c->next = first_http_ctx;
    first_http_ctx = c;
    nb_connections++;

    start_wait_request(c, is_rtsp);

    return;

 fail:
    if (c) {
        av_freep(&c->buffer);
        av_free(c);
    }
    closesocket(fd);
}


static void close_connection(HTTPContext *c)
{
    HTTPContext **cp, *c1;
    int i, nb_streams;
    AVFormatContext *ctx;
    AVStream *st;

    /* remove connection from list */
    cp = &first_http_ctx;
    while (*cp) {
        c1 = *cp;
        if (c1 == c)
            *cp = c->next;
        else
            cp = &c1->next;
    }

    /* remove references, if any (XXX: do it faster) */
    for(c1 = first_http_ctx; c1; c1 = c1->next) {
        if (c1->rtsp_c == c)
            c1->rtsp_c = NULL;
    }

    /* remove connection associated resources */
    if (c->fd >= 0)
        closesocket(c->fd);
    if (c->fmt_in) {
        /* close each frame parser */
        for(i=0;i<c->fmt_in->nb_streams;i++) {
            st = c->fmt_in->streams[i];
            if (st->codec->codec)
                avcodec_close(st->codec);
        }
        avformat_close_input(&c->fmt_in);
    }

    /* free RTP output streams if any */
    nb_streams = 0;
    if (c->stream)
        nb_streams = c->stream->nb_streams;

    for(i=0;i<nb_streams;i++) {
        ctx = c->rtp_ctx[i];
        if (ctx) {
            av_write_trailer(ctx);
            av_dict_free(&ctx->metadata);
            av_freep(&ctx->streams[0]);
            av_freep(&ctx);
        }
        ffurl_close(c->rtp_handles[i]);
    }

    ctx = c->pfmt_ctx;

    if (ctx) {
        if (!c->last_packet_sent && c->state == HTTPSTATE_SEND_DATA_TRAILER) {
            /* prepare header */
            if (ctx->oformat && avio_open_dyn_buf(&ctx->pb) >= 0) {
                av_write_trailer(ctx);
                av_freep(&c->pb_buffer);
                avio_close_dyn_buf(ctx->pb, &c->pb_buffer);
            }
        }
        for(i=0; i<ctx->nb_streams; i++)
            av_freep(&ctx->streams[i]);
        av_freep(&ctx->streams);
        av_freep(&ctx->priv_data);
        }

    if (c->stream && !c->post && c->stream->stream_type == STREAM_TYPE_LIVE)
        current_bandwidth -= c->stream->bandwidth;

    /* signal that there is no feed if we are the feeder socket */
    if (c->state == HTTPSTATE_RECEIVE_DATA && c->stream) {
        c->stream->feed_opened = 0;
        close(c->feed_fd);
    }

    av_freep(&c->pb_buffer);
    av_freep(&c->packet_buffer);
    av_freep(&c->buffer);
    av_free(c);
    nb_connections--;
}

static int http_prepare_data(HTTPContext *c)
{
    int i, len, ret;
    AVFormatContext *ctx;

    av_freep(&c->pb_buffer);
    switch(c->state) {
    case HTTPSTATE_SEND_DATA_HEADER:
        ctx = avformat_alloc_context();
        if (!ctx)
            return AVERROR(ENOMEM);
        c->pfmt_ctx = ctx;
        av_dict_copy(&(c->pfmt_ctx->metadata), c->stream->metadata, 0);

        for(i=0;i<c->stream->nb_streams;i++) {
            LayeredAVStream *src;
            AVStream *st = avformat_new_stream(c->pfmt_ctx, NULL);
            if (!st)
                return AVERROR(ENOMEM);

            /* if file or feed, then just take streams from FFServerStream
             * struct */
            if (!c->stream->feed ||
                c->stream->feed == c->stream)
                src = c->stream->streams[i];
            else
                src = c->stream->feed->streams[c->stream->feed_streams[i]];

            unlayer_stream(c->pfmt_ctx->streams[i], src); //TODO we no longer copy st->internal, does this matter?
            av_assert0(!c->pfmt_ctx->streams[i]->priv_data);

            if (src->codec->flags & AV_CODEC_FLAG_BITEXACT)
                c->pfmt_ctx->flags |= AVFMT_FLAG_BITEXACT;
        }
        /* set output format parameters */
        c->pfmt_ctx->oformat = c->stream->fmt;
        av_assert0(c->pfmt_ctx->nb_streams == c->stream->nb_streams);

        c->got_key_frame = 0;

        /* prepare header and save header data in a stream */
        if (avio_open_dyn_buf(&c->pfmt_ctx->pb) < 0) {
            /* XXX: potential leak */
            return -1;
        }
        c->pfmt_ctx->pb->seekable = 0;

        /*
         * HACK to avoid MPEG-PS muxer to spit many underflow errors
         * Default value from FFmpeg
         * Try to set it using configuration option
         */
        c->pfmt_ctx->max_delay = (int)(0.7*AV_TIME_BASE);

        if ((ret = avformat_write_header(c->pfmt_ctx, NULL)) < 0) {
            http_log("Error writing output header for stream '%s': %s\n",
                     c->stream->filename, av_err2str(ret));
            return ret;
        }
        av_dict_free(&c->pfmt_ctx->metadata);

        len = avio_close_dyn_buf(c->pfmt_ctx->pb, &c->pb_buffer);
        c->buffer_ptr = c->pb_buffer;
        c->buffer_end = c->pb_buffer + len;

        c->state = HTTPSTATE_SEND_DATA;
        c->last_packet_sent = 0;
        break;
    case HTTPSTATE_SEND_DATA:
        /* find a new packet */
        /* read a packet from the input stream */
        if (c->stream->feed)
            ffm_set_write_index(c->fmt_in,
                                c->stream->feed->feed_write_index,
                                c->stream->feed->feed_size);

        if (c->stream->max_time &&
            c->stream->max_time + c->start_time - cur_time < 0)
            /* We have timed out */
            c->state = HTTPSTATE_SEND_DATA_TRAILER;
        else {
            AVPacket pkt;
        redo:
            ret = av_read_frame(c->fmt_in, &pkt);
            if (ret < 0) {
                if (c->stream->feed) {
                    /* if coming from feed, it means we reached the end of the
                     * ffm file, so must wait for more data */
                    c->state = HTTPSTATE_WAIT_FEED;
                    return 1; /* state changed */
                }
                if (ret == AVERROR(EAGAIN)) {
                    /* input not ready, come back later */
                    return 0;
                }
                if (c->stream->loop) {
                    avformat_close_input(&c->fmt_in);
                    if (open_input_stream(c, "") < 0)
                        goto no_loop;
                    goto redo;
                } else {
                    no_loop:
                        /* must send trailer now because EOF or error */
                        c->state = HTTPSTATE_SEND_DATA_TRAILER;
                }
            } else {
                int source_index = pkt.stream_index;
                /* update first pts if needed */
                if (c->first_pts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE) {
                    c->first_pts = av_rescale_q(pkt.dts, c->fmt_in->streams[pkt.stream_index]->time_base, AV_TIME_BASE_Q);
                    c->start_time = cur_time;
                }
                /* send it to the appropriate stream */
                if (c->stream->feed) {
                    /* if coming from a feed, select the right stream */
                    if (c->switch_pending) {
                        c->switch_pending = 0;
                        for(i=0;i<c->stream->nb_streams;i++) {
                            if (c->switch_feed_streams[i] == pkt.stream_index)
                                if (pkt.flags & AV_PKT_FLAG_KEY)
                                    c->switch_feed_streams[i] = -1;
                            if (c->switch_feed_streams[i] >= 0)
                                c->switch_pending = 1;
                        }
                    }
                    for(i=0;i<c->stream->nb_streams;i++) {
                        if (c->stream->feed_streams[i] == pkt.stream_index) {
                            AVStream *st = c->fmt_in->streams[source_index];
                            pkt.stream_index = i;
                            if (pkt.flags & AV_PKT_FLAG_KEY &&
                                (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
                                 c->stream->nb_streams == 1))
                                c->got_key_frame = 1;
                            if (!c->stream->send_on_key || c->got_key_frame)
                                goto send_it;
                        }
                    }
                } else {
                    AVStream *ist, *ost;
                send_it:
                    ist = c->fmt_in->streams[source_index];
                    /* specific handling for RTP: we use several
                     * output streams (one for each RTP connection).
                     * XXX: need more abstract handling */
                    if (c->is_packetized) {
                        /* compute send time and duration */
                        if (pkt.dts != AV_NOPTS_VALUE) {
                            c->cur_pts = av_rescale_q(pkt.dts, ist->time_base, AV_TIME_BASE_Q);
                            c->cur_pts -= c->first_pts;
                        }
                        c->cur_frame_duration = av_rescale_q(pkt.duration, ist->time_base, AV_TIME_BASE_Q);
                        /* find RTP context */
                        c->packet_stream_index = pkt.stream_index;
                        ctx = c->rtp_ctx[c->packet_stream_index];
                        if(!ctx) {
                            av_packet_unref(&pkt);
                            break;
                        }
                        /* only one stream per RTP connection */
                        pkt.stream_index = 0;
                    } else {
                        ctx = c->pfmt_ctx;
                        /* Fudge here */
                    }

                    if (c->is_packetized) {
                        int max_packet_size;
                        if (c->rtp_protocol == RTSP_LOWER_TRANSPORT_TCP)
                            max_packet_size = RTSP_TCP_MAX_PACKET_SIZE;
                        else
                            max_packet_size = c->rtp_handles[c->packet_stream_index]->max_packet_size;
                        ret = ffio_open_dyn_packet_buf(&ctx->pb,
                                                       max_packet_size);
                    } else
                        ret = avio_open_dyn_buf(&ctx->pb);

                    if (ret < 0) {
                        /* XXX: potential leak */
                        return -1;
                    }
                    ost = ctx->streams[pkt.stream_index];

                    ctx->pb->seekable = 0;
                    if (pkt.dts != AV_NOPTS_VALUE)
                        pkt.dts = av_rescale_q(pkt.dts, ist->time_base,
                                               ost->time_base);
                    if (pkt.pts != AV_NOPTS_VALUE)
                        pkt.pts = av_rescale_q(pkt.pts, ist->time_base,
                                               ost->time_base);
                    pkt.duration = av_rescale_q(pkt.duration, ist->time_base,
                                                ost->time_base);
                    if ((ret = av_write_frame(ctx, &pkt)) < 0) {
                        http_log("Error writing frame to output for stream '%s': %s\n",
                                 c->stream->filename, av_err2str(ret));
                        c->state = HTTPSTATE_SEND_DATA_TRAILER;
                    }

                    av_freep(&c->pb_buffer);
                    len = avio_close_dyn_buf(ctx->pb, &c->pb_buffer);
                    ctx->pb = NULL;
                    c->cur_frame_bytes = len;
                    c->buffer_ptr = c->pb_buffer;
                    c->buffer_end = c->pb_buffer + len;

                    if (len == 0) {
                        av_packet_unref(&pkt);
                        goto redo;
                    }
                }
                av_packet_unref(&pkt);
            }
        }
        break;
    default:
    case HTTPSTATE_SEND_DATA_TRAILER:
        /* last packet test ? */
        if (c->last_packet_sent || c->is_packetized)
            return -1;
        ctx = c->pfmt_ctx;
        /* prepare header */
        if (avio_open_dyn_buf(&ctx->pb) < 0) {
            /* XXX: potential leak */
            return -1;
        }
        c->pfmt_ctx->pb->seekable = 0;
        av_write_trailer(ctx);
        len = avio_close_dyn_buf(ctx->pb, &c->pb_buffer);
        c->buffer_ptr = c->pb_buffer;
        c->buffer_end = c->pb_buffer + len;

        c->last_packet_sent = 1;
        break;
    }
    return 0;
}

/* should convert the format at the same time */
/* send data starting at c->buffer_ptr to the output connection
 * (either UDP or TCP)
 */
static int http_send_data(HTTPContext *c)
{
    int len, ret;

    for(;;) {
        if (c->buffer_ptr >= c->buffer_end) {
            ret = http_prepare_data(c);
            if (ret < 0)
                return -1;
            else if (ret)
                /* state change requested */
                break;
        } else {
            if (c->is_packetized) {
                /* RTP data output */
                len = c->buffer_end - c->buffer_ptr;
                if (len < 4) {
                    /* fail safe - should never happen */
                fail1:
                    c->buffer_ptr = c->buffer_end;
                    return 0;
                }
                len = (c->buffer_ptr[0] << 24) |
                    (c->buffer_ptr[1] << 16) |
                    (c->buffer_ptr[2] << 8) |
                    (c->buffer_ptr[3]);
                if (len > (c->buffer_end - c->buffer_ptr))
                    goto fail1;
                if ((get_packet_send_clock(c) - get_server_clock(c)) > 0) {
                    /* nothing to send yet: we can wait */
                    return 0;
                }

                c->data_count += len;
                update_datarate(&c->datarate, c->data_count);
                if (c->stream)
                    c->stream->bytes_served += len;

                if (c->rtp_protocol == RTSP_LOWER_TRANSPORT_TCP) {
                    printf("%s %d\n", __func__, __LINE__);
                    /* RTP packets are sent inside the RTSP TCP connection */
                    AVIOContext *pb;
                    int interleaved_index, size;
                    uint8_t header[4];
                    HTTPContext *rtsp_c;

                    rtsp_c = c->rtsp_c;
                    /* if no RTSP connection left, error */
                    if (!rtsp_c)
                        return -1;
                    /* if already sending something, then wait. */
                    if (rtsp_c->state != RTSPSTATE_WAIT_REQUEST)
                        break;
                    if (avio_open_dyn_buf(&pb) < 0)
                        goto fail1;
                    interleaved_index = c->packet_stream_index * 2;
                    /* RTCP packets are sent at odd indexes */
                    if (c->buffer_ptr[1] == 200)
                        interleaved_index++;
                    /* write RTSP TCP header */
                    header[0] = '$';
                    header[1] = interleaved_index;
                    header[2] = len >> 8;
                    header[3] = len;
                    avio_write(pb, header, 4);
                    /* write RTP packet data */
                    c->buffer_ptr += 4;
                    avio_write(pb, c->buffer_ptr, len);
                    size = avio_close_dyn_buf(pb, &c->packet_buffer);
                    /* prepare asynchronous TCP sending */
                    rtsp_c->packet_buffer_ptr = c->packet_buffer;
                    rtsp_c->packet_buffer_end = c->packet_buffer + size;
                    c->buffer_ptr += len;
                    printf("%s %d\n", __func__, __LINE__);
                    /* send everything we can NOW */
                    len = send(rtsp_c->fd, rtsp_c->packet_buffer_ptr,
                               rtsp_c->packet_buffer_end - rtsp_c->packet_buffer_ptr, 0);
                    if (len > 0)
                        rtsp_c->packet_buffer_ptr += len;
                    if (rtsp_c->packet_buffer_ptr < rtsp_c->packet_buffer_end) {
                        /* if we could not send all the data, we will
                         * send it later, so a new state is needed to
                         * "lock" the RTSP TCP connection */
                        rtsp_c->state = RTSPSTATE_SEND_PACKET;
                        break;
                    } else
                        /* all data has been sent */
                        av_freep(&c->packet_buffer);
                } else {
                    printf("%s %d\n", __func__, __LINE__);
                    /* send RTP packet directly in UDP */
                    c->buffer_ptr += 4;
                    ffurl_write(c->rtp_handles[c->packet_stream_index],
                                c->buffer_ptr, len);
                    c->buffer_ptr += len;
                    /* here we continue as we can send several packets
                     * per 10 ms slot */
                }
            } else {
                /* TCP data output */
                    printf("%s %d\n", __func__, __LINE__);
                len = send(c->fd, c->buffer_ptr,
                           c->buffer_end - c->buffer_ptr, 0);
                if (len < 0) {
                    if (ff_neterrno() != AVERROR(EAGAIN) &&
                        ff_neterrno() != AVERROR(EINTR))
                        /* error : close connection */
                        return -1;
                    else
                        return 0;
                }
                c->buffer_ptr += len;

                c->data_count += len;
                update_datarate(&c->datarate, c->data_count);
                if (c->stream)
                    c->stream->bytes_served += len;
                break;
            }
        }
    } /* for(;;) */
    return 0;
}

static void compute_status(HTTPContext *c)
{
    HTTPContext *c1;
    FFServerStream *stream;
    char *p;
    time_t ti;
    int i, len;
    AVIOContext *pb;

    if (avio_open_dyn_buf(&pb) < 0) {
        /* XXX: return an error ? */
        c->buffer_ptr = c->buffer;
        c->buffer_end = c->buffer;
        return;
    }

    avio_printf(pb, "HTTP/1.0 200 OK\r\n");
    avio_printf(pb, "Content-type: text/html\r\n");
    avio_printf(pb, "Pragma: no-cache\r\n");
    avio_printf(pb, "\r\n");

    avio_printf(pb, "<!DOCTYPE html>\n");
    avio_printf(pb, "<html><head><title>%s Status</title>\n", program_name);
    if (c->stream->feed_filename[0])
        avio_printf(pb, "<link rel=\"shortcut icon\" href=\"%s\">\n",
                    c->stream->feed_filename);
    avio_printf(pb, "</head>\n<body>");
    avio_printf(pb, "<h1>%s Status</h1>\n", program_name);
    /* format status */
    avio_printf(pb, "<h2>Available Streams</h2>\n");
    avio_printf(pb, "<table>\n");
    avio_printf(pb, "<tr><th>Path<th>Served<br>Conns<th><br>bytes<th>Format<th>Bit rate<br>kbit/s<th>Video<br>kbit/s<th><br>Codec<th>Audio<br>kbit/s<th><br>Codec<th>Feed\n");
    stream = config.first_stream;
    while (stream) {
        char sfilename[1024];
        char *eosf;

        if (stream->feed == stream) {
            stream = stream->next;
            continue;
        }

        av_strlcpy(sfilename, stream->filename, sizeof(sfilename) - 10);
        eosf = sfilename + strlen(sfilename);
        if (eosf - sfilename >= 4) {
            if (strcmp(eosf - 4, ".asf") == 0)
                strcpy(eosf - 4, ".asx");
            else if (strcmp(eosf - 3, ".rm") == 0)
                strcpy(eosf - 3, ".ram");
            else if (stream->fmt && !strcmp(stream->fmt->name, "rtp")) {
                /* generate a sample RTSP director if
                 * unicast. Generate an SDP redirector if
                 * multicast */
                eosf = strrchr(sfilename, '.');
                if (!eosf)
                    eosf = sfilename + strlen(sfilename);
                if (stream->is_multicast)
                    strcpy(eosf, ".sdp");
                else
                    strcpy(eosf, ".rtsp");
            }
        }

        avio_printf(pb, "<tr><td><a href=\"/%s\">%s</a> ",
                    sfilename, stream->filename);
        avio_printf(pb, "<td> %d <td> ",
                    stream->conns_served);
        // TODO: Investigate if we can make http bitexact so it always produces the same count of bytes
        if (!config.bitexact)
            fmt_bytecount(pb, stream->bytes_served);

        switch(stream->stream_type) {
        case STREAM_TYPE_LIVE: {
            int audio_bit_rate = 0;
            int video_bit_rate = 0;
            const char *audio_codec_name = "";
            const char *video_codec_name = "";
            const char *audio_codec_name_extra = "";
            const char *video_codec_name_extra = "";

            for(i=0;i<stream->nb_streams;i++) {
                LayeredAVStream *st = stream->streams[i];
                AVCodec *codec = avcodec_find_encoder(st->codecpar->codec_id);

                switch(st->codecpar->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    audio_bit_rate += st->codecpar->bit_rate;
                    if (codec) {
                        if (*audio_codec_name)
                            audio_codec_name_extra = "...";
                        audio_codec_name = codec->name;
                    }
                    break;
                case AVMEDIA_TYPE_VIDEO:
                    video_bit_rate += st->codecpar->bit_rate;
                    if (codec) {
                        if (*video_codec_name)
                            video_codec_name_extra = "...";
                        video_codec_name = codec->name;
                    }
                    break;
                case AVMEDIA_TYPE_DATA:
                    video_bit_rate += st->codecpar->bit_rate;
                    break;
                default:
                    abort();
                }
            }

            avio_printf(pb, "<td> %s <td> %d <td> %d <td> %s %s <td> "
                            "%d <td> %s %s",
                        stream->fmt->name, stream->bandwidth,
                        video_bit_rate / 1000, video_codec_name,
                        video_codec_name_extra, audio_bit_rate / 1000,
                        audio_codec_name, audio_codec_name_extra);

            if (stream->feed)
                avio_printf(pb, "<td>%s", stream->feed->filename);
            else
                avio_printf(pb, "<td>%s", stream->feed_filename);
            avio_printf(pb, "\n");
        }
            break;
        default:
            avio_printf(pb, "<td> - <td> - "
                            "<td> - <td><td> - <td>\n");
            break;
        }
        stream = stream->next;
    }
    avio_printf(pb, "</table>\n");

    stream = config.first_stream;
    while (stream) {

        if (stream->feed != stream) {
            stream = stream->next;
            continue;
        }

        avio_printf(pb, "<h2>Feed %s</h2>", stream->filename);
        if (stream->pid) {
            avio_printf(pb, "Running as pid %"PRId64".\n", (int64_t) stream->pid);

#if defined(linux)
            {
                FILE *pid_stat;
                char ps_cmd[64];

                /* This is somewhat linux specific I guess */
                snprintf(ps_cmd, sizeof(ps_cmd),
                         "ps -o \"%%cpu,cputime\" --no-headers %"PRId64"",
                         (int64_t) stream->pid);

                 pid_stat = popen(ps_cmd, "r");
                 if (pid_stat) {
                     char cpuperc[10];
                     char cpuused[64];

                     if (fscanf(pid_stat, "%9s %63s", cpuperc, cpuused) == 2) {
                         avio_printf(pb, "Currently using %s%% of the cpu. "
                                         "Total time used %s.\n",
                                     cpuperc, cpuused);
                     }
                     fclose(pid_stat);
                 }
            }
#endif

            avio_printf(pb, "<p>");
        }

        print_stream_params(pb, stream);
        stream = stream->next;
    }

    /* connection status */
    avio_printf(pb, "<h2>Connection Status</h2>\n");

    avio_printf(pb, "Number of connections: %d / %d<br>\n",
                nb_connections, config.nb_max_connections);

    avio_printf(pb, "Bandwidth in use: %"PRIu64"k / %"PRIu64"k<br>\n",
                current_bandwidth, config.max_bandwidth);

    avio_printf(pb, "<table>\n");
    avio_printf(pb, "<tr><th>#<th>File<th>IP<th>URL<th>Proto<th>State<th>Target "
                    "bit/s<th>Actual bit/s<th>Bytes transferred\n");
    c1 = first_http_ctx;
    i = 0;
    while (c1) {
        int bitrate;
        int j;

        bitrate = 0;
        if (c1->stream) {
            for (j = 0; j < c1->stream->nb_streams; j++) {
                if (!c1->stream->feed)
                    bitrate += c1->stream->streams[j]->codecpar->bit_rate;
                else if (c1->feed_streams[j] >= 0)
                    bitrate += c1->stream->feed->streams[c1->feed_streams[j]]->codecpar->bit_rate;
            }
        }

        i++;
        p = inet_ntoa(c1->from_addr.sin_addr);
        clean_html(c1->clean_url, sizeof(c1->clean_url), c1->url);
        avio_printf(pb, "<tr><td><b>%d</b><td>%s%s<td>%s<td>%s<td>%s<td>%s"
                        "<td>",
                    i, c1->stream ? c1->stream->filename : "",
                    c1->state == HTTPSTATE_RECEIVE_DATA ? "(input)" : "",
                    p,
                    c1->clean_url,
                    c1->protocol, http_state[c1->state]);
        fmt_bytecount(pb, bitrate);
        avio_printf(pb, "<td>");
        fmt_bytecount(pb, compute_datarate(&c1->datarate, c1->data_count) * 8);
        avio_printf(pb, "<td>");
        fmt_bytecount(pb, c1->data_count);
        avio_printf(pb, "\n");
        c1 = c1->next;
    }
    avio_printf(pb, "</table>\n");

    if (!config.bitexact) {
        /* date */
        ti = time(NULL);
        p = ctime(&ti);
        avio_printf(pb, "<hr>Generated at %s", p);
    }
    avio_printf(pb, "</body>\n</html>\n");

    len = avio_close_dyn_buf(pb, &c->pb_buffer);
    c->buffer_ptr = c->pb_buffer;
    c->buffer_end = c->pb_buffer + len;
}


/* parse HTTP request and prepare header */
static int http_parse_request(HTTPContext *c)
{
    const char *p;
    char *p1;
    enum RedirType redir_type;
    char cmd[32];
    char info[1024], filename[1024];
    char url[1024], *q;
    char protocol[32];
    char msg[1024];
    char *encoded_msg = NULL;
    const char *mime_type;
    FFServerStream *stream;
    int i;
    char ratebuf[32];
    const char *useragent = 0;

    p = c->buffer;
    get_word(cmd, sizeof(cmd), &p);
    av_strlcpy(c->method, cmd, sizeof(c->method));

    if (!strcmp(cmd, "GET"))
        c->post = 0;
    else if (!strcmp(cmd, "POST"))
        c->post = 1;
    else
        return -1;

    get_word(url, sizeof(url), &p);
    av_strlcpy(c->url, url, sizeof(c->url));

    get_word(protocol, sizeof(protocol), (const char **)&p);
    if (strcmp(protocol, "HTTP/1.0") && strcmp(protocol, "HTTP/1.1"))
        return -1;

    av_strlcpy(c->protocol, protocol, sizeof(c->protocol));

    if (config.debug)
        http_log("%s - - New connection: %s %s\n",
                 inet_ntoa(c->from_addr.sin_addr), cmd, url);

    /* find the filename and the optional info string in the request */
    p1 = strchr(url, '?');
    if (p1) {
        av_strlcpy(info, p1, sizeof(info));
        *p1 = '\0';
    } else
        info[0] = '\0';

    av_strlcpy(filename, url + ((*url == '/') ? 1 : 0), sizeof(filename)-1);

    for (p = c->buffer; *p && *p != '\r' && *p != '\n'; ) {
        if (av_strncasecmp(p, "User-Agent:", 11) == 0) {
            useragent = p + 11;
            if (*useragent && *useragent != '\n' && av_isspace(*useragent))
                useragent++;
            break;
        }
        p = strchr(p, '\n');
        if (!p)
            break;

        p++;
    }

    redir_type = REDIR_NONE;
    if (av_match_ext(filename, "asx")) {
        redir_type = REDIR_ASX;
        filename[strlen(filename)-1] = 'f';
    } else if (av_match_ext(filename, "asf") &&
        (!useragent || av_strncasecmp(useragent, "NSPlayer", 8))) {
        /* if this isn't WMP or lookalike, return the redirector file */
        redir_type = REDIR_ASF;
    } else if (av_match_ext(filename, "rpm,ram")) {
        redir_type = REDIR_RAM;
        strcpy(filename + strlen(filename)-2, "m");
    } else if (av_match_ext(filename, "rtsp")) {
        redir_type = REDIR_RTSP;
        compute_real_filename(filename, sizeof(filename) - 1);
    } else if (av_match_ext(filename, "sdp")) {
        redir_type = REDIR_SDP;
        compute_real_filename(filename, sizeof(filename) - 1);
    }

    /* "redirect" request to index.html */
    if (!strlen(filename))
        av_strlcpy(filename, "index.html", sizeof(filename) - 1);

    stream = config.first_stream;
    while (stream) {
        if (!strcmp(stream->filename, filename) && validate_acl(stream, c))
            break;
        stream = stream->next;
    }
    if (!stream) {
        snprintf(msg, sizeof(msg), "File '%s' not found", url);
        http_log("File '%s' not found\n", url);
        goto send_error;
    }

    c->stream = stream;
    memcpy(c->feed_streams, stream->feed_streams, sizeof(c->feed_streams));
    memset(c->switch_feed_streams, -1, sizeof(c->switch_feed_streams));

    if (stream->stream_type == STREAM_TYPE_REDIRECT) {
        c->http_error = 301;
        q = c->buffer;
        snprintf(q, c->buffer_size,
                      "HTTP/1.0 301 Moved\r\n"
                      "Location: %s\r\n"
                      "Content-type: text/html\r\n"
                      "\r\n"
                      "<!DOCTYPE html>\n"
                      "<html><head><title>Moved</title></head><body>\r\n"
                      "You should be <a href=\"%s\">redirected</a>.\r\n"
                      "</body></html>\r\n",
                 stream->feed_filename, stream->feed_filename);
        q += strlen(q);
        /* prepare output buffer */
        c->buffer_ptr = c->buffer;
        c->buffer_end = q;
        c->state = HTTPSTATE_SEND_HEADER;
        return 0;
    }

    /* If this is WMP, get the rate information */
    if (extract_rates(ratebuf, sizeof(ratebuf), c->buffer)) {
        if (modify_current_stream(c, ratebuf)) {
            for (i = 0; i < FF_ARRAY_ELEMS(c->feed_streams); i++) {
                if (c->switch_feed_streams[i] >= 0)
                    c->switch_feed_streams[i] = -1;
            }
        }
    }

    if (c->post == 0 && stream->stream_type == STREAM_TYPE_LIVE)
        current_bandwidth += stream->bandwidth;

    /* If already streaming this feed, do not let another feeder start */
    if (stream->feed_opened) {
        snprintf(msg, sizeof(msg), "This feed is already being received.");
        http_log("Feed '%s' already being received\n", stream->feed_filename);
        goto send_error;
    }

    if (c->post == 0 && config.max_bandwidth < current_bandwidth) {
        c->http_error = 503;
        q = c->buffer;
        snprintf(q, c->buffer_size,
                      "HTTP/1.0 503 Server too busy\r\n"
                      "Content-type: text/html\r\n"
                      "\r\n"
                      "<!DOCTYPE html>\n"
                      "<html><head><title>Too busy</title></head><body>\r\n"
                      "<p>The server is too busy to serve your request at "
                      "this time.</p>\r\n"
                      "<p>The bandwidth being served (including your stream) "
                      "is %"PRIu64"kbit/s, and this exceeds the limit of "
                      "%"PRIu64"kbit/s.</p>\r\n"
                      "</body></html>\r\n",
                 current_bandwidth, config.max_bandwidth);
        q += strlen(q);
        /* prepare output buffer */
        c->buffer_ptr = c->buffer;
        c->buffer_end = q;
        c->state = HTTPSTATE_SEND_HEADER;
        return 0;
    }

    if (redir_type != REDIR_NONE) {
        const char *hostinfo = 0;

        for (p = c->buffer; *p && *p != '\r' && *p != '\n'; ) {
            if (av_strncasecmp(p, "Host:", 5) == 0) {
                hostinfo = p + 5;
                break;
            }
            p = strchr(p, '\n');
            if (!p)
                break;

            p++;
        }

        if (hostinfo) {
            char *eoh;
            char hostbuf[260];

            while (av_isspace(*hostinfo))
                hostinfo++;

            eoh = strchr(hostinfo, '\n');
            if (eoh) {
                if (eoh[-1] == '\r')
                    eoh--;

                if (eoh - hostinfo < sizeof(hostbuf) - 1) {
                    memcpy(hostbuf, hostinfo, eoh - hostinfo);
                    hostbuf[eoh - hostinfo] = 0;

                    c->http_error = 200;
                    q = c->buffer;
                    switch(redir_type) {
                    case REDIR_ASX:
                        snprintf(q, c->buffer_size,
                                      "HTTP/1.0 200 ASX Follows\r\n"
                                      "Content-type: video/x-ms-asf\r\n"
                                      "\r\n"
                                      "<ASX Version=\"3\">\r\n"
                                      //"<!-- Autogenerated by ffserver -->\r\n"
                                      "<ENTRY><REF HREF=\"http://%s/%s%s\"/></ENTRY>\r\n"
                                      "</ASX>\r\n", hostbuf, filename, info);
                        q += strlen(q);
                        break;
                    case REDIR_RAM:
                        snprintf(q, c->buffer_size,
                                      "HTTP/1.0 200 RAM Follows\r\n"
                                      "Content-type: audio/x-pn-realaudio\r\n"
                                      "\r\n"
                                      "# Autogenerated by ffserver\r\n"
                                      "http://%s/%s%s\r\n", hostbuf, filename, info);
                        q += strlen(q);
                        break;
                    case REDIR_ASF:
                        snprintf(q, c->buffer_size,
                                      "HTTP/1.0 200 ASF Redirect follows\r\n"
                                      "Content-type: video/x-ms-asf\r\n"
                                      "\r\n"
                                      "[Reference]\r\n"
                                      "Ref1=http://%s/%s%s\r\n", hostbuf, filename, info);
                        q += strlen(q);
                        break;
                    case REDIR_RTSP:
                        {
                            char hostname[256], *p;
                            /* extract only hostname */
                            av_strlcpy(hostname, hostbuf, sizeof(hostname));
                            p = strrchr(hostname, ':');
                            if (p)
                                *p = '\0';
                            snprintf(q, c->buffer_size,
                                          "HTTP/1.0 200 RTSP Redirect follows\r\n"
                                          /* XXX: incorrect MIME type ? */
                                          "Content-type: application/x-rtsp\r\n"
                                          "\r\n"
                                          "rtsp://%s:%d/%s\r\n", hostname, ntohs(config.rtsp_addr.sin_port), filename);
                            q += strlen(q);
                        }
                        break;
                    case REDIR_SDP:
                        {
                            uint8_t *sdp_data;
                            int sdp_data_size;
                            socklen_t len;
                            struct sockaddr_in my_addr;

                            snprintf(q, c->buffer_size,
                                          "HTTP/1.0 200 OK\r\n"
                                          "Content-type: application/sdp\r\n"
                                          "\r\n");
                            q += strlen(q);

                            len = sizeof(my_addr);

                            /* XXX: Should probably fail? */
                            if (getsockname(c->fd, (struct sockaddr *)&my_addr, &len))
                                http_log("getsockname() failed\n");

                            /* XXX: should use a dynamic buffer */
                            sdp_data_size = prepare_sdp_description(stream,
                                                                    &sdp_data,
                                                                    my_addr.sin_addr);
                            if (sdp_data_size > 0) {
                                memcpy(q, sdp_data, sdp_data_size);
                                q += sdp_data_size;
                                *q = '\0';
                                av_freep(&sdp_data);
                            }
                        }
                        break;
                    default:
                        abort();
                        break;
                    }

                    /* prepare output buffer */
                    c->buffer_ptr = c->buffer;
                    c->buffer_end = q;
                    c->state = HTTPSTATE_SEND_HEADER;
                    return 0;
                }
            }
        }

        snprintf(msg, sizeof(msg), "ASX/RAM file not handled");
        goto send_error;
    }

    stream->conns_served++;

    /* XXX: add there authenticate and IP match */

    if (c->post) {
        /* if post, it means a feed is being sent */
        if (!stream->is_feed) {
            /* However it might be a status report from WMP! Let us log the
             * data as it might come handy one day. */
            const char *logline = 0;
            int client_id = 0;

            for (p = c->buffer; *p && *p != '\r' && *p != '\n'; ) {
                if (av_strncasecmp(p, "Pragma: log-line=", 17) == 0) {
                    logline = p;
                    break;
                }
                if (av_strncasecmp(p, "Pragma: client-id=", 18) == 0)
                    client_id = strtol(p + 18, 0, 10);
                p = strchr(p, '\n');
                if (!p)
                    break;

                p++;
            }

            if (logline) {
                char *eol = strchr(logline, '\n');

                logline += 17;

                if (eol) {
                    if (eol[-1] == '\r')
                        eol--;
                    http_log("%.*s\n", (int) (eol - logline), logline);
                    c->suppress_log = 1;
                }
            }

#ifdef DEBUG
            http_log("\nGot request:\n%s\n", c->buffer);
#endif

            if (client_id && extract_rates(ratebuf, sizeof(ratebuf), c->buffer)) {
                HTTPContext *wmpc;

                /* Now we have to find the client_id */
                for (wmpc = first_http_ctx; wmpc; wmpc = wmpc->next) {
                    if (wmpc->wmp_client_id == client_id)
                        break;
                }

                if (wmpc && modify_current_stream(wmpc, ratebuf))
                    wmpc->switch_pending = 1;
            }

            snprintf(msg, sizeof(msg), "POST command not handled");
            c->stream = 0;
            goto send_error;
        }
        if (http_start_receive_data(c) < 0) {
            snprintf(msg, sizeof(msg), "could not open feed");
            goto send_error;
        }
        c->http_error = 0;
        c->state = HTTPSTATE_RECEIVE_DATA;
        return 0;
    }

#ifdef DEBUG
    if (strcmp(stream->filename + strlen(stream->filename) - 4, ".asf") == 0)
        http_log("\nGot request:\n%s\n", c->buffer);
#endif

    if (c->stream->stream_type == STREAM_TYPE_STATUS)
        goto send_status;

    /* open input stream */
    if (open_input_stream(c, info) < 0) {
        snprintf(msg, sizeof(msg), "Input stream corresponding to '%s' not found", url);
        goto send_error;
    }

    /* prepare HTTP header */
    c->buffer[0] = 0;
    av_strlcatf(c->buffer, c->buffer_size, "HTTP/1.0 200 OK\r\n");
    mime_type = c->stream->fmt->mime_type;
    if (!mime_type)
        mime_type = "application/x-octet-stream";
    av_strlcatf(c->buffer, c->buffer_size, "Pragma: no-cache\r\n");

    /* for asf, we need extra headers */
    if (!strcmp(c->stream->fmt->name,"asf_stream")) {
        /* Need to allocate a client id */

        c->wmp_client_id = av_lfg_get(&random_state);

        av_strlcatf(c->buffer, c->buffer_size, "Server: Cougar 4.1.0.3923\r\nCache-Control: no-cache\r\nPragma: client-id=%d\r\nPragma: features=\"broadcast\"\r\n", c->wmp_client_id);
    }
    av_strlcatf(c->buffer, c->buffer_size, "Content-Type: %s\r\n", mime_type);
    av_strlcatf(c->buffer, c->buffer_size, "\r\n");
    q = c->buffer + strlen(c->buffer);

    /* prepare output buffer */
    c->http_error = 0;
    c->buffer_ptr = c->buffer;
    c->buffer_end = q;
    c->state = HTTPSTATE_SEND_HEADER;
    return 0;
 send_error:
    c->http_error = 404;
    q = c->buffer;
    if (!htmlencode(msg, &encoded_msg)) {
        http_log("Could not encode filename '%s' as HTML\n", msg);
    }
    snprintf(q, c->buffer_size,
                  "HTTP/1.0 404 Not Found\r\n"
                  "Content-type: text/html\r\n"
                  "\r\n"
                  "<!DOCTYPE html>\n"
                  "<html>\n"
                  "<head>\n"
                  "<meta charset=\"UTF-8\">\n"
                  "<title>404 Not Found</title>\n"
                  "</head>\n"
                  "<body>%s</body>\n"
                  "</html>\n", encoded_msg? encoded_msg : "File not found");
    q += strlen(q);
    /* prepare output buffer */
    c->buffer_ptr = c->buffer;
    c->buffer_end = q;
    c->state = HTTPSTATE_SEND_HEADER;
    av_freep(&encoded_msg);
    return 0;
 send_status:
    compute_status(c);
    /* horrible: we use this value to avoid
     * going to the send data state */
    c->http_error = 200;
    c->state = HTTPSTATE_SEND_HEADER;
    return 0;
}

static void log_connection(HTTPContext *c)
{
    if (c->suppress_log)
        return;

    http_log("%s - - [%s] \"%s %s\" %d %"PRId64"\n",
             inet_ntoa(c->from_addr.sin_addr), c->method, c->url,
             c->protocol, (c->http_error ? c->http_error : 200), c->data_count);
}

static int http_receive_data(HTTPContext *c)
{
    HTTPContext *c1;
    int len, loop_run = 0;

    while (c->chunked_encoding && !c->chunk_size &&
           c->buffer_end > c->buffer_ptr) {
        /* read chunk header, if present */
        len = recv(c->fd, c->buffer_ptr, 1, 0);

        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR))
                /* error : close connection */
                goto fail;
            return 0;
        } else if (len == 0) {
            /* end of connection : close it */
            goto fail;
        } else if (c->buffer_ptr - c->buffer >= 2 &&
                   !memcmp(c->buffer_ptr - 1, "\r\n", 2)) {
            c->chunk_size = strtol(c->buffer, 0, 16);
            if (c->chunk_size <= 0) { // end of stream or invalid chunk size
                c->chunk_size = 0;
                goto fail;
            }
            c->buffer_ptr = c->buffer;
            break;
        } else if (++loop_run > 10)
            /* no chunk header, abort */
            goto fail;
        else
            c->buffer_ptr++;
    }

    if (c->buffer_end > c->buffer_ptr) {
        len = recv(c->fd, c->buffer_ptr,
                   FFMIN(c->chunk_size, c->buffer_end - c->buffer_ptr), 0);
        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR))
                /* error : close connection */
                goto fail;
        } else if (len == 0)
            /* end of connection : close it */
            goto fail;
        else {
            av_assert0(len <= c->chunk_size);
            c->chunk_size -= len;
            c->buffer_ptr += len;
            c->data_count += len;
            update_datarate(&c->datarate, c->data_count);
        }
    }

    if (c->buffer_ptr - c->buffer >= 2 && c->data_count > FFM_PACKET_SIZE) {
        if (c->buffer[0] != 'f' ||
            c->buffer[1] != 'm') {
            http_log("Feed stream has become desynchronized -- disconnecting\n");
            goto fail;
        }
    }

    if (c->buffer_ptr >= c->buffer_end) {
        FFServerStream *feed = c->stream;
        /* a packet has been received : write it in the store, except
         * if header */
        if (c->data_count > FFM_PACKET_SIZE) {
            /* XXX: use llseek or url_seek
             * XXX: Should probably fail? */
            if (lseek(c->feed_fd, feed->feed_write_index, SEEK_SET) == -1)
                http_log("Seek to %"PRId64" failed\n", feed->feed_write_index);

            if (write(c->feed_fd, c->buffer, FFM_PACKET_SIZE) < 0) {
                http_log("Error writing to feed file: %s\n", strerror(errno));
                goto fail;
            }

            feed->feed_write_index += FFM_PACKET_SIZE;
            /* update file size */
            if (feed->feed_write_index > c->stream->feed_size)
                feed->feed_size = feed->feed_write_index;

            /* handle wrap around if max file size reached */
            if (c->stream->feed_max_size &&
                feed->feed_write_index >= c->stream->feed_max_size)
                feed->feed_write_index = FFM_PACKET_SIZE;

            /* write index */
            if (ffm_write_write_index(c->feed_fd, feed->feed_write_index) < 0) {
                http_log("Error writing index to feed file: %s\n",
                         strerror(errno));
                goto fail;
            }

            /* wake up any waiting connections */
            for(c1 = first_http_ctx; c1; c1 = c1->next) {
                if (c1->state == HTTPSTATE_WAIT_FEED &&
                    c1->stream->feed == c->stream->feed)
                    c1->state = HTTPSTATE_SEND_DATA;
            }
        } else {
            /* We have a header in our hands that contains useful data */
            AVFormatContext *s = avformat_alloc_context();
            AVIOContext *pb;
            AVInputFormat *fmt_in;
            int i;

            if (!s)
                goto fail;

            /* use feed output format name to find corresponding input format */
            fmt_in = av_find_input_format(feed->fmt->name);
            if (!fmt_in)
                goto fail;

            pb = avio_alloc_context(c->buffer, c->buffer_end - c->buffer,
                                    0, NULL, NULL, NULL, NULL);
            if (!pb)
                goto fail;

            pb->seekable = 0;

            s->pb = pb;
            if (avformat_open_input(&s, c->stream->feed_filename, fmt_in, NULL) < 0) {
                av_freep(&pb);
                goto fail;
            }

            /* Now we have the actual streams */
            if (s->nb_streams != feed->nb_streams) {
                avformat_close_input(&s);
                av_freep(&pb);
                http_log("Feed '%s' stream number does not match registered feed\n",
                         c->stream->feed_filename);
                goto fail;
            }

            for (i = 0; i < s->nb_streams; i++) {
                LayeredAVStream *fst = feed->streams[i];
                AVStream *st = s->streams[i];
                avcodec_parameters_to_context(fst->codec, st->codecpar);
                avcodec_parameters_from_context(fst->codecpar, fst->codec);
            }

            avformat_close_input(&s);
            av_freep(&pb);
        }
        c->buffer_ptr = c->buffer;
    }

    return 0;
 fail:
    c->stream->feed_opened = 0;
    close(c->feed_fd);
    /* wake up any waiting connections to stop waiting for feed */
    for(c1 = first_http_ctx; c1; c1 = c1->next) {
        if (c1->state == HTTPSTATE_WAIT_FEED &&
            c1->stream->feed == c->stream->feed)
            c1->state = HTTPSTATE_SEND_DATA_TRAILER;
    }
    return -1;
}

/********************************************************************/
/* RTSP handling */

static void rtsp_reply_header(HTTPContext *c, enum RTSPStatusCode error_number)
{
    const char *str;
    time_t ti;
    struct tm *tm;
    char buf2[32];

    str = RTSP_STATUS_CODE2STRING(error_number);
    if (!str)
        str = "Unknown Error";

    avio_printf(c->pb, "RTSP/1.0 %d %s\r\n", error_number, str);
    avio_printf(c->pb, "CSeq: %d\r\n", c->seq);

    /* output GMT time */
    ti = time(NULL);
    tm = gmtime(&ti);
    strftime(buf2, sizeof(buf2), "%a, %d %b %Y %H:%M:%S", tm);
    avio_printf(c->pb, "Date: %s GMT\r\n", buf2);
}

static void rtsp_reply_error(HTTPContext *c, enum RTSPStatusCode error_number)
{
    rtsp_reply_header(c, error_number);
    avio_printf(c->pb, "\r\n");
}

static HTTPContext *find_rtp_session(const char *session_id)
{
    HTTPContext *c;

    if (session_id[0] == '\0')
        return NULL;

    for(c = first_http_ctx; c; c = c->next) {
        if (!strcmp(c->session_id, session_id))
            return c;
    }
    return NULL;
}

static RTSPTransportField *find_transport(RTSPMessageHeader *h, enum RTSPLowerTransport lower_transport)
{
    RTSPTransportField *th;
    int i;

    for(i=0;i<h->nb_transports;i++) {
        th = &h->transports[i];
        if (th->lower_transport == lower_transport)
            return th;
    }
    return NULL;
}

/**
 * find an RTP connection by using the session ID. Check consistency
 * with filename
 */
static HTTPContext *find_rtp_session_with_url(const char *url,
                                              const char *session_id)
{
    HTTPContext *rtp_c;
    char path1[1024];
    const char *path;
    char buf[1024];
    int s, len;

    rtp_c = find_rtp_session(session_id);
    if (!rtp_c)
        return NULL;

    /* find which URL is asked */
    av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path1, sizeof(path1), url);
    path = path1;
    if (*path == '/')
        path++;
    if(!strcmp(path, rtp_c->stream->filename)) return rtp_c;
    for(s=0; s<rtp_c->stream->nb_streams; ++s) {
      snprintf(buf, sizeof(buf), "%s/streamid=%d",
        rtp_c->stream->filename, s);
      if(!strncmp(path, buf, sizeof(buf)))
        /* XXX: Should we reply with RTSP_STATUS_ONLY_AGGREGATE
         * if nb_streams>1? */
        return rtp_c;
    }
    len = strlen(path);
    if (len > 0 && path[len - 1] == '/' &&
        !strncmp(path, rtp_c->stream->filename, len - 1))
        return rtp_c;
    return NULL;
}

static void rtsp_cmd_interrupt(HTTPContext *c, const char *url,
                               RTSPMessageHeader *h, int pause_only)
{
    HTTPContext *rtp_c;

    rtp_c = find_rtp_session_with_url(url, h->session_id);
    if (!rtp_c) {
        rtsp_reply_error(c, RTSP_STATUS_SESSION);
        return;
    }

    if (pause_only) {
        if (rtp_c->state != HTTPSTATE_SEND_DATA &&
            rtp_c->state != HTTPSTATE_WAIT_FEED) {
            rtsp_reply_error(c, RTSP_STATUS_STATE);
            return;
        }
        rtp_c->state = HTTPSTATE_READY;
        rtp_c->first_pts = AV_NOPTS_VALUE;
    }

    /* now everything is OK, so we can send the connection parameters */
    rtsp_reply_header(c, RTSP_STATUS_OK);
    /* session ID */
    avio_printf(c->pb, "Session: %s\r\n", rtp_c->session_id);
    avio_printf(c->pb, "\r\n");

    if (!pause_only)
        close_connection(rtp_c);
}

static void rtsp_cmd_describe(HTTPContext *c, const char *url)
{
    FFServerStream *stream;
    char path1[1024];
    const char *path;
    uint8_t *content;
    int content_length;
    socklen_t len;
    struct sockaddr_in my_addr;

    /* find which URL is asked */
    av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path1, sizeof(path1), url);
    path = path1;
    if (*path == '/')
        path++;

    for(stream = config.first_stream; stream; stream = stream->next) {
        if (!stream->is_feed &&
            stream->fmt && !strcmp(stream->fmt->name, "rtp") &&
            !strcmp(path, stream->filename)) {
            goto found;
        }
    }
    /* no stream found */
    rtsp_reply_error(c, RTSP_STATUS_NOT_FOUND);
    return;

 found:
    /* prepare the media description in SDP format */

    /* get the host IP */
    len = sizeof(my_addr);
    getsockname(c->fd, (struct sockaddr *)&my_addr, &len);
    content_length = prepare_sdp_description(stream, &content,
                                             my_addr.sin_addr);
    if (content_length < 0) {
        rtsp_reply_error(c, RTSP_STATUS_INTERNAL);
        return;
    }
    rtsp_reply_header(c, RTSP_STATUS_OK);
    avio_printf(c->pb, "Content-Base: %s/\r\n", url);
    avio_printf(c->pb, "Content-Type: application/sdp\r\n");
    avio_printf(c->pb, "Content-Length: %d\r\n", content_length);
    avio_printf(c->pb, "\r\n");
    avio_write(c->pb, content, content_length);
    av_free(content);
}

static void rtsp_cmd_options(HTTPContext *c, const char *url)
{
    /* rtsp_reply_header(c, RTSP_STATUS_OK); */
    avio_printf(c->pb, "RTSP/1.0 %d %s\r\n", RTSP_STATUS_OK, "OK");
    avio_printf(c->pb, "CSeq: %d\r\n", c->seq);
    avio_printf(c->pb, "Public: %s\r\n",
                "OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE");
    avio_printf(c->pb, "\r\n");
}

static void rtsp_cmd_setup(HTTPContext *c, const char *url,
                           RTSPMessageHeader *h)
{
    FFServerStream *stream;
    int stream_index, rtp_port, rtcp_port;
    char buf[1024];
    char path1[1024];
    const char *path;
    HTTPContext *rtp_c;
    RTSPTransportField *th;
    struct sockaddr_in dest_addr;
    RTSPActionServerSetup setup;

    /* find which URL is asked */
    av_url_split(NULL, 0, NULL, 0, NULL, 0, NULL, path1, sizeof(path1), url);
    path = path1;
    if (*path == '/')
        path++;

    /* now check each stream */
    for(stream = config.first_stream; stream; stream = stream->next) {
        if (stream->is_feed || !stream->fmt ||
            strcmp(stream->fmt->name, "rtp")) {
            continue;
        }
        /* accept aggregate filenames only if single stream */
        if (!strcmp(path, stream->filename)) {
            if (stream->nb_streams != 1) {
                rtsp_reply_error(c, RTSP_STATUS_AGGREGATE);
                return;
            }
            stream_index = 0;
            goto found;
        }

        for(stream_index = 0; stream_index < stream->nb_streams;
            stream_index++) {
            snprintf(buf, sizeof(buf), "%s/streamid=%d",
                     stream->filename, stream_index);
            if (!strcmp(path, buf))
                goto found;
        }
    }
    /* no stream found */
    rtsp_reply_error(c, RTSP_STATUS_SERVICE); /* XXX: right error ? */
    return;
 found:

    /* generate session id if needed */
    if (h->session_id[0] == '\0') {
        unsigned random0 = av_lfg_get(&random_state);
        unsigned random1 = av_lfg_get(&random_state);
        snprintf(h->session_id, sizeof(h->session_id), "%08x%08x",
                 random0, random1);
    }

    /* find RTP session, and create it if none found */
    rtp_c = find_rtp_session(h->session_id);
    if (!rtp_c) {
        /* always prefer UDP */
        th = find_transport(h, RTSP_LOWER_TRANSPORT_UDP);
        if (!th) {
            th = find_transport(h, RTSP_LOWER_TRANSPORT_TCP);
            if (!th) {
                rtsp_reply_error(c, RTSP_STATUS_TRANSPORT);
                return;
            }
        }

        rtp_c = rtp_new_connection(&c->from_addr, stream, h->session_id,
                                   th->lower_transport);
        if (!rtp_c) {
            rtsp_reply_error(c, RTSP_STATUS_BANDWIDTH);
            return;
        }

        /* open input stream */
        if (open_input_stream(rtp_c, "") < 0) {
            rtsp_reply_error(c, RTSP_STATUS_INTERNAL);
            return;
        }
    }

    /* test if stream is OK (test needed because several SETUP needs
     * to be done for a given file) */
    if (rtp_c->stream != stream) {
        rtsp_reply_error(c, RTSP_STATUS_SERVICE);
        return;
    }

    /* test if stream is already set up */
    if (rtp_c->rtp_ctx[stream_index]) {
        rtsp_reply_error(c, RTSP_STATUS_STATE);
        return;
    }

    /* check transport */
    th = find_transport(h, rtp_c->rtp_protocol);
    if (!th || (th->lower_transport == RTSP_LOWER_TRANSPORT_UDP &&
                th->client_port_min <= 0)) {
        rtsp_reply_error(c, RTSP_STATUS_TRANSPORT);
        return;
    }

    /* setup default options */
    setup.transport_option[0] = '\0';
    dest_addr = rtp_c->from_addr;
    dest_addr.sin_port = htons(th->client_port_min);

    /* setup stream */
    if (rtp_new_av_stream(rtp_c, stream_index, &dest_addr, c) < 0) {
        rtsp_reply_error(c, RTSP_STATUS_TRANSPORT);
        return;
    }

    /* now everything is OK, so we can send the connection parameters */
    rtsp_reply_header(c, RTSP_STATUS_OK);
    /* session ID */
    avio_printf(c->pb, "Session: %s\r\n", rtp_c->session_id);

    switch(rtp_c->rtp_protocol) {
    case RTSP_LOWER_TRANSPORT_UDP:
        rtp_port = ff_rtp_get_local_rtp_port(rtp_c->rtp_handles[stream_index]);
        rtcp_port = ff_rtp_get_local_rtcp_port(rtp_c->rtp_handles[stream_index]);
        avio_printf(c->pb, "Transport: RTP/AVP/UDP;unicast;"
                    "client_port=%d-%d;server_port=%d-%d",
                    th->client_port_min, th->client_port_max,
                    rtp_port, rtcp_port);
        break;
    case RTSP_LOWER_TRANSPORT_TCP:
        avio_printf(c->pb, "Transport: RTP/AVP/TCP;interleaved=%d-%d",
                    stream_index * 2, stream_index * 2 + 1);
        break;
    default:
        break;
    }
    if (setup.transport_option[0] != '\0')
        avio_printf(c->pb, ";%s", setup.transport_option);
    avio_printf(c->pb, "\r\n");


    avio_printf(c->pb, "\r\n");
}

static void rtsp_cmd_play(HTTPContext *c, const char *url, RTSPMessageHeader *h)
{
    HTTPContext *rtp_c;

    rtp_c = find_rtp_session_with_url(url, h->session_id);
    if (!rtp_c) {
        rtsp_reply_error(c, RTSP_STATUS_SESSION);
        return;
    }

    if (rtp_c->state != HTTPSTATE_SEND_DATA &&
        rtp_c->state != HTTPSTATE_WAIT_FEED &&
        rtp_c->state != HTTPSTATE_READY) {
        rtsp_reply_error(c, RTSP_STATUS_STATE);
        return;
    }

    rtp_c->state = HTTPSTATE_SEND_DATA;

    /* now everything is OK, so we can send the connection parameters */
    rtsp_reply_header(c, RTSP_STATUS_OK);
    /* session ID */
    avio_printf(c->pb, "Session: %s\r\n", rtp_c->session_id);
    avio_printf(c->pb, "\r\n");
}

static int rtsp_parse_request(HTTPContext *c)
{
    const char *p, *p1, *p2;
    char cmd[32];
    char url[1024];
    char protocol[32];
    char line[1024];
    int len;
    RTSPMessageHeader header1 = { 0 }, *header = &header1;

    c->buffer_ptr[0] = '\0';
    p = c->buffer;

    get_word(cmd, sizeof(cmd), &p);
    get_word(url, sizeof(url), &p);
    get_word(protocol, sizeof(protocol), &p);

    av_strlcpy(c->method, cmd, sizeof(c->method));
    av_strlcpy(c->url, url, sizeof(c->url));
    av_strlcpy(c->protocol, protocol, sizeof(c->protocol));

    if (avio_open_dyn_buf(&c->pb) < 0) {
        /* XXX: cannot do more */
        c->pb = NULL; /* safety */
        return -1;
    }

    /* check version name */
    if (strcmp(protocol, "RTSP/1.0")) {
        rtsp_reply_error(c, RTSP_STATUS_VERSION);
        goto the_end;
    }

    /* parse each header line */
    /* skip to next line */
    while (*p != '\n' && *p != '\0')
        p++;
    if (*p == '\n')
        p++;
    while (*p != '\0') {
        p1 = memchr(p, '\n', (char *)c->buffer_ptr - p);
        if (!p1)
            break;
        p2 = p1;
        if (p2 > p && p2[-1] == '\r')
            p2--;
        /* skip empty line */
        if (p2 == p)
            break;
        len = p2 - p;
        if (len > sizeof(line) - 1)
            len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        ff_rtsp_parse_line(NULL, header, line, NULL, NULL);
        p = p1 + 1;
    }

    /* handle sequence number */
    c->seq = header->seq;

    if (!strcmp(cmd, "DESCRIBE"))
        rtsp_cmd_describe(c, url);
    else if (!strcmp(cmd, "OPTIONS"))
        rtsp_cmd_options(c, url);
    else if (!strcmp(cmd, "SETUP"))
        rtsp_cmd_setup(c, url, header);
    else if (!strcmp(cmd, "PLAY"))
        rtsp_cmd_play(c, url, header);
    else if (!strcmp(cmd, "PAUSE"))
        rtsp_cmd_interrupt(c, url, header, 1);
    else if (!strcmp(cmd, "TEARDOWN"))
        rtsp_cmd_interrupt(c, url, header, 0);
    else
        rtsp_reply_error(c, RTSP_STATUS_METHOD);

 the_end:
    len = avio_close_dyn_buf(c->pb, &c->pb_buffer);
    c->pb = NULL; /* safety */
    if (len < 0)
        /* XXX: cannot do more */
        return -1;

    c->buffer_ptr = c->pb_buffer;
    c->buffer_end = c->pb_buffer + len;
    c->state = RTSPSTATE_SEND_REPLY;
    return 0;
}

static int handle_connection(HTTPContext *c)
{
    int len, ret;
    uint8_t *ptr;

    switch(c->state) {
    case HTTPSTATE_WAIT_REQUEST:
    case RTSPSTATE_WAIT_REQUEST:
        /* timeout ? */
        if ((c->timeout - cur_time) < 0)
            return -1;
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
            return -1;

        /* no need to read if no events */
        if (!(c->poll_entry->revents & POLLIN))
            return 0;
        /* read the data */
    read_loop:
        if (!(len = recv(c->fd, c->buffer_ptr, 1, 0)))
            return -1;

        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR))
                return -1;
            break;
        }
        /* search for end of request. */
        c->buffer_ptr += len;
        ptr = c->buffer_ptr;
        if ((ptr >= c->buffer + 2 && !memcmp(ptr-2, "\n\n", 2)) ||
            (ptr >= c->buffer + 4 && !memcmp(ptr-4, "\r\n\r\n", 4))) {
            /* request found : parse it and reply */
            if (c->state == HTTPSTATE_WAIT_REQUEST)
                ret = http_parse_request(c);
            else
                ret = rtsp_parse_request(c);

            if (ret < 0)
                return -1;
        } else if (ptr >= c->buffer_end) {
            /* request too long: cannot do anything */
            return -1;
        } else goto read_loop;

        break;

    case HTTPSTATE_SEND_HEADER:
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
            return -1;

        /* no need to write if no events */
        if (!(c->poll_entry->revents & POLLOUT))
            return 0;
        len = send(c->fd, c->buffer_ptr, c->buffer_end - c->buffer_ptr, 0);
        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR)) {
                goto close_connection;
            }
            break;
        }
        c->buffer_ptr += len;
        if (c->stream)
            c->stream->bytes_served += len;
        c->data_count += len;
        if (c->buffer_ptr >= c->buffer_end) {
            av_freep(&c->pb_buffer);
            /* if error, exit */
            if (c->http_error)
                return -1;
            /* all the buffer was sent : synchronize to the incoming
             * stream */
            c->state = HTTPSTATE_SEND_DATA_HEADER;
            c->buffer_ptr = c->buffer_end = c->buffer;
        }
        break;

    case HTTPSTATE_SEND_DATA:
    case HTTPSTATE_SEND_DATA_HEADER:
    case HTTPSTATE_SEND_DATA_TRAILER:
        /* for packetized output, we consider we can always write (the
         * input streams set the speed). It may be better to verify
         * that we do not rely too much on the kernel queues */
        if (!c->is_packetized) {
            if (c->poll_entry->revents & (POLLERR | POLLHUP))
                return -1;

            /* no need to read if no events */
            if (!(c->poll_entry->revents & POLLOUT))
                return 0;
        }
        if (http_send_data(c) < 0)
            return -1;
        /* close connection if trailer sent */
        if (c->state == HTTPSTATE_SEND_DATA_TRAILER)
            return -1;
        /* Check if it is a single jpeg frame 123 */
        if (c->stream->single_frame && c->data_count > c->cur_frame_bytes && c->cur_frame_bytes > 0) {
            close_connection(c);
        }
        break;
    case HTTPSTATE_RECEIVE_DATA:
        /* no need to read if no events */
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
            return -1;
        if (!(c->poll_entry->revents & POLLIN))
            return 0;
        if (http_receive_data(c) < 0)
            return -1;
        break;
    case HTTPSTATE_WAIT_FEED:
        /* no need to read if no events */
        if (c->poll_entry->revents & (POLLIN | POLLERR | POLLHUP))
            return -1;

        /* nothing to do, we'll be waken up by incoming feed packets */
        break;

    case RTSPSTATE_SEND_REPLY:
        if (c->poll_entry->revents & (POLLERR | POLLHUP))
            goto close_connection;
        /* no need to write if no events */
        if (!(c->poll_entry->revents & POLLOUT))
            return 0;
        len = send(c->fd, c->buffer_ptr, c->buffer_end - c->buffer_ptr, 0);
        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR)) {
                goto close_connection;
            }
            break;
        }
        c->buffer_ptr += len;
        c->data_count += len;
        if (c->buffer_ptr >= c->buffer_end) {
            /* all the buffer was sent : wait for a new request */
            av_freep(&c->pb_buffer);
            start_wait_request(c, 1);
        }
        break;
    case RTSPSTATE_SEND_PACKET:
        if (c->poll_entry->revents & (POLLERR | POLLHUP)) {
            av_freep(&c->packet_buffer);
            return -1;
        }
        /* no need to write if no events */
        if (!(c->poll_entry->revents & POLLOUT))
            return 0;
        printf("%s %d\n", __func__, __LINE__);
        len = send(c->fd, c->packet_buffer_ptr,
                    c->packet_buffer_end - c->packet_buffer_ptr, 0);
        if (len < 0) {
            if (ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR)) {
                /* error : close connection */
                av_freep(&c->packet_buffer);
                return -1;
            }
            break;
        }
        c->packet_buffer_ptr += len;
        if (c->packet_buffer_ptr >= c->packet_buffer_end) {
            /* all the buffer was sent : wait for a new request */
            av_freep(&c->packet_buffer);
            c->state = RTSPSTATE_WAIT_REQUEST;
        }
        break;
    case HTTPSTATE_READY:
        /* nothing to do */
        break;
    default:
        return -1;
    }
    return 0;

close_connection:
    av_freep(&c->pb_buffer);
    return -1;
}

/* main loop of the HTTP server */
static int http_server(void)
{
    int server_fd = 0, rtsp_server_fd = 0;
    int ret, delay;
    struct pollfd *poll_table, *poll_entry;
    HTTPContext *c, *c_next;

    poll_table = av_mallocz_array(config.nb_max_http_connections + 2,
                                  sizeof(*poll_table));
    if(!poll_table) {
        http_log("Impossible to allocate a poll table handling %d "
                 "connections.\n", config.nb_max_http_connections);
        return -1;
    }

    if (config.http_addr.sin_port) {
        server_fd = socket_open_listen(&config.http_addr);
        if (server_fd < 0)
            goto quit;
    }

    if (config.rtsp_addr.sin_port) {
        rtsp_server_fd = socket_open_listen(&config.rtsp_addr);
        if (rtsp_server_fd < 0) {
            closesocket(server_fd);
            goto quit;
        }
    }

    if (!rtsp_server_fd && !server_fd) {
        http_log("HTTP and RTSP disabled.\n");
        goto quit;
    }

    http_log("FFserver started.\n");

    //start_children(config.first_feed);

    start_multicast();

    for(;;) {
        poll_entry = poll_table;
        if (server_fd) {
            poll_entry->fd = server_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
        }
        if (rtsp_server_fd) {
            poll_entry->fd = rtsp_server_fd;
            poll_entry->events = POLLIN;
            poll_entry++;
        }

        /* wait for events on each HTTP handle */
        c = first_http_ctx;
        delay = 1000;
        while (c) {
            int fd;
            fd = c->fd;
            switch(c->state) {
            case HTTPSTATE_SEND_HEADER:
            case RTSPSTATE_SEND_REPLY:
            case RTSPSTATE_SEND_PACKET:
                c->poll_entry = poll_entry;
                poll_entry->fd = fd;
                poll_entry->events = POLLOUT;
                poll_entry++;
                break;
            case HTTPSTATE_SEND_DATA_HEADER:
            case HTTPSTATE_SEND_DATA:
            case HTTPSTATE_SEND_DATA_TRAILER:
                if (!c->is_packetized) {
                    /* for TCP, we output as much as we can
                     * (may need to put a limit) */
                    c->poll_entry = poll_entry;
                    poll_entry->fd = fd;
                    poll_entry->events = POLLOUT;
                    poll_entry++;
                } else {
                    /* when ffserver is doing the timing, we work by
                     * looking at which packet needs to be sent every
                     * 10 ms (one tick wait XXX: 10 ms assumed) */
                    if (delay > 10)
                        delay = 10;
                }
                break;
            case HTTPSTATE_WAIT_REQUEST:
            case HTTPSTATE_RECEIVE_DATA:
            case HTTPSTATE_WAIT_FEED:
            case RTSPSTATE_WAIT_REQUEST:
                /* need to catch errors */
                c->poll_entry = poll_entry;
                poll_entry->fd = fd;
                poll_entry->events = POLLIN;/* Maybe this will work */
                poll_entry++;
                break;
            default:
                c->poll_entry = NULL;
                break;
            }
            c = c->next;
        }

        /* wait for an event on one connection. We poll at least every
         * second to handle timeouts */
        do {
            ret = poll(poll_table, poll_entry - poll_table, delay);
            if (ret < 0 && ff_neterrno() != AVERROR(EAGAIN) &&
                ff_neterrno() != AVERROR(EINTR)) {
                goto quit;
            }
        } while (ret < 0);

        cur_time = av_gettime() / 1000;

        if (need_to_start_children) {
            need_to_start_children = 0;
            //start_children(config.first_feed);
        }

        /* now handle the events */
        for(c = first_http_ctx; c; c = c_next) {
            c_next = c->next;
            if (handle_connection(c) < 0) {
                log_connection(c);
                /* close and free the connection */
                close_connection(c);
            }
        }

        poll_entry = poll_table;
        if (server_fd) {
            /* new HTTP connection request ? */
            if (poll_entry->revents & POLLIN)
                new_connection(server_fd, 0);
            poll_entry++;
        }
        if (rtsp_server_fd) {
            /* new RTSP connection request ? */
            if (poll_entry->revents & POLLIN)
                new_connection(rtsp_server_fd, 1);
        }
    }

quit:
    av_free(poll_table);
    return -1;
}


int main(int argc, char **argv)
{
    struct sigaction sigact = { { 0 } };
    int cfg_parsed;
    int ret = EXIT_FAILURE;

    config.filename = av_strdup("/etc/ffserver.conf");

    av_register_all();
    avformat_network_init();

    my_program_name = argv[0];

//    parse_options(NULL, argc, argv, options, NULL);

    unsetenv("http_proxy");             /* Kill the http_proxy */

    av_lfg_init(&random_state, av_get_random_seed());

    sigact.sa_handler = handle_child_exit;
    sigact.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigaction(SIGCHLD, &sigact, 0);
    printf("%s %d\n", __func__, __LINE__);
    if ((cfg_parsed = ffserver_parse_ffconfig(config.filename, &config)) < 0) {
        fprintf(stderr, "Error reading configuration file '%s': %s\n",
                config.filename, av_err2str(cfg_parsed));
        goto bail;
    }

    /* open log file if needed */
    if (config.logfilename[0] != '\0') {
        if (!strcmp(config.logfilename, "-"))
            logfile = stdout;
        else
            logfile = fopen(config.logfilename, "a");
        av_log_set_callback(http_av_log);
    }

    build_file_streams();

    compute_bandwidth();
    /* signal init */
    signal(SIGPIPE, SIG_IGN);

    if (http_server() < 0) {
        http_log("Could not start server\n");
        goto bail;
    }
    ret=EXIT_SUCCESS;

bail:
    av_freep (&config.filename);
    avformat_network_deinit();
    return ret;
}
