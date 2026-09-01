/*
 * isp_bridge_test.c
 * =========================================================================
 * 目的:验证 rkcif(/dev/video0) -> rkisp0-vir0 离线管线
 *      (rawrd0_m/video26 -> isp-subdev -> mainpath/video22)这条链路是否打通。
 *      只做单帧测试,不追求成像质量,不做真实3A闭环。
 *
 * 整体思路(对应 media-ctl 拓扑里的四个节点):
 *
 *   video0(采集raw) --EXPBUF导出dmabuf--> video26(写入raw给ISP)
 *                                              |
 *                                        isp-subdev处理
 *                                              |
 *   video30(写入3A参数,这里用全零) ------------>|
 *                                              v
 *                                       video22(读出YUV结果)
 *
 * 每一步都是标准 V4L2 API 的固定套路:
 *   REQBUFS(申请buffer) -> QUERYBUF/EXPBUF(查询/导出buffer) ->
 *   QBUF(把buffer交给内核) -> STREAMON(开始流) -> DQBUF(取回填好数据的buffer)
 *
 * !!! 重要提醒 !!!
 * - video30 的具体 buffer type 和参数结构体因内核/BSP版本而异,这里刻意
 *   只探测它要求的buffer大小再整块清零发送,不去猜测具体结构体字段。
 * - 这份代码没有在真实硬件上编译运行验证过,只是按V4L2标准API流程写的,
 *   拿到板子上大概率需要按实际报错调整。
 *
 * 编译: gcc -O2 -Wall -o isp_bridge_test isp_bridge_test.c
 * 运行: sudo ./isp_bridge_test
 * 运行(与 rkaiq_3A_server 共存,通过 rkaiq 取帧):
 *   sudo ./isp_bridge_test --via-rkaiq --video22 /dev/video31
 * =========================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/media.h>
#include <linux/videodev2.h>   /* 所有 V4L2_xxx 常量、struct v4l2_xxx 结构体都来自这个头文件 */

/* Some cross-toolchain sysroots ship older kernel headers without META_* buf types. */
#ifndef V4L2_BUF_TYPE_META_CAPTURE
#define V4L2_BUF_TYPE_META_CAPTURE 13
#endif
#ifndef V4L2_BUF_TYPE_META_OUTPUT
#define V4L2_BUF_TYPE_META_OUTPUT 14
#endif

/* ---- 设备节点路径,对应前面 media-ctl 拓扑分析出来的四个节点 ---- */
#define VIDEO0_RAW      "/dev/video0"   /* rkcif 采集raw(stream_cif_mipi_id0) */
#define VIDEO26_RAWWR   "/dev/video26"  /* rkisp_rawrd0_m,把raw写入给ISP     */
#define VIDEO30_PARAMS  "/dev/video30"  /* rkisp-input-params,写入3A调参数据  */
#define VIDEO22_YUV     "/dev/video22"  /* rkisp_mainpath,读取ISP处理后的YUV  */

static const char *g_video0_raw = VIDEO0_RAW;
static const char *g_video26_rawwr = VIDEO26_RAWWR;
static const char *g_video30_params = VIDEO30_PARAMS;
static const char *g_video22_yuv = VIDEO22_YUV;

static int g_allow_occupied = 0;
static int g_via_rkaiq = 0;

/* ---- 分辨率:raw是传感器裁剪后的原始尺寸,YUV是ISP输出给mainpath的目标尺寸 ---- */
#define RAW_W 3840
#define RAW_H 2160
#define YUV_W 1920
#define YUV_H 1080

/*
 * xioctl: ioctl() 的薄封装。
 *
 * 为什么需要它:Linux 的 ioctl() 调用如果被信号打断,会返回 -1 并把 errno
 * 设成 EINTR(表示"不是真错误,只是被打断了,请重试")。几乎所有 V4L2 编程
 * 范例都会包一层这样的重试逻辑,否则程序在多线程/有信号的环境下会莫名其妙
 * 报失败。
 *
 * 参数:
 *   fd  - 已经 open() 打开的设备文件描述符
 *   req - ioctl 请求码,比如 VIDIOC_QBUF、VIDIOC_S_FMT 等,这些宏最终会编码
 *         出"这次调用要读/写多大的数据结构、方向是读还是写"等信息
 *   arg - 指向对应结构体的指针,内核会读取/填写这块内存
 */
static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static void fourcc_to_str(__u32 fourcc, char out[5]) {
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
}

static const char *buf_type_name(enum v4l2_buf_type type) {
    switch (type) {
    case V4L2_BUF_TYPE_VIDEO_CAPTURE: return "VIDEO_CAPTURE";
    case V4L2_BUF_TYPE_VIDEO_OUTPUT: return "VIDEO_OUTPUT";
    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE: return "VIDEO_CAPTURE_MPLANE";
    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE: return "VIDEO_OUTPUT_MPLANE";
    case V4L2_BUF_TYPE_META_CAPTURE: return "META_CAPTURE";
    case V4L2_BUF_TYPE_META_OUTPUT: return "META_OUTPUT";
    default: return "UNKNOWN";
    }
}

static void dump_media_device_for_businfo(const char *bus_info, const char *tag);

static void dump_querycap(int fd, const char *tag) {
    struct v4l2_capability cap = {0};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[%s] VIDIOC_QUERYCAP 失败: %s\n", tag, strerror(errno));
        return;
    }
    fprintf(stderr, "[%s] driver=%s card=%s bus=%s version=%u caps=0x%08x dev_caps=0x%08x\n",
            tag, (char *)cap.driver, (char *)cap.card, (char *)cap.bus_info, cap.version,
            cap.capabilities, cap.device_caps);
    dump_media_device_for_businfo((const char *)cap.bus_info, tag);
}

