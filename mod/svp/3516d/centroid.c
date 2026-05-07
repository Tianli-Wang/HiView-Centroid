#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "cfg.h"
#include "centroid_sample.h"
#include "mpi_ae.h"

#define CENTROID_UART_DEFAULT "/dev/ttyAMA0"
#define CENTROID_BAUD_DEFAULT 115200
#define CENTROID_THRESHOLD_DEFAULT 220
#define CENTROID_MIN_AREA_DEFAULT 4
#define CENTROID_WINDOW_DEFAULT 64
#define CENTROID_WINDOW_MIN 8
#define CENTROID_WINDOW_MAX 512
#define CENTROID_CLARITY_WINDOW_W_DEFAULT 360
#define CENTROID_CLARITY_WINDOW_H_DEFAULT 150
#define CENTROID_CLARITY_WINDOW_W_MIN 180
#define CENTROID_CLARITY_WINDOW_H_MIN 90
#define CENTROID_CLARITY_WINDOW_W_MAX 960
#define CENTROID_CLARITY_WINDOW_H_MAX 540
#define CENTROID_INTERVAL_DEFAULT 100
#define CENTROID_AE_SATURATION_DEFAULT 255
#define CENTROID_AE_MIN_EXP_DEFAULT 100
#define CENTROID_AE_MAX_EXP_DEFAULT 33333
#define CENTROID_AE_GAIN_DEFAULT 1024
#define CENTROID_AE_GAIN_MAX 262144
#define CENTROID_AE_TARGET_RATIO_DEFAULT 0.8
#define CENTROID_AE_TARGET_RATIO_MIN 0.1
#define CENTROID_AE_TARGET_RATIO_MAX 0.95
#define CENTROID_AE_TOLERANCE_RATIO 0.03

typedef struct centroid_ae_cfg_s
{
  int enable;
  int pipe;
  int saturation;
  double target_ratio;
  int min_exp_us;
  int max_exp_us;
  int init_exp_us;
  int manual_gain;
} centroid_ae_cfg_t;

typedef struct centroid_ae_state_s
{
  int enabled;
  int configured;
  int pipe;
  int saturation;
  double target_ratio;
  int min_exp_us;
  int max_exp_us;
  int init_exp_us;
  int manual_gain;
  int exp_us;
  int target_exp_us;
  int target_gray;
  int adjust;
  int last_ret;
  int has_origin_attr;
  int origin_pipe;
  ISP_EXPOSURE_ATTR_S origin_attr;
} centroid_ae_state_t;

/* 质心算法运行在线程中，生命周期由 svp.c 根据 svp_parm.centroid.centroid_alg 控制。
 * 质心提取相关配置统一放在 GSF_ID_SVP_CENTROID_CFG 中；配置文件或消息接口改变
 * 算法开关后，主循环负责启动/停止，不需要重启整个 svp.exe。
 */
static pthread_t s_centroid_thread = 0;
static volatile int s_centroid_stop = 0;
static volatile int s_centroid_alive = 0;
static pthread_mutex_t s_centroid_status_lock = PTHREAD_MUTEX_INITIALIZER;
static gsf_svp_centroid_status_t s_centroid_status = {0};

static void* centroid_task(void* p);

static unsigned long long centroid_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static void centroid_status_reset(int running)
{
  pthread_mutex_lock(&s_centroid_status_lock);
  memset(&s_centroid_status, 0, sizeof(s_centroid_status));
  s_centroid_status.running = running;
  s_centroid_status.update_ms = centroid_now_ms();
  pthread_mutex_unlock(&s_centroid_status_lock);
}

static double centroid_positive_or_default(double value, double default_value)
{
  return value > 0.0 ? value : default_value;
}

static int centroid_clamp_int(int value, int min_value, int max_value)
{
  if(value < min_value)
  {
    return min_value;
  }
  if(value > max_value)
  {
    return max_value;
  }
  return value;
}

static double centroid_clamp_double(double value, double min_value, double max_value)
{
  if(value < min_value)
  {
    return min_value;
  }
  if(value > max_value)
  {
    return max_value;
  }
  return value;
}

