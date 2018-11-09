#include <stdint.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/wait.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avassert.h>
#include <libavutil/lfg.h>
#include <libavutil/avstring.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <libavutil/opt.h>
#include <libavformat/url.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libavutil/intreadwrite.h>
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_ADDRINFO 1
#include <libavformat/rtsp.h>
#include <unistd.h>
#include <poll.h>

#define FFSERVER_MAX_STREAMS 20
#define MAX_CHILD_ARGS 64
#define closesocket close
#define ERROR(...) report_config_error(config->filename, config->line_num,\
                                       AV_LOG_ERROR, &config->errors, __VA_ARGS__)
#define WARNING(...) report_config_error(config->filename, config->line_num,\
                                         AV_LOG_WARNING, &config->warnings, __VA_ARGS__)
#define FLT_MAX 3.40282346638528859812e+38F

#define FLT_MIN 1.17549435082228750797e-38F

#define DBL_MAX ((double)1.79769313486231570815e+308L)

#define DBL_MIN ((double)2.22507385850720138309e-308L)
#define AV_OPT_FLAG_AUDIO_PARAM     8
#define AV_OPT_FLAG_VIDEO_PARAM     16
#define AV_OPT_FLAG_SUBTITLE_PARAM  32
#define FFMPEG_DATADIR "/usr/local/share/ffmpeg"
#define IOBUFFER_INIT_SIZE 8192
#define FFM_PACKET_SIZE 4096
#define RTSP_TCP_MAX_PACKET_SIZE 1472
/* timeouts are in ms */
#define HTTP_REQUEST_TIMEOUT (15 * 1000)
#define RTSP_REQUEST_TIMEOUT (3600 * 24 * 1000)

//#define POLLIN     0x0001  /* any readable data available */
//#define POLLOUT    0x0002  /* file descriptor is writeable */
#define ff_neterrno() AVERROR(errno)
#pragma GCC diagnostic ignored "-Wpointer-sign"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wparentheses"
/* maximum number of simultaneous HTTP connections */
static unsigned int nb_connections;
static FILE *logfile = NULL;
/* Making this global saves on passing it around everywhere */
static int64_t cur_time;
int ff_inet_aton(const char *str, struct in_addr *add);
int av_parse_video_size(int *width_ptr, int *height_ptr, const char *str);
enum AVPixelFormat av_get_pix_fmt(const char *name);
static void report_config_error(const char *filename, int line_num,
                                int log_level, int *errors, const char *fmt,
                                ...);
int ff_socket_nonblock(int socket, int enable);
int av_find_info_tag(char *arg, int arg_size, const char *tag1, const char *info);
int av_parse_time(int64_t *timeval, const char *timestr, int duration);
int64_t av_gettime(void);
int ffio_set_buf_size(AVIOContext *s, int buf_size);
int ff_rtp_get_local_rtp_port(URLContext *h);
int ff_rtp_get_local_rtcp_port(URLContext *h);
static uint64_t current_bandwidth;
static void vreport_config_error(const char *filename, int line_num,
                                 int log_level, int *errors, const char *fmt,
                                 va_list vl)
{
    av_log(NULL, log_level, "%s:%d: ", filename, line_num);
    av_vlog(NULL, log_level, fmt, vl);
    if (errors)
        (*errors)++;
}

static void report_config_error(const char *filename, int line_num,
                                int log_level, int *errors,
                                const char *fmt, ...)
{
    va_list vl;
    va_start(vl, fmt);
    vreport_config_error(filename, line_num, log_level, errors, fmt, vl);
    va_end(vl);
}


/* each generated stream is described here */
enum FFServerStreamType {
    STREAM_TYPE_LIVE,
    STREAM_TYPE_STATUS,
    STREAM_TYPE_REDIRECT,
};

enum RedirType {
    REDIR_NONE,
    REDIR_ASX,
    REDIR_RAM,
    REDIR_ASF,
    REDIR_RTSP,
    REDIR_SDP,
};

enum FFServerIPAddressAction {
    IP_ALLOW = 1,
    IP_DENY, 
};  

static const char * const http_state[] = {
    "HTTP_WAIT_REQUEST",
    "HTTP_SEND_HEADER",

    "SEND_DATA_HEADER",
    "SEND_DATA",
    "SEND_DATA_TRAILER",
    "RECEIVE_DATA",
    "WAIT_FEED",
    "READY",

    "RTSP_WAIT_REQUEST",
    "RTSP_SEND_REPLY",
    "RTSP_SEND_PACKET",
};