static void dump_media_device_for_businfo(const char *bus_info, const char *tag) {
    if (!bus_info || !bus_info[0]) return;

    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/media%d", i);
        int mfd = open(path, O_RDONLY | O_NONBLOCK);
        if (mfd < 0) continue;

        struct media_device_info mdi = {0};
        if (ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &mdi) == 0) {
            if (strncmp((const char *)mdi.bus_info, bus_info, sizeof(mdi.bus_info)) == 0) {
                fprintf(stderr, "[%s] media=%s driver=%s model=%s bus=%s\n",
                        tag, path, mdi.driver, mdi.model, mdi.bus_info);
                close(mfd);
                return;
            }
        }
        close(mfd);
    }
}

static void dump_format(int fd, enum v4l2_buf_type type, const char *tag) {
    struct v4l2_format fmt = {0};
    fmt.type = type;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        fprintf(stderr, "[%s] VIDIOC_G_FMT(%s) 失败: %s\n", tag, buf_type_name(type), strerror(errno));
        return;
    }

    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE || type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        char fcc[5];
        fourcc_to_str(fmt.fmt.pix_mp.pixelformat, fcc);
        fprintf(stderr, "[%s] G_FMT(%s): %ux%u fourcc=%s planes=%u field=%u colorspace=%u quant=%u\n",
                tag, buf_type_name(type),
                fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, fcc, fmt.fmt.pix_mp.num_planes,
                fmt.fmt.pix_mp.field, fmt.fmt.pix_mp.colorspace, fmt.fmt.pix_mp.quantization);
        for (unsigned int i = 0; i < fmt.fmt.pix_mp.num_planes && i < VIDEO_MAX_PLANES; i++) {
            fprintf(stderr, "  - plane%u: bytesperline=%u sizeimage=%u\n",
                    i, fmt.fmt.pix_mp.plane_fmt[i].bytesperline, fmt.fmt.pix_mp.plane_fmt[i].sizeimage);
        }
        return;
    }

    if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE || type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        char fcc[5];
        fourcc_to_str(fmt.fmt.pix.pixelformat, fcc);
        fprintf(stderr, "[%s] G_FMT(%s): %ux%u fourcc=%s bytesperline=%u sizeimage=%u field=%u colorspace=%u quant=%u\n",
                tag, buf_type_name(type),
                fmt.fmt.pix.width, fmt.fmt.pix.height, fcc,
                fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage, fmt.fmt.pix.field,
                fmt.fmt.pix.colorspace, fmt.fmt.pix.quantization);
        return;
    }

    fprintf(stderr, "[%s] G_FMT(%s): (unhandled type)\n", tag, buf_type_name(type));
}

static int wait_meta_output_dqbuf(int fd, struct v4l2_buffer *buf, int timeout_ms, const char *tag) {
    struct pollfd pfd = {0};
    pfd.fd = fd;
    pfd.events = POLLOUT | POLLERR | POLLHUP;

    int waited_ms = 0;
    while (waited_ms < timeout_ms) {
        int step_ms = 50;
        int pr = poll(&pfd, 1, step_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[%s] poll(POLLOUT) 失败: %s\n", tag, strerror(errno));
            return -1;
        }
        waited_ms += step_ms;

        memset(buf, 0, sizeof(*buf));
        buf->type = V4L2_BUF_TYPE_META_OUTPUT;
        buf->memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_DQBUF, buf) == 0) return 0;
        if (errno != EAGAIN) {
            fprintf(stderr, "[%s] VIDIOC_DQBUF(META_OUTPUT) 失败: %s\n", tag, strerror(errno));
            return -1;
        }
    }

    errno = EAGAIN;
    return -1;
}

static int is_all_digits(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

static int is_likely_rkaiq_comm(const char *comm) {
    if (!comm || !comm[0]) return 0;
    if (strcmp(comm, "rkaiq_3A_server") == 0) return 1;
    if (strncmp(comm, "rkaiq", 5) == 0) return 1;
    if (strstr(comm, "aiq") != NULL) return 1;
    return 0;
}

static int print_proc_fd_holders(const char *target_path, const char *tag, int *out_seen_rkaiq) {
    DIR *proc = opendir("/proc");
    if (!proc) {
        fprintf(stderr, "[%s] opendir(/proc) 失败: %s\n", tag, strerror(errno));
        return 0;
    }

    fprintf(stderr, "[%s] 检查占用: %s\n", tag, target_path);
    int shown = 0;
    int seen_rkaiq = 0;

    struct dirent *de;
    while ((de = readdir(proc)) != NULL) {
        if (!is_all_digits(de->d_name)) continue;

        char fd_dir[PATH_MAX];
        snprintf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", de->d_name);
        DIR *fds = opendir(fd_dir);
        if (!fds) continue;

        struct dirent *fe;
        while ((fe = readdir(fds)) != NULL) {
            if (!is_all_digits(fe->d_name)) continue;

            char link_path[PATH_MAX];
            char link_target[PATH_MAX];
            snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir, fe->d_name);
            ssize_t n = readlink(link_path, link_target, (ssize_t)sizeof(link_target) - 1);
            if (n <= 0) continue;
            link_target[n] = '\0';

            if (strcmp(link_target, target_path) != 0) continue;

            char comm_path[PATH_MAX];
            snprintf(comm_path, sizeof(comm_path), "/proc/%s/comm", de->d_name);
            FILE *f = fopen(comm_path, "r");
            char comm[128] = {0};
            if (f) {
                (void)fgets(comm, sizeof(comm), f);
                fclose(f);
                size_t l = strlen(comm);
                if (l && comm[l - 1] == '\n') comm[l - 1] = '\0';
            } else {
                strncpy(comm, "?", sizeof(comm) - 1);
            }

            fprintf(stderr, "  - pid=%s comm=%s fd=%s\n", de->d_name, comm, fe->d_name);
            shown++;
            if (is_likely_rkaiq_comm(comm)) seen_rkaiq = 1;
            if (shown >= 10) break;
        }

        closedir(fds);
        if (shown >= 10) break;
    }

    closedir(proc);
    if (out_seen_rkaiq) *out_seen_rkaiq = seen_rkaiq;
    if (shown == 0) {
        /* 进一步判断是否因为 /proc hidepid 导致无法查看其他进程 fd */
        DIR *p1 = opendir("/proc/1/fd");
        if (!p1 && (errno == EACCES || errno == EPERM)) {
            fprintf(stderr, "  (未发现占用；且无权限读取 /proc/1/fd，可能启用了 hidepid，导致无法枚举其他进程 fd)\n");
        } else {
            if (p1) closedir(p1);
            fprintf(stderr, "  (未发现占用，或无法读取 /proc/*/fd)\n");
        }
    }
    return shown;
}