static void centroid_ae_cfg_load(centroid_ae_cfg_t *cfg)
{
  if(!cfg)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));

  /* 质心线程每处理一帧都会重新读取 svp_parm.centroid。
   * 注意这里沿用 web 配置字段名 ae_enable，但它表示“是否使用 ISP 自动曝光”：
   * - ae_enable=0：关闭 ISP 自动曝光/自动增益，由本线程用 max_gray 闭环调手动曝光；
   * - ae_enable=1：不接管曝光，恢复接管前的 ISP 曝光配置。
   * 因此内部 cfg->enable 表示“是否启用本线程的手动曝光闭环”，和配置字段取反。
   */
  cfg->enable = svp_parm.centroid.ae_enable ? 0 : 1;
  cfg->pipe = svp_parm.centroid.ae_isp_pipe >= 0 ?
              svp_parm.centroid.ae_isp_pipe : 0;
  cfg->saturation = centroid_clamp_int(svp_parm.centroid.ae_saturation,
                                       1, CENTROID_AE_SATURATION_DEFAULT);
  cfg->target_ratio = svp_parm.centroid.ae_target_ratio > 0.0 ?
                      svp_parm.centroid.ae_target_ratio :
                      CENTROID_AE_TARGET_RATIO_DEFAULT;
  cfg->target_ratio = centroid_clamp_double(cfg->target_ratio,
                                            CENTROID_AE_TARGET_RATIO_MIN,
                                            CENTROID_AE_TARGET_RATIO_MAX);
  cfg->min_exp_us = svp_parm.centroid.ae_min_exp_us > 0 ?
                    svp_parm.centroid.ae_min_exp_us : CENTROID_AE_MIN_EXP_DEFAULT;
  cfg->max_exp_us = svp_parm.centroid.ae_max_exp_us > 0 ?
                    svp_parm.centroid.ae_max_exp_us : CENTROID_AE_MAX_EXP_DEFAULT;
  if(cfg->max_exp_us < cfg->min_exp_us)
  {
    cfg->max_exp_us = cfg->min_exp_us;
  }
  cfg->init_exp_us = svp_parm.centroid.ae_init_exp_us > 0 ?
                     svp_parm.centroid.ae_init_exp_us : 0;
  if(cfg->init_exp_us > 0)
  {
    cfg->init_exp_us = centroid_clamp_int(cfg->init_exp_us,
                                          cfg->min_exp_us,
                                          cfg->max_exp_us);
  }

  /* HiSilicon 曝光接口的增益是 22.10 定点数，1024 表示 1 倍。
   * 本功能只自动调曝光时间，AGain/DGain/ISPDGain 全部固定为同一个手动增益，
   * 这样就真正关闭了 ISP 自动增益，亮度变化只来自曝光时间的二倍/二分调节。
   */
  cfg->manual_gain = centroid_clamp_int(svp_parm.centroid.ae_manual_gain,
                                        CENTROID_AE_GAIN_DEFAULT,
                                        CENTROID_AE_GAIN_MAX);
}

static int centroid_ae_cfg_changed(const centroid_ae_state_t *state,
                                   const centroid_ae_cfg_t *cfg)
{
  if(!state || !cfg || !state->configured)
  {
    return 1;
  }

  return state->pipe != cfg->pipe ||
         state->saturation != cfg->saturation ||
         state->target_ratio != cfg->target_ratio ||
         state->min_exp_us != cfg->min_exp_us ||
         state->max_exp_us != cfg->max_exp_us ||
         state->init_exp_us != cfg->init_exp_us ||
         state->manual_gain != cfg->manual_gain;
}