typedef struct FFServerIPAddressACL {
    struct FFServerIPAddressACL *next;
    enum FFServerIPAddressAction action;
    /* These are in host order */
    struct in_addr first;
    struct in_addr last;
} FFServerIPAddressACL;


typedef struct LayeredAVStream {
    int index;
    int id;
    AVCodecParameters *codecpar;
    AVCodecContext *codec;
    AVRational time_base;
    int pts_wrap_bits;
    AVRational sample_aspect_ratio;
    char *recommended_encoder_configuration;
} LayeredAVStream;

/* description of each stream of the ffserver.conf file */
typedef struct FFServerStream {
    enum FFServerStreamType stream_type;
    char filename[1024];          /* stream filename */
    struct FFServerStream *feed;  /* feed we are using (can be null if coming from file) */
    AVDictionary *in_opts;        /* input parameters */
    AVDictionary *metadata;       /* metadata to set on the stream */
    AVInputFormat *ifmt;          /* if non NULL, force input format */
    AVOutputFormat *fmt;
    FFServerIPAddressACL *acl;
    char dynamic_acl[1024];
    int nb_streams;
    int prebuffer;                /* Number of milliseconds early to start */
    int64_t max_time;             /* Number of milliseconds to run */
    int send_on_key;
    LayeredAVStream *streams[FFSERVER_MAX_STREAMS];
    int feed_streams[FFSERVER_MAX_STREAMS]; /* index of streams in the feed */
    char feed_filename[1024];     /* file name of the feed storage, or
                                     input file name for a stream */
    pid_t pid;                    /* Of ffmpeg process */
    time_t pid_start;             /* Of ffmpeg process */
    char **child_argv;
    struct FFServerStream *next;
    unsigned bandwidth;           /* bandwidth, in kbits/s */
    /* RTSP options */
    char *rtsp_option;
    /* multicast specific */
    int is_multicast;
    struct in_addr multicast_ip;
    int multicast_port;           /* first port used for multicast */
    int multicast_ttl;
    int loop;                     /* if true, send the stream in loops (only meaningful if file) */
    char single_frame;            /* only single frame */

    /* feed specific */
    int feed_opened;              /* true if someone is writing to the feed */
    int is_feed;                  /* true if it is a feed */
    int readonly;                 /* True if writing is prohibited to the file */
    int truncate;                 /* True if feeder connection truncate the feed file */
    int conns_served;
    int64_t bytes_served;
    int64_t feed_max_size;        /* maximum storage size, zero means unlimited */
    int64_t feed_write_index;     /* current write position in feed (it wraps around) */
    int64_t feed_size;            /* current size of feed */
    struct FFServerStream *next_feed;
} FFServerStream;


typedef struct FFServerConfig {
    char *filename;
    FFServerStream *first_feed;   /* contains only feeds */
    FFServerStream *first_stream; /* contains all streams, including feeds */
    unsigned int nb_max_http_connections;
    unsigned int nb_max_connections;
    uint64_t max_bandwidth;
    int debug;
    int bitexact;
    char logfilename[1024];
    struct sockaddr_in http_addr;
    struct sockaddr_in rtsp_addr;
    int errors;
    int warnings;
    int use_defaults;
    // Following variables MUST NOT be used outside configuration parsing code.
    enum AVCodecID guessed_audio_codec_id;
    enum AVCodecID guessed_video_codec_id;
    AVDictionary *video_opts;     /* AVOptions for video encoder */
    AVDictionary *audio_opts;     /* AVOptions for audio encoder */
    AVCodecContext *dummy_actx;   /* Used internally to test audio AVOptions. */
    AVCodecContext *dummy_vctx;   /* Used internally to test video AVOptions. */
    int no_audio;
    int no_video;
    int line_num;
    int stream_use_defaults;
} FFServerConfig;

enum HTTPState {
    HTTPSTATE_WAIT_REQUEST,
    HTTPSTATE_SEND_HEADER,
    HTTPSTATE_SEND_DATA_HEADER,
    HTTPSTATE_SEND_DATA,          /* sending TCP or UDP data */
    HTTPSTATE_SEND_DATA_TRAILER,
    HTTPSTATE_RECEIVE_DATA,
    HTTPSTATE_WAIT_FEED,          /* wait for data from the feed */
    HTTPSTATE_READY,