static void preflight_require_not_occupied(const char *path, const char *tag) {
    int seen_rkaiq = 0;
    int holders = print_proc_fd_holders(path, tag, &seen_rkaiq);
    if (holders <= 0 || g_allow_occupied) return;

    if (seen_rkaiq) {
        fprintf(stderr,
                "[%s] 发现 rkaiq/3A 代理(例如 rkaiq_3A_server)正在占用 %s。\n"
                "      本程序将尝试直接操作这条 pipeline 的 /dev/video* 节点，通常需要独占控制；\n"
                "      资源被 rkaiq 接管时，VIDIOC_STREAMON 往往会返回 EPERM(Operation not permitted)。\n"
                "      建议：先停止 rkaiq_3A_server(或相机服务)后再运行本程序；\n"
                "      如必须继续尝试，请加 --allow-occupied 或 --force。\n",
                tag, path);
    } else {
        fprintf(stderr,
                "[%s] %s 已被其他进程占用。建议先停止占用者后再运行；\n"
                "      如必须继续尝试，请加 --allow-occupied 或 --force。\n",
                tag, path);
    }
    exit(1);
}

static int wait_capture_dqbuf(int fd,
                              struct v4l2_buffer *buf,
                              struct v4l2_plane *planes,
                              unsigned int num_planes,
                              int timeout_ms,
                              const char *tag) {
    struct pollfd pfd = {0};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;

    int waited_ms = 0;
    while (waited_ms < timeout_ms) {
        int step_ms = 50;
        int pr = poll(&pfd, 1, step_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[%s] poll(POLLIN) 失败: %s\n", tag, strerror(errno));
            return -1;
        }
        waited_ms += step_ms;

        memset(buf, 0, sizeof(*buf));
        buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf->memory = V4L2_MEMORY_MMAP;
        buf->m.planes = planes;
        buf->length = num_planes;
        if (xioctl(fd, VIDIOC_DQBUF, buf) == 0) return 0;
        if (errno != EAGAIN) {
            fprintf(stderr, "[%s] VIDIOC_DQBUF(CAPTURE) 失败: %s\n", tag, strerror(errno));
            return -1;
        }
    }

    errno = EAGAIN;
    return -1;
}

static int wait_output_dqbuf(int fd,
                             struct v4l2_buffer *buf,
                             struct v4l2_plane *planes,
                             unsigned int num_planes,
                             int timeout_ms,
                             const char *tag) {
    struct pollfd pfd = {0};
    pfd.fd = fd;
    pfd.events = POLLOUT | POLLERR | POLLHUP;

    int waited_ms = 0;
    while (waited_ms < timeout_ms) {
        int step_ms = 50;
        int pr = poll(&pfd, 1, step_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[%s] poll(POLLOUT) 失败: %s\n", tag, strerror(errno));
            return -1;
        }
        waited_ms += step_ms;

        memset(buf, 0, sizeof(*buf));
        buf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf->memory = V4L2_MEMORY_DMABUF;
        buf->m.planes = planes;
        buf->length = num_planes;
        if (xioctl(fd, VIDIOC_DQBUF, buf) == 0) return 0;
        if (errno != EAGAIN) {
            fprintf(stderr, "[%s] VIDIOC_DQBUF(OUTPUT) 失败: %s\n", tag, strerror(errno));
            return -1;
        }
    }

    errno = EAGAIN;
    return -1;
}

/*
 * open_dev: 打开一个 /dev/videoN 设备节点。
 *
 * O_RDWR     - V4L2 设备必须以读写方式打开,即使你只打算采集(读),因为
 *              建流过程中需要通过 ioctl 双向通信(内核既要读你下发的配置,
 *              也要把状态写回给你)。
 * O_NONBLOCK - 非阻塞模式打开。这样 VIDIOC_DQBUF 在没有帧就绪时会立刻返回
 *              -1(errno=EAGAIN),而不是把整个进程阻塞住,方便我们自己用
 *              轮询+sleep 的方式等待,便于观察到底卡在哪一步。
 */
static int open_dev(const char *path) {
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "打开 %s 失败: %s\n", path, strerror(errno));
        exit(1);
    }
    return fd;
}