static int centroid_ae_query_current_exp(const centroid_ae_cfg_t *cfg,
                                         const ISP_EXPOSURE_ATTR_S *attr)
{
  ISP_EXP_INFO_S info;
  int exp_us = cfg ? cfg->init_exp_us : 0;

  if(!cfg)
  {
    return CENTROID_AE_MIN_EXP_DEFAULT;
  }

  if(exp_us <= 0)
  {
    memset(&info, 0, sizeof(info));
    if(HI_MPI_ISP_QueryExposureInfo((VI_PIPE)cfg->pipe, &info) == 0 &&
       info.u32ExpTime > 0)
    {
      exp_us = (int)info.u32ExpTime;
    }
  }

  if(exp_us <= 0 && attr && attr->stManual.u32ExpTime > 0)
  {
    exp_us = (int)attr->stManual.u32ExpTime;
  }

  if(exp_us <= 0)
  {
    exp_us = cfg->min_exp_us;
  }

  return centroid_clamp_int(exp_us, cfg->min_exp_us, cfg->max_exp_us);
}

static int centroid_ae_apply_manual(centroid_ae_state_t *state,
                                    int pipe,
                                    int exp_us,
                                    int manual_gain)
{
  ISP_EXPOSURE_ATTR_S attr;
  int ret = 0;

  if(!state)
  {
    return -1;
  }

  memset(&attr, 0, sizeof(attr));
  ret = HI_MPI_ISP_GetExposureAttr((VI_PIPE)pipe, &attr);
  if(ret != 0)
  {
    state->last_ret = ret;
    return ret;
  }

  /* 关闭 ISP 内置自动曝光，并把曝光时间、模拟增益、数字增益、ISP 数字增益
   * 全部切到手动模式。后续循环只改 u32ExpTime，增益保持固定，
   * 这样满足“关闭自动曝光、关闭自动增益”的要求。
   */
  attr.enOpType = OP_TYPE_MANUAL;
  attr.stManual.enExpTimeOpType = OP_TYPE_MANUAL;
  attr.stManual.enAGainOpType = OP_TYPE_MANUAL;
  attr.stManual.enDGainOpType = OP_TYPE_MANUAL;
  attr.stManual.enISPDGainOpType = OP_TYPE_MANUAL;
  attr.stManual.u32ExpTime = (HI_U32)exp_us;
  attr.stManual.u32AGain = (HI_U32)manual_gain;
  attr.stManual.u32DGain = (HI_U32)manual_gain;
  attr.stManual.u32ISPDGain = (HI_U32)manual_gain;

  ret = HI_MPI_ISP_SetExposureAttr((VI_PIPE)pipe, &attr);
  state->last_ret = ret;
  if(ret == 0)
  {
    state->exp_us = exp_us;
    state->target_exp_us = exp_us;
  }

  return ret;
}

static void centroid_ae_restore(centroid_ae_state_t *state)
{
  int ret = 0;

  if(!state || !state->enabled)
  {
    return;
  }

  /* 关闭自定义曝光时，尽量恢复接管前保存的 ISP 曝光配置。
   * 如果现场原来就是 ISP 自动曝光，这一步会把系统带回原来的自动模式；
   * 如果保存失败，则只清理线程内部状态，不额外写未知配置到 ISP。
   */
  if(state->has_origin_attr)
  {
    ret = HI_MPI_ISP_SetExposureAttr((VI_PIPE)state->origin_pipe,
                                     &state->origin_attr);
    state->last_ret = ret;
  }

    state->enabled = 0;
    state->configured = 0;
    state->exp_us = 0;
    state->target_exp_us = 0;
    state->target_gray = 0;
    state->adjust = 0;
  state->has_origin_attr = 0;
}