    RTSPSTATE_WAIT_REQUEST,
    RTSPSTATE_SEND_REPLY,
    RTSPSTATE_SEND_PACKET,
};

typedef struct {
    int64_t count1, count2;
    int64_t time1, time2;
} DataRateData;

/**
 * Network layer over which RTP/etc packet data will be transported.
 */
#if 0
enum RTSPLowerTransport {
    RTSP_LOWER_TRANSPORT_UDP = 0,           /**< UDP/unicast */
    RTSP_LOWER_TRANSPORT_TCP = 1,           /**< TCP; interleaved in RTSP */
    RTSP_LOWER_TRANSPORT_UDP_MULTICAST = 2, /**< UDP/multicast */
    RTSP_LOWER_TRANSPORT_NB,
    RTSP_LOWER_TRANSPORT_HTTP = 8,          /**< HTTP tunneled - not a proper
                                                 transport mode as such,
                                                 only for use via AVOptions */
    RTSP_LOWER_TRANSPORT_CUSTOM = 16,       /**< Custom IO - not a public
                                                 option for lower_transport_mask,
                                                 but set in the SDP demuxer based
                                                 on a flag. */
};
struct pollfd {
    int fd;
    short events;  /* events to look for */
    short revents; /* events that occurred */
};
#endif
/* context associated with one connection */
typedef struct HTTPContext {
    enum HTTPState state;
    int fd; /* socket file descriptor */
    struct sockaddr_in from_addr; /* origin */
    struct pollfd *poll_entry; /* used when polling */
    int64_t timeout;
    uint8_t *buffer_ptr, *buffer_end;
    int http_error;
    int post;
    int chunked_encoding;
    int chunk_size;               /* 0 if it needs to be read */
    struct HTTPContext *next;
    int got_key_frame; /* stream 0 => 1, stream 1 => 2, stream 2=> 4 */
    int64_t data_count;
    /* feed input */
    int feed_fd;
    /* input format handling */
    AVFormatContext *fmt_in;
    int64_t start_time;            /* In milliseconds - this wraps fairly often */
    int64_t first_pts;            /* initial pts value */
    int64_t cur_pts;             /* current pts value from the stream in us */
    int64_t cur_frame_duration;  /* duration of the current frame in us */
    int cur_frame_bytes;       /* output frame size, needed to compute
                                  the time at which we send each
                                  packet */
    int pts_stream_index;        /* stream we choose as clock reference */
    int64_t cur_clock;           /* current clock reference value in us */
    /* output format handling */
    struct FFServerStream *stream;
    /* -1 is invalid stream */
    int feed_streams[FFSERVER_MAX_STREAMS]; /* index of streams in the feed */
    int switch_feed_streams[FFSERVER_MAX_STREAMS]; /* index of streams in the feed */
    int switch_pending;
    AVFormatContext *pfmt_ctx; /* instance of FFServerStream for one user */
    int last_packet_sent; /* true if last data packet was sent */
    int suppress_log;
    DataRateData datarate;
    int wmp_client_id;
    char protocol[16];
    char method[16];
    char url[128];
    char clean_url[128*7];
    int buffer_size;
    uint8_t *buffer;
    int is_packetized; /* if true, the stream is packetized */
    int packet_stream_index; /* current stream for output in state machine */

    /* RTSP state specific */
    uint8_t *pb_buffer; /* XXX: use that in all the code */
    AVIOContext *pb;
    int seq; /* RTSP sequence number */

    /* RTP state specific */
    enum RTSPLowerTransport rtp_protocol;
    char session_id[32]; /* session id */
    AVFormatContext *rtp_ctx[FFSERVER_MAX_STREAMS];

    /* RTP/UDP specific */
    URLContext *rtp_handles[FFSERVER_MAX_STREAMS];

    /* RTP/TCP specific */
    struct HTTPContext *rtsp_c;
    uint8_t *packet_buffer, *packet_buffer_ptr, *packet_buffer_end;
} HTTPContext;
static HTTPContext *first_http_ctx;
/* RTP handling */
static HTTPContext *rtp_new_connection(struct sockaddr_in *from_addr,
                                       FFServerStream *stream,
                                       const char *session_id,
                                       enum RTSPLowerTransport rtp_protocol);
static int rtp_new_av_stream(HTTPContext *c,
                             int stream_index, struct sockaddr_in *dest_addr,
                             HTTPContext *rtsp_c);
int ffio_open_dyn_packet_buf(AVIOContext **s, int max_packet_size);