/* =========================================================================
 * 第1步: video0 采集一帧 raw,并导出为 DMA-BUF fd
 * ========================================================================= */
static int capture_raw_and_export(int *out_dmabuf_fd, size_t *out_len) {
    preflight_require_not_occupied(g_video0_raw, "video0");
    int fd = open_dev(g_video0_raw);
    dump_querycap(fd, "video0");
    dump_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "video0(before S_FMT)");

    /*
     * struct v4l2_format + VIDIOC_S_FMT:告诉内核"接下来我要用这个格式采集"。
     *
     * .type            - V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 表示这是一个
     *                     "多平面(multi-planar)采集"设备,对应我们在
     *                     v4l2-ctl --all 里看到的 "Video Capture Multiplanar"
     *                     能力位。RAW Bayer 虽然只有1个plane,但这颗驱动
     *                     统一走多平面API。
     * .fmt.pix_mp.width/height - 采集分辨率,必须落在设备支持的
     *                     stepwise范围内(前面 --list-formats-ext 查到的
     *                     64x64 ~ 3864x2192,步进8)。
     * .fmt.pix_mp.pixelformat  - 用 v4l2_fourcc() 宏拼出四字符编码,
     *                     'G','B','1','0' 对应 fourcc "GB10",就是我们前面
     *                     confirm 过的这颗imx415实际输出的Bayer排列。
     * .fmt.pix_mp.field        - V4L2_FIELD_NONE 表示逐行扫描(非隔行),
     *                     摄像头传感器固定是这个值。
     * .fmt.pix_mp.num_planes   - 声明用1个plane装数据。
     */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = RAW_W;
    fmt.fmt.pix_mp.height = RAW_H;
    fmt.fmt.pix_mp.pixelformat = v4l2_fourcc('G', 'B', '1', '0'); /* GB10 */
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "video0 S_FMT 失败: %s\n", strerror(errno));
        exit(1);
    }
    dump_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "video0(after S_FMT)");

    /*
     * struct v4l2_requestbuffers + VIDIOC_REQBUFS:向内核申请一批采集缓冲区。
     *
     * .count  - 申请1个buffer。真实连续采集通常要3~6个做环形缓冲避免丢帧,
     *           这里只测单帧,1个够用。
     * .type   - 必须和上面 S_FMT 用的type一致。
     * .memory - V4L2_MEMORY_MMAP 表示"缓冲区的物理内存由内核分配,用户态
     *           通过mmap()把它映射到自己的地址空间来读写",这是最常见的
     *           零拷贝采集方式(相对于 V4L2_MEMORY_USERPTR 用户自己分配内存
     *           再交给内核,或 V4L2_MEMORY_DMABUF 导入别的设备导出的buffer)。
     */
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "video0 REQBUFS 失败: %s\n", strerror(errno));
        exit(1);
    }

    /*
     * struct v4l2_buffer 描述"一个具体的buffer"。多平面模式下,每个buffer
     * 可能包含多个plane(比如YUV420分Y/UV两个plane),所以需要额外的
     * struct v4l2_plane 数组通过 buf.m.planes 挂进去,这里只有1个plane。
     *
     * VIDIOC_QUERYBUF:查询index=0这个buffer的详细信息(比如它在内核里的
     * 长度、mmap偏移量等),这里我们其实只是为了拿到 planes[0] 的元信息,
     * 采集完之后马上就要用 EXPBUF 导出,不需要真的mmap读数据。
     */
    struct v4l2_plane planes[1] = {0};
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;              /* 操作REQBUFS申请到的第0个buffer */
    buf.m.planes = planes;      /* 多平面模式必须指向一个plane数组 */
    buf.length = 1;             /* 数组长度,即plane数量 */
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "video0 QUERYBUF 失败: %s\n", strerror(errno));
        exit(1);
    }

    /*
     * VIDIOC_QBUF:把这个空buffer"交还"给内核的采集队列,告诉驱动
     * "这块内存你可以用来装下一帧数据"。QBUF之后这个buffer的所有权
     * 从用户态转移到内核态,直到你后面DQBUF把它取回来为止。
     */
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "video0 QBUF 失败: %s\n", strerror(errno));
        exit(1);
    }

    /*
     * VIDIOC_STREAMON:正式启动采集流。在这之前硬件(CIF)是不会真正
     * 触发DMA写数据的,STREAMON之后驱动才会使能对应的中断/DMA通路,
     * 传感器数据开始真正往QBUF进去的那块内存里写。
     */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "video0 STREAMON 失败: %s\n", strerror(errno));
        exit(1);
    }

    /*
     * VIDIOC_DQBUF:从内核的"已完成"队列里取回一个buffer,只有当硬件
     * 真的把一帧数据写满了这块内存,内核才会把它标记为可DQBUF。因为
     * open时用了O_NONBLOCK,数据没准备好时DQBUF会立刻返回EAGAIN,
     * 所以这里用一个简单的轮询+sleep(1ms)循环等待,最多等2秒
     * (2000次×1ms)。生产代码应该用 select()/poll() 在fd上等可读事件,
     * 而不是轮询,这里为了代码直观简化处理。
     */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    int tries = 0;
    while (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN || ++tries > 2000) {
            fprintf(stderr, "video0 DQBUF 失败: %s\n", strerror(errno));
            exit(1);
        }
        usleep(1000);
    }
    /* DQBUF成功后,planes[0].bytesused 会被内核填成这一帧实际写入的字节数 */

    /*
     * VIDIOC_EXPBUF:这是整个桥接方案的核心机制——把index=0这个buffer
     * (刚刚被硬件填满数据的那块内核物理内存)导出成一个 DMA-BUF 文件
     * 描述符(dma_buf fd)。DMA-BUF 是 Linux 内核的跨设备内存共享机制:
     * 拿到这个fd之后,任何支持DMA-BUF导入的设备(比如video26)都可以
     * 直接引用同一块物理内存,不需要CPU把数据从A设备的内存拷贝到
     * B设备的内存——这就是"零拷贝"的含义,对4K raw这种大数据量帧
     * 尤其关键,拷贝一帧的CPU开销和带宽消耗都不小。
     *
     * .type  - 依然是采集类型,表示要导出的是video0的采集buffer
     * .index - 要导出哪个buffer(第0个)
     * .plane - 多平面buffer里导出哪个plane(第0个plane)
     * 调用成功后 expbuf.fd 就是这个新的dma-buf文件描述符。
     */
    struct v4l2_exportbuffer expbuf = {0};
    expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    expbuf.index = buf.index;
    expbuf.plane = 0;
    if (xioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        fprintf(stderr, "video0 EXPBUF 失败: %s\n", strerror(errno));
        exit(1);
    }

    fprintf(stderr, "[video0] EXPBUF: dmabuf_fd=%d bytesused=%u\n", expbuf.fd, planes[0].bytesused);

    *out_dmabuf_fd = expbuf.fd;
    *out_len = planes[0].bytesused;  /* 实际有效数据长度,后面QBUF给video26时要用 */

    /*
     * 注意:这里故意不 close(fd)、不 STREAMOFF。因为导出的 dma-buf fd
     * 引用的是video0这个采集队列里的物理内存,如果现在就关闭/停流,
     * 底层驱动可能提前回收这块内存,导致video26读到的是脏数据甚至
     * 触发内核警告。生产代码需要更严谨的生命周期管理(比如引用计数、
     * 显式在video26消费完之后才STREAMOFF+close video0),这里为了让
     * 单帧演示流程保持简单,直接让fd在main()退出进程结束时被系统回收。
     */
    return fd;
}