static void centroid_ae_prepare(centroid_ae_state_t *state)
{
  centroid_ae_cfg_t cfg;
  ISP_EXPOSURE_ATTR_S attr;
  int exp_us = 0;
  int ret = 0;

  if(!state)
  {
    return;
  }

  centroid_ae_cfg_load(&cfg);
  state->adjust = 0;

  if(!cfg.enable)
  {
    centroid_ae_restore(state);
    state->saturation = cfg.saturation;
    return;
  }

  if(state->enabled && state->pipe != cfg.pipe)
  {
    centroid_ae_restore(state);
  }

  memset(&attr, 0, sizeof(attr));
  ret = HI_MPI_ISP_GetExposureAttr((VI_PIPE)cfg.pipe, &attr);
  if(ret != 0)
  {
    state->enabled = 1;
    state->configured = 0;
    state->pipe = cfg.pipe;
    state->saturation = cfg.saturation;
    state->target_ratio = cfg.target_ratio;
    state->target_gray = centroid_clamp_int((int)(cfg.saturation * cfg.target_ratio + 0.5),
                                            1,
                                            cfg.saturation);
    state->min_exp_us = cfg.min_exp_us;
    state->max_exp_us = cfg.max_exp_us;
    state->init_exp_us = cfg.init_exp_us;
    state->manual_gain = cfg.manual_gain;
    state->last_ret = ret;
    return;
  }

  if(!state->enabled)
  {
    state->origin_attr = attr;
    state->origin_pipe = cfg.pipe;
    state->has_origin_attr = 1;
  }

  if(!state->enabled || centroid_ae_cfg_changed(state, &cfg))
  {
    exp_us = centroid_ae_query_current_exp(&cfg, &attr);

    state->enabled = 1;
    state->configured = 0;
    state->pipe = cfg.pipe;
    state->saturation = cfg.saturation;
    state->target_ratio = cfg.target_ratio;
    state->target_gray = centroid_clamp_int((int)(cfg.saturation * cfg.target_ratio + 0.5),
                                            1,
                                            cfg.saturation);
    state->min_exp_us = cfg.min_exp_us;
    state->max_exp_us = cfg.max_exp_us;
    state->init_exp_us = cfg.init_exp_us;
    state->manual_gain = cfg.manual_gain;
    state->target_exp_us = exp_us;

    ret = centroid_ae_apply_manual(state, cfg.pipe, exp_us, cfg.manual_gain);
    if(ret == 0)
    {
      state->configured = 1;
    }
  }
}

static void centroid_ae_update_after_frame(centroid_ae_state_t *state,
                                           const centroid_result_t *result)
{
  int max_gray = 0;
  int target_gray = 0;
  int tolerance_gray = 0;
  int target_exp_us = 0;
  int adjust = 0;

  if(!state || !result || !state->enabled || !state->configured)
  {
    return;
  }

  state->adjust = 0;
  state->target_exp_us = state->exp_us;

  if(result->w <= 0 || result->h <= 0)
  {
    return;
  }

  max_gray = result->max_gray;
  target_gray = state->target_gray > 0 ?
                state->target_gray :
                centroid_clamp_int((int)(state->saturation * CENTROID_AE_TARGET_RATIO_DEFAULT + 0.5),
                                   1,
                                   state->saturation);
  tolerance_gray = (int)(target_gray * CENTROID_AE_TOLERANCE_RATIO + 0.5);
  if(tolerance_gray < 2)
  {
    tolerance_gray = 2;
  }
  target_exp_us = state->exp_us;

  /* 曝光调节必须和当前帧最大亮度检测绑定在一起：
   * centroid_detect() 已经从本帧图像得到 max_gray，只有在这一帧检测完成后才允许改曝光。
   * 现在的目标不是“只要不饱和就行”，而是把最大亮度稳定在饱和值的 ae_target_ratio 附近。
   * 默认 ae_target_ratio=0.8，所以 8bit 图像的目标最大灰度约为 204。
   *
   * 控制策略：
   * - 已经饱和时真实峰值未知，先按 1/2 快速降曝光，让图像尽快离开饱和平台；
   * - 未饱和但高于目标亮度时，按 target_gray / max_gray 比例降低曝光；
   * - 低于目标亮度时，按相同比例增加曝光，但单帧最多加倍，避免弱光时突然冲到过曝；
   * - 目标附近保留一个很小的死区，避免曝光在 0.8 饱和附近来回抖动。
   */
  if(max_gray >= state->saturation)
  {
    target_exp_us = state->exp_us / 2;
    adjust = -1;
  }
  else if(max_gray > target_gray + tolerance_gray)
  {
    double ratio = (double)target_gray / (double)max_gray;
    target_exp_us = (int)(state->exp_us * ratio + 0.5);
    adjust = -1;
  }
  else if(max_gray < target_gray - tolerance_gray)
  {
    if(max_gray <= 0)
    {
      target_exp_us = state->exp_us * 2;
    }
    else
    {
      double ratio = (double)target_gray / (double)max_gray;
      if(ratio > 2.0)
      {
        ratio = 2.0;
      }
      target_exp_us = (int)(state->exp_us * ratio + 0.5);
    }
    adjust = 1;
  }

  if(adjust < 0 && target_exp_us >= state->exp_us)
  {
    target_exp_us = state->exp_us - 1;
  }

  target_exp_us = centroid_clamp_int(target_exp_us,
                                     state->min_exp_us,
                                     state->max_exp_us);
  if(target_exp_us == state->exp_us)
  {
    adjust = 0;
  }

  state->adjust = adjust;
  state->target_exp_us = target_exp_us;
  if(adjust != 0)
  {
    centroid_ae_apply_manual(state, state->pipe, target_exp_us, state->manual_gain);
  }
}

static void centroid_result_fill_clarity(centroid_result_t *result,
                                         double clarity_gain,
                                         double clarity_exposure)
{
  if(!result)
  {
    return;
  }

  /* 用户给出的清晰度函数为：
   * 当前最大灰度值 / 增益设定值 / 曝光时间 / 超过当前最大灰度值 50% 灰度值的像元数量。
   * max_gray 和 half_area 来自当前帧；gain/exposure 来自 GSF_ID_SVP_CENTROID_CFG 对应的 svp_parm.centroid。
   * 只要任一分母无效，就把清晰度置 0，避免 web 曲线出现无穷大或 NaN。
   */
  if(result->max_gray > 0 && result->half_area > 0 &&
     clarity_gain > 0.0 && clarity_exposure > 0.0)
  {
    result->clarity = (double)result->max_gray /
                      clarity_gain /
                      clarity_exposure /
                      (double)result->half_area;
  }
  else
  {
    result->clarity = 0.0;
  }
}

static void centroid_status_update(const centroid_result_t *result,
                                   double clarity_gain,
                                   double clarity_exposure,
                                   int clarity_window_w,
                                   int clarity_window_h,
                                   const centroid_ae_state_t *ae_state)
{
  if(!result)
  {
    return;
  }

  pthread_mutex_lock(&s_centroid_status_lock);

  /* 质心线程是唯一写入者，web 消息线程只读取。
   * 这里把算法层的 centroid_result_t 转成对外公开的 gsf_svp_centroid_status_t，
   * 浏览器轮询时直接读这一份快照即可，不会反过来触发抓帧或增加 MMZ 压力。
   */
  s_centroid_status.running = 1;
  s_centroid_status.valid = result->valid;
  s_centroid_status.chn = result->chn;
  s_centroid_status.w = result->w;
  s_centroid_status.h = result->h;
  s_centroid_status.x = result->x;
  s_centroid_status.y = result->y;
  s_centroid_status.area = result->area;
  s_centroid_status.max_gray = result->max_gray;
  s_centroid_status.half_area = result->half_area;
  s_centroid_status.clarity = result->clarity;
  s_centroid_status.clarity_gain = clarity_gain;
  s_centroid_status.clarity_exposure = clarity_exposure;
  s_centroid_status.clarity_window_w = clarity_window_w;
  s_centroid_status.clarity_window_h = clarity_window_h;
  if(ae_state && ae_state->enabled)
  {
    s_centroid_status.ae_enabled = 1;
    s_centroid_status.ae_exp_us = ae_state->exp_us;
    s_centroid_status.ae_target_exp_us = ae_state->target_exp_us;
    s_centroid_status.ae_adjust = ae_state->adjust;
    s_centroid_status.ae_saturation = ae_state->saturation;
    s_centroid_status.ae_target_gray = ae_state->target_gray;
    s_centroid_status.ae_last_ret = ae_state->last_ret;
  }
  else
  {
    s_centroid_status.ae_enabled = 0;
    s_centroid_status.ae_exp_us = 0;
    s_centroid_status.ae_target_exp_us = 0;
    s_centroid_status.ae_adjust = 0;
    s_centroid_status.ae_saturation = 0;
    s_centroid_status.ae_target_gray = 0;
    s_centroid_status.ae_last_ret = 0;
  }
  s_centroid_status.pts = result->pts;
  s_centroid_status.update_ms = centroid_now_ms();
  s_centroid_status.age_ms = 0;

  pthread_mutex_unlock(&s_centroid_status_lock);
}