/* =========================================================================
 * 第2步: video30 写入一份全零参数(仅打通流程,不做真3A)
 * ========================================================================= */
static void feed_zero_params(void) {
    preflight_require_not_occupied(g_video30_params, "video30");
    int fd = open_dev(g_video30_params);
    dump_querycap(fd, "video30");

    /*
     * VIDIOC_G_FMT (Get Format):向内核"查询"这个设备当前期望的数据格式,
     * 而不是S_FMT那样去"设置"。之所以用G_FMT而不是直接写死一个buffer大小,
     * 是因为input-params这类"元数据(metadata)"节点的buffer大小取决于
     * 具体ISP硬件版本支持的参数项数量,不同内核版本/BSP可能不一样,
     * 用G_FMT问驱动"你需要多大的buffer"比自己猜更可靠。
     *
     * .type = V4L2_BUF_TYPE_META_OUTPUT:表示这是一个"元数据输出"类型的
     * 节点——"元数据"指的不是图像像素,而是控制/配置类的结构化数据;
     * "输出(OUTPUT)"指的是数据流向是"用户态写入、设备消费",和图像采集
     * 的"采集(CAPTURE)"方向相反。rkisp1系列驱动里,3A参数下发节点
     * 通常就是这个类型,但如果你的内核版本给它定义成别的buf type,
     * 这里会拿到报错,提示你需要按实际情况调整。
     */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_META_OUTPUT;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        fprintf(stderr, "video30 G_FMT 失败(可能buf type不是META_OUTPUT,"
                "需要按实际驱动调整): %s\n", strerror(errno));
        close(fd);
        return; /* 不中断整个流程,允许你先跳过参数环节排查其余部分 */
    }
    fprintf(stderr, "[video30] G_FMT(META_OUTPUT): buffersize=%u\n", fmt.fmt.meta.buffersize);

    /* 同上面video0的REQBUFS,只是type换成META_OUTPUT */
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_META_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "video30 REQBUFS 失败: %s\n", strerror(errno));
        close(fd);
        return;
    }
    fprintf(stderr, "[video30] REQBUFS(MMAP) count=%u\n", req.count);

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_META_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "video30 QUERYBUF 失败: %s\n", strerror(errno));
        close(fd);
        return;
    }
    fprintf(stderr, "[video30] QUERYBUF length=%u offset=%u\n", buf.length, buf.m.offset);

    /*
     * mmap():把内核里这块buffer的物理内存映射到当前进程的虚拟地址空间,
     * 之后就可以像操作普通内存一样通过指针读写它。
     *
     * PROT_READ|PROT_WRITE - 既要写入参数(WRITE),某些驱动实现也允许
     *                         读回当前生效值(READ),所以两个权限都要。
     * MAP_SHARED             - 修改要真正同步回内核那块物理内存(而不是
     *                         写时拷贝出一份进程私有副本),这样内核才能
     *                         看到我们写入的数据。
     * buf.m.offset            - QUERYBUF返回的"这块buffer在设备文件里的
     *                         偏移量",mmap的第6个参数(offset)必须传
     *                         这个值,内核靠它找到对应的物理页。
     */
    void *params = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, buf.m.offset);
    if (params == MAP_FAILED) {
        fprintf(stderr, "video30 mmap 失败: %s\n", strerror(errno));
        close(fd);
        return;
    }
    /*
     * memset全零的含义:rkisp1参数结构体里,每个处理模块(去噪、锐化、
     * 白平衡矩阵、伽马校正...)通常都有一个"enable"标志位,全零意味着
     * 这些标志位大多是0(禁用),ISP只做最基础的Bayer->YUV转换(demosaic
     * +色彩空间转换),不叠加任何调优,所以画面大概率偏色/欠曝——这是
     * 预期的,目的只是验证数据能不能端到端流过去。
     */
    memset(params, 0, buf.length);

    buf.bytesused = buf.length;  /* 告诉内核这次实际写入了多少字节 */
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "video30 QBUF 失败: %s\n", strerror(errno));
        munmap(params, buf.length);
        close(fd);
        return;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_META_OUTPUT;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "video30 STREAMON 失败: %s\n", strerror(errno));
    }

    /* 观察参数 buffer 是否被 ISP 消费（能否 DQBUF 回来） */
    struct v4l2_buffer dq = {0};
    if (wait_meta_output_dqbuf(fd, &dq, 2000, "video30") == 0) {
        fprintf(stderr, "[video30] DQBUF(META_OUTPUT) OK: idx=%u bytesused=%u flags=0x%08x\n",
                dq.index, dq.bytesused, dq.flags);
        /* 尽量不改变原有行为：立刻再 QBUF 回去，确保参数节点持续有 buffer 可用 */
        dq.bytesused = buf.length;
        if (xioctl(fd, VIDIOC_QBUF, &dq) < 0) {
            fprintf(stderr, "[video30] QBUF(again) 失败: %s\n", strerror(errno));
        } else {
            fprintf(stderr, "[video30] QBUF(again) OK\n");
        }
    } else {
        fprintf(stderr, "[video30] DQBUF(META_OUTPUT) 超时/失败: %s\n", strerror(errno));
    }
    /* 同样不提前munmap/close,让这份参数在ISP处理期间保持有效 */
}