int centroid_get_latest(gsf_svp_centroid_status_t *status)
{
  unsigned long long now_ms = centroid_now_ms();

  if(!status)
  {
    return -1;
  }

  pthread_mutex_lock(&s_centroid_status_lock);
  *status = s_centroid_status;
  if(status->update_ms > 0 && now_ms >= status->update_ms)
  {
    unsigned long long age_ms = now_ms - status->update_ms;
    status->age_ms = (age_ms > 0x7fffffffULL) ? 0x7fffffff : (int)age_ms;
  }
  pthread_mutex_unlock(&s_centroid_status_lock);

  return 0;
}

int centroid_is_running(void)
{
  int running = 0;

  pthread_mutex_lock(&s_centroid_status_lock);
  running = s_centroid_status.running;
  pthread_mutex_unlock(&s_centroid_status_lock);

  /* centroid_alg=1 后，线程可能因为 VPSS 尚未 ready 或通道号配置不对而初始化失败。
   * 这时 running 会被线程置 0，alive 也会清掉；svp.c 主循环看到 false 后会自动重试，
   * 避免用户已经 SET 为 1，但算法永远不再启动的状态。
   */
  return running && s_centroid_alive;
}

static void centroid_join_exited_thread(void)
{
  if(s_centroid_thread && !s_centroid_alive)
  {
    /* 线程初始化失败后会自己退出，但 pthread_t 仍是一个需要回收的 joinable 句柄。
     * 下一次重试启动前先 join 掉旧线程，既避免资源泄漏，也避免 s_centroid_thread
     * 残留导致 centroid_start() 误以为算法仍在运行。
     */
    pthread_join(s_centroid_thread, NULL);
    s_centroid_thread = 0;
  }
}

static speed_t centroid_baud_to_speed(int baudrate)
{
  switch(baudrate)
  {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return B115200;
  }
}

static int centroid_serial_open(const char *dev, int baudrate)
{
  int fd = -1;
  struct termios tio;
  speed_t speed = centroid_baud_to_speed(baudrate);

  if(!dev || !dev[0])
  {
    dev = CENTROID_UART_DEFAULT;
  }

  fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if(fd < 0)
  {
    printf("centroid serial open failed, dev:%s, errno:%d(%s)\n",
           dev, errno, strerror(errno));
    return -1;
  }

  memset(&tio, 0, sizeof(tio));
  if(tcgetattr(fd, &tio) != 0)
  {
    printf("centroid serial tcgetattr failed, dev:%s, errno:%d(%s)\n",
           dev, errno, strerror(errno));
    close(fd);
    return -1;
  }

  /* 串口使用 8N1 原始模式：
   * 8 个数据位、无校验、1 个停止位、关闭软/硬件流控。
   * 上位机只需要按行解析 ASCII 文本即可，不会被终端回显或换行转换干扰。
   */
  tio.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF | IXANY);
  tio.c_oflag &= ~OPOST;
  tio.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  cfsetispeed(&tio, speed);
  cfsetospeed(&tio, speed);
  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cflag &= ~CSIZE;
  tio.c_cflag |= CS8;
  tio.c_cflag &= ~PARENB;
  tio.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
  tio.c_cflag &= ~CRTSCTS;
#endif
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 5;

  if(tcsetattr(fd, TCSANOW, &tio) != 0)
  {
    printf("centroid serial tcsetattr failed, dev:%s, errno:%d(%s)\n",
           dev, errno, strerror(errno));
    close(fd);
    return -1;
  }

  tcflush(fd, TCIOFLUSH);
  printf("centroid serial open ok, dev:%s, baudrate:%d\n", dev, baudrate);
  return fd;
}