/* =========================================================================
 * 第3步: video26 用 DMA-BUF 方式把 raw 帧喂给 ISP
 * ========================================================================= */
static void feed_raw_to_isp(int dmabuf_fd, size_t len) {
    preflight_require_not_occupied(g_video26_rawwr, "video26");
    int fd = open_dev(g_video26_rawwr);
    dump_querycap(fd, "video26");
    dump_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "video26(initial)");

    /* 对当前 fd 显式做一次 S_FMT，避免仅靠外部 v4l2-ctl 预设导致“格式没生效/按 fd 保存” */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = RAW_W;
    fmt.fmt.pix_mp.height = RAW_H;
    fmt.fmt.pix_mp.pixelformat = v4l2_fourcc('G', 'B', '1', '0'); /* GB10 */
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    /* 10-bit packed: bytes_per_line ~= align_up(width * 10 / 8, 64) */
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = (((RAW_W * 10U + 7U) / 8U) + 63U) & ~63U;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (unsigned int)len;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "video26 S_FMT 失败: %s\n", strerror(errno));
        dump_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "video26(on S_FMT fail)");
        exit(1);
    }
    dump_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "video26(after S_FMT)");

    /* 基本一致性检查：如果 sizeimage 仍远小于输入长度，后续大概率卡在不消费 */
    struct v4l2_format got = {0};
    got.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_G_FMT, &got) == 0) {
        if (got.fmt.pix_mp.num_planes < 1) {
            fprintf(stderr, "[video26] 错误: num_planes=%u\n", got.fmt.pix_mp.num_planes);
            exit(1);
        }
        if (got.fmt.pix_mp.plane_fmt[0].sizeimage &&
            got.fmt.pix_mp.plane_fmt[0].sizeimage < len) {
            fprintf(stderr, "[video26] 错误: sizeimage=%u < input_len=%zu (video26 格式/stride 很可能没生效)\n",
                    got.fmt.pix_mp.plane_fmt[0].sizeimage, len);
            exit(1);
        }
    }

    /*
     * 这里没有调用S_FMT——因为格式已经在前面用
     *   (旧版说明: 曾依赖外部 v4l2-ctl 预设；当前版本已在代码里对 /dev/video26 显式 VIDIOC_S_FMT)
     * 设置过了,这里复用那次协商好的格式,只负责建流。
     *
     * .type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:注意是OUTPUT(输出)
     *           不是CAPTURE(采集)——数据流向是"用户态/上一个设备的数据
     *           流入这个节点",对应拓扑图里rawrd0_m的pad方向是Source
     *           (向isp-subdev发送数据),从这个video节点自己的角度看,
     *           它是在"消费"用户态灌进来的数据,所以buf type是OUTPUT。
     * .memory = V4L2_MEMORY_DMABUF:表示这次的buffer不是让内核自己分配
     *           (MMAP)或用户态malloc一块内存(USERPTR),而是直接导入
     *           一个外部fd(也就是第1步从video0导出的那个dma-buf)。
     */
    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "video26 REQBUFS(DMABUF) 失败: %s\n", strerror(errno));
        exit(1);
    }
    fprintf(stderr, "[video26] REQBUFS(DMABUF) count=%u\n", req.count);

    /*
     * 组装要QBUF的buffer描述:
     * planes[0].m.fd       - 关键字段,填入第1步导出的dma-buf fd,这就是
     *                        "导入"这块外部内存的方式,内核会通过这个fd
     *                        找到video0导出的那块物理内存并直接复用,
     *                        不发生数据拷贝。
     * planes[0].length     - 这个plane对应的buffer总容量。
     * planes[0].bytesused  - 这个plane里实际有效的数据长度,这里两者
     *                        都填raw帧的实际字节数(EXPBUF时video0返回
     *                        的bytesused)。
     */
    struct v4l2_plane planes[1] = {0};
    planes[0].m.fd = dmabuf_fd;
    planes[0].length = len;
    planes[0].bytesused = len;

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 0;
    buf.m.planes = planes;
    buf.length = 1;
    /*
     * VIDIOC_QBUF在这里的含义和video0那次不同:那次是"交一个空buffer
     * 等硬件填数据",这次是"把一个已经装满数据的buffer交给ISP去读取
     * 消费"。方向相反,但API调用形式是一样的,靠buf.type里的
     * OUTPUT/CAPTURE来区分语义。
     */
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "video26 QBUF(DMABUF导入) 失败: %s\n", strerror(errno));
        exit(1);
    }
    fprintf(stderr, "[video26] QBUF: dmabuf_fd=%d bytesused=%u length=%u\n",
            dmabuf_fd, planes[0].bytesused, planes[0].length);

    /* STREAMON之后,ISP硬件才会真正开始从这块dma-buf里读取raw数据处理 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "video26 STREAMON 失败: %s\n", strerror(errno));
        exit(1);
    }
    dump_format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "video26(after STREAMON)");

    /* 尝试观察 ISP 是否真正消费了这帧(OUTPUT 队列能否 DQBUF 回来) */
    struct v4l2_plane dq_planes[1] = {0};
    struct v4l2_buffer dq = {0};
    if (wait_output_dqbuf(fd, &dq, dq_planes, 1, 5000, "video26") == 0) {
        fprintf(stderr, "[video26] DQBUF(OUTPUT) OK: idx=%u bytesused=%u flags=0x%08x\n",
                dq.index, dq_planes[0].bytesused, dq.flags);
    } else {
        fprintf(stderr, "[video26] DQBUF(OUTPUT) 超时/失败: %s\n", strerror(errno));
    }
}