static void centroid_serial_write(int fd, const centroid_result_t *result)
{
  char line[160];
  int len = 0;

  if(fd < 0 || !result)
  {
    return;
  }

  /* 输出协议保持简单稳定，方便单片机、PC 串口助手或 Python 脚本解析：
   * CENTROID,valid,chn,x,y,w,h,area,pts
   * valid=1 时 x/y 为质心坐标；valid=0 时 x/y 为 -1，表示本帧没有检测到亮斑。
   */
  if(result->valid)
  {
    len = snprintf(line, sizeof(line),
                   "CENTROID,1,%d,%.2f,%.2f,%d,%d,%.0f,%llu\r\n",
                   result->chn, result->x, result->y, result->w, result->h,
                   result->area, result->pts);
  }
  else
  {
    len = snprintf(line, sizeof(line),
                   "CENTROID,0,%d,-1,-1,%d,%d,0,%llu\r\n",
                   result->chn, result->w, result->h, result->pts);
  }

  if(len > 0)
  {
    write(fd, line, len);
  }
}

int centroid_start(void)
{
  centroid_join_exited_thread();

  if(s_centroid_thread)
  {
    return 0;
  }

  s_centroid_stop = 0;
  s_centroid_alive = 1;
  centroid_status_reset(1);

  int ret = pthread_create(&s_centroid_thread, NULL, centroid_task, NULL);
  if(ret != 0)
  {
    s_centroid_alive = 0;
    centroid_status_reset(0);
  }
  return ret;
}

int centroid_stop(void)
{
  if(s_centroid_thread)
  {
    s_centroid_stop = 1;
    pthread_join(s_centroid_thread, NULL);
    s_centroid_thread = 0;
  }
  s_centroid_alive = 0;
  centroid_status_reset(0);
  return 0;
}