/* =========================================================================
 * 第4步: video22 读一帧 NV12 并存盘
 * ========================================================================= */
static void capture_yuv_and_save(const char *out_path, int lenient_fmt) {
    preflight_require_not_occupied(g_video22_yuv, "video22");
    int fd = open_dev(g_video22_yuv);
    dump_querycap(fd, "video22");
    dump_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "video22(before S_FMT)");

    /*
     * 这个S_FMT和第一步video0的S_FMT结构类似,区别在于:
     * .pixelformat = V4L2_PIX_FMT_NV12:请求YUV420半平面格式(一个Y平面+
     *                一个UV交织平面),这是ISP subdev pad2那端已经协商好
     *                能输出YUV8_2X8之后,mainpath这个节点自己再做的
     *                "从内部YUV中间格式转换/打包成NV12"这一步,是
     *                mainpath自己支持的能力,不需要再手动设isp-subdev。
     */
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = YUV_W;
    fmt.fmt.pix_mp.height = YUV_H;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        if (lenient_fmt) {
            fprintf(stderr, "[video22] S_FMT 失败(%s)，rkaiq 模式下沿用驱动当前格式继续...\n",
                    strerror(errno));
            struct v4l2_format cur = {0};
            cur.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (xioctl(fd, VIDIOC_G_FMT, &cur) < 0) {
                fprintf(stderr, "video22 G_FMT 失败: %s\n", strerror(errno));
                exit(1);
            }
            dump_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "video22(using current)");
        } else {
            fprintf(stderr, "video22 S_FMT 失败: %s\n", strerror(errno));
            exit(1);
        }
    } else {
        dump_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "video22(after S_FMT)");
    }

    struct v4l2_requestbuffers req = {0};
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "video22 REQBUFS 失败: %s\n", strerror(errno));
        exit(1);
    }
    fprintf(stderr, "[video22] REQBUFS(MMAP) count=%u\n", req.count);

    struct v4l2_plane planes[1] = {0};
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = planes;
    buf.length = 1;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        fprintf(stderr, "video22 QUERYBUF 失败: %s\n", strerror(errno));
        exit(1);
    }
    fprintf(stderr, "[video22] QUERYBUF plane0.length=%u mem_offset=%u\n",
            planes[0].length, planes[0].m.mem_offset);

    /*
     * 这次和video0不同,我们要真正mmap这块内存来读取里面的图像数据
     * (而不是导出给别人),所以用 PROT_READ(只读即可,我们不修改
     * 图像内容)。planes[0].m.mem_offset是QUERYBUF返回的、这个plane
     * 在设备文件里对应的偏移量。
     */
    void *ymem = mmap(NULL, planes[0].length, PROT_READ, MAP_SHARED,
                       fd, planes[0].m.mem_offset);
    if (ymem == MAP_FAILED) {
        fprintf(stderr, "video22 mmap 失败: %s\n", strerror(errno));
        exit(1);
    }

    /* 采集类节点的常规流程:先QBUF交一个空buffer,再STREAMON,再DQBUF取回 */
    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "video22 QBUF 失败: %s\n", strerror(errno));
        exit(1);
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "video22 STREAMON 失败: %s\n", strerror(errno));
        exit(1);
    }

    /*
     * 这里的DQBUF能不能成功,取决于前面第2、3步是否真的让ISP跑起来了
     * ——如果video26那边的数据没有被isp-subdev正确消费处理,这里会
     * 一直超时拿不到帧,这也是判断"链路是否真正打通"最直接的信号。
     */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;
    if (wait_capture_dqbuf(fd, &buf, planes, 1, 5000, "video22") < 0) {
        fprintf(stderr, "video22 DQBUF 失败(可能上游没有真正送数据过来): %s\n", strerror(errno));
        if (lenient_fmt) {
            fprintf(stderr,
                    "      rkaiq 模式排查: 确认 rkaiq_3A_server 正在运行且传感器已出流;\n"
                    "      可先用 v4l2-ctl -d %s --stream-mmap --stream-count=1 验证该节点本身能否取帧。\n",
                    g_video22_yuv);
        }
        dump_format(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "video22(on DQBUF fail)");
        exit(1);
    }
    fprintf(stderr, "[video22] DQBUF OK: bytesused=%u flags=0x%08x\n", planes[0].bytesused, buf.flags);

    /* planes[0].bytesused 是内核告诉我们这一帧实际写入了多少字节,
     * 直接从mmap映射的地址ymem里按这个长度写文件即可 */
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "打开输出文件失败: %s\n", strerror(errno));
        exit(1);
    }
    fwrite(ymem, 1, planes[0].bytesused, f);
    fclose(f);

    printf("已保存一帧 NV12 到 %s,大小 %u 字节\n", out_path, planes[0].bytesused);
}

int main(int argc, char **argv) {
    int dmabuf_fd;
    size_t raw_len;
    int skip_params = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--allow-occupied") == 0 || strcmp(argv[i], "--force") == 0) {
            g_allow_occupied = 1;
            continue;
        }
        if (strcmp(argv[i], "--via-rkaiq") == 0 || strcmp(argv[i], "--rkaiq") == 0) {
            g_via_rkaiq = 1;
            continue;
        }
        if (strcmp(argv[i], "--skip-params") == 0 || strcmp(argv[i], "--no-params") == 0) {
            skip_params = 1;
            continue;
        }
        if (strcmp(argv[i], "--video0") == 0 && i + 1 < argc) {
            g_video0_raw = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--video26") == 0 && i + 1 < argc) {
            g_video26_rawwr = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--video30") == 0 && i + 1 < argc) {
            g_video30_params = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--video22") == 0 && i + 1 < argc) {
            g_video22_yuv = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [--skip-params|--no-params]\n", argv[0]);
            printf("  --skip-params/--no-params: 不操作 /dev/video30, 由外部(rkaiq/应用)负责下发 first IQ/params\n");
            printf("  --allow-occupied/--force : 跳过占用检查(不推荐;可能 EPERM/EBUSY)\n");
            printf("  --via-rkaiq/--rkaiq      : 通过 rkaiq 取帧:不操作 video0/26/30,只从 mainpath(--video22)取一帧\n");
            printf("  --video0  <path>: 指定 raw 采集节点(默认 %s)\n", VIDEO0_RAW);
            printf("  --video26 <path>: 指定 raw 注入节点(默认 %s)\n", VIDEO26_RAWWR);
            printf("  --video30 <path>: 指定 params 节点(默认 %s)\n", VIDEO30_PARAMS);
            printf("  --video22 <path>: 指定 yuv 输出节点(默认 %s)\n", VIDEO22_YUV);
            return 0;
        }
        fprintf(stderr, "未知参数: %s (使用 --help 查看)\n", argv[i]);
        return 2;
    }

    fprintf(stderr, "[dev] video0=%s video26=%s video30=%s video22=%s\n",
            g_video0_raw, g_video26_rawwr, g_video30_params, g_video22_yuv);

    if (g_via_rkaiq) {
        if (skip_params ||
            strcmp(g_video0_raw, VIDEO0_RAW) != 0 ||
            strcmp(g_video26_rawwr, VIDEO26_RAWWR) != 0 ||
            strcmp(g_video30_params, VIDEO30_PARAMS) != 0) {
            fprintf(stderr, "[via-rkaiq] 提示: 该模式只使用 mainpath 节点(--video22)，已忽略 --video0/--video26/--video30/--skip-params。\n");
        }
        fprintf(stderr, "[dev] rkaiq 模式: mainpath=%s (video0/26/30 由 rkaiq_3A_server 接管,本程序不操作)\n",
                g_video22_yuv);
        printf("[via-rkaiq] 从 mainpath 取一帧 NV12(rkaiq 负责 3A/ISP 调参)...\n");
        capture_yuv_and_save("/tmp/isp_frame_via_rkaiq.nv12", 1);
        printf("流程走完。帧已保存: /tmp/isp_frame_via_rkaiq.nv12\n");
        return 0;
    }

    printf("[1/4] 从 video0 采集一帧 raw 并导出 DMA-BUF...\n");
    capture_raw_and_export(&dmabuf_fd, &raw_len);
    printf("      raw 帧大小: %zu 字节, dmabuf fd=%d\n", raw_len, dmabuf_fd);

    if (!skip_params) {
        printf("[2/4] 向 video30 写入全零参数(仅打通流程)...\n");
        feed_zero_params();
    } else {
        printf("[2/4] 跳过 video30 参数下发(--skip-params)...\n");
        printf("      注意: rkisp 通常仍需要外部(rkaiq/应用)在 stream on 前完成 first IQ/params 初始化。\n");
    }

    printf("[3/4] 把 raw 帧通过 DMA-BUF 喂给 video26...\n");
    feed_raw_to_isp(dmabuf_fd, raw_len);

    printf("[4/4] 从 video22 读取ISP处理后的一帧并保存...\n");
    capture_yuv_and_save("/tmp/isp_test_frame.nv12", 0);

    printf("流程走完。若失败,请把具体哪一步的报错信息发回来定位问题。\n");
    return 0;
}