static void* centroid_task(void* p)
{
  int ret = 0;
  int serial_fd = -1;
  int serial_baud = 0;
  char serial_dev[64] = {0};
  time_t next_serial_open_time = 0;
  int warn_count = 0;
  centroid_ae_state_t ae_state;

  int vpss_grp = svp_parm.centroid.vpss_grp;
  int vpss_chn = svp_parm.centroid.vpss_chn;

  memset(&ae_state, 0, sizeof(ae_state));

  ret = centroid_init(vpss_grp, vpss_chn);
  if(ret < 0)
  {
    printf("centroid task init failed, VpssGrp:%d, VpssChn:%d\n", vpss_grp, vpss_chn);
    centroid_status_reset(0);
    s_centroid_alive = 0;
    return NULL;
  }

  while(!s_centroid_stop)
  {
    centroid_result_t result;
    const char *dev = svp_parm.centroid.uart_dev[0] ?
                      svp_parm.centroid.uart_dev : CENTROID_UART_DEFAULT;
    int baudrate = svp_parm.centroid.baudrate > 0 ?
                   svp_parm.centroid.baudrate : CENTROID_BAUD_DEFAULT;
    int threshold = svp_parm.centroid.threshold;
    int min_area = svp_parm.centroid.min_area > 0 ?
                   svp_parm.centroid.min_area : CENTROID_MIN_AREA_DEFAULT;
    int window_size = svp_parm.centroid.window_size > 0 ?
                      svp_parm.centroid.window_size : CENTROID_WINDOW_DEFAULT;
    int interval_ms = svp_parm.centroid.interval_ms > 0 ?
                      svp_parm.centroid.interval_ms : CENTROID_INTERVAL_DEFAULT;
    double clarity_gain = centroid_positive_or_default(svp_parm.centroid.clarity_gain, 1.0);
    double clarity_exposure = centroid_positive_or_default(svp_parm.centroid.clarity_exposure, 1.0);
    int clarity_window_w = svp_parm.centroid.clarity_window_w > 0 ?
                           svp_parm.centroid.clarity_window_w :
                           CENTROID_CLARITY_WINDOW_W_DEFAULT;
    int clarity_window_h = svp_parm.centroid.clarity_window_h > 0 ?
                           svp_parm.centroid.clarity_window_h :
                           CENTROID_CLARITY_WINDOW_H_DEFAULT;

    if(interval_ms < 10)
    {
      interval_ms = 10;
    }
    /* window_size 允许运行中通过 GSF_ID_SVP_CENTROID_CFG 实时调整。
     * 小窗口用于锁住最亮点附近的主光斑核心，避免大连通域中的反光、拖尾或噪声
     * 被一起送入灰度矩计算。范围限制和 msg_func.c 保持一致，保证直接改 json 时也有兜底。
     */
    window_size = centroid_clamp_int(window_size, CENTROID_WINDOW_MIN, CENTROID_WINDOW_MAX);
    /* 清晰度曲线浮窗尺寸允许通过 CENTROID_CFG 实时调整。
     * 这里和 msg_func.c 使用同样范围兜底，保证直接修改 svp_parm.json 时也不会出现
     * 0 尺寸、负尺寸或过大的覆盖窗口。
     */
    clarity_window_w = centroid_clamp_int(clarity_window_w,
                                          CENTROID_CLARITY_WINDOW_W_MIN,
                                          CENTROID_CLARITY_WINDOW_W_MAX);
    clarity_window_h = centroid_clamp_int(clarity_window_h,
                                          CENTROID_CLARITY_WINDOW_H_MIN,
                                          CENTROID_CLARITY_WINDOW_H_MAX);

    /* 如果配置接口在运行过程中更新 svp_parm.centroid，
     * 下一轮循环会自动感知串口、波特率、清晰度 gain/exposure 等变化。
     * 若只是手工改磁盘上的 svp_parm.json，则需要重启 svp.exe 后才会重新加载文件。
     */
    if(serial_baud != baudrate || strcmp(serial_dev, dev) != 0)
    {
      if(serial_fd >= 0)
      {
        close(serial_fd);
        serial_fd = -1;
      }
      strncpy(serial_dev, dev, sizeof(serial_dev)-1);
      serial_dev[sizeof(serial_dev)-1] = '\0';
      serial_baud = baudrate;
      next_serial_open_time = 0;
      warn_count = 0;
    }

    if(serial_fd < 0 && time(NULL) >= next_serial_open_time)
    {
      serial_fd = centroid_serial_open(serial_dev, serial_baud);
      if(serial_fd < 0)
      {
        next_serial_open_time = time(NULL) + 2;
      }
    }

    centroid_ae_prepare(&ae_state);

    memset(&result, 0, sizeof(result));
    ret = centroid_detect(&result, threshold, min_area, window_size);
    if(ret == 0)
    {
      centroid_result_fill_clarity(&result, clarity_gain, clarity_exposure);
      centroid_ae_update_after_frame(&ae_state, &result);
      centroid_status_update(&result,
                             clarity_gain,
                             clarity_exposure,
                             clarity_window_w,
                             clarity_window_h,
                             &ae_state);
      if(result.valid)
      {
        printf("centroid chn:%d, x:%.2f, y:%.2f, area:%.0f, size:%dx%d\n",
               result.chn, result.x, result.y, result.area, result.w, result.h);
        centroid_serial_write(serial_fd, &result);
      }
      else if(svp_parm.centroid.print_no_spot)
      {
        printf("centroid no spot, chn:%d, size:%dx%d\n",
               result.chn, result.w, result.h);
        centroid_serial_write(serial_fd, &result);
      }
    }
    else if(ret < 0 && serial_fd < 0 && warn_count++ < 5)
    {
      printf("centroid serial not ready, result will only print to console.\n");
    }

    usleep(interval_ms * 1000);
  }

  if(serial_fd >= 0)
  {
    close(serial_fd);
  }
  centroid_ae_restore(&ae_state);
  centroid_deinit();
  s_centroid_alive = 0;
  return NULL;
}
