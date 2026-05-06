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

#define CENTROID_UART_DEFAULT "/dev/ttyAMA0"
#define CENTROID_BAUD_DEFAULT 115200
#define CENTROID_THRESHOLD_DEFAULT 220
#define CENTROID_MIN_AREA_DEFAULT 4
#define CENTROID_INTERVAL_DEFAULT 100

/* 质心算法运行在线程中，生命周期由 svp.c 根据 svp_parm.svp.centroid_alg 控制。
 * 这样做和 yolo/lpr 的开关方式保持一致：配置文件或消息接口改变算法开关后，
 * 主循环负责启动/停止，不需要重启整个 svp.exe。
 */
static pthread_t s_centroid_thread = 0;
static volatile int s_centroid_stop = 0;

static void* centroid_task(void* p);

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
  if(s_centroid_thread)
  {
    return 0;
  }

  s_centroid_stop = 0;
  return pthread_create(&s_centroid_thread, NULL, centroid_task, NULL);
}

int centroid_stop(void)
{
  if(s_centroid_thread)
  {
    s_centroid_stop = 1;
    pthread_join(s_centroid_thread, NULL);
    s_centroid_thread = 0;
  }
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

  int vpss_grp = svp_parm.centroid.vpss_grp;
  int vpss_chn = svp_parm.centroid.vpss_chn;

  ret = centroid_init(vpss_grp, vpss_chn);
  if(ret < 0)
  {
    printf("centroid task init failed, VpssGrp:%d, VpssChn:%d\n", vpss_grp, vpss_chn);
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
    int interval_ms = svp_parm.centroid.interval_ms > 0 ?
                      svp_parm.centroid.interval_ms : CENTROID_INTERVAL_DEFAULT;

    if(interval_ms < 10)
    {
      interval_ms = 10;
    }

    /* 如果 svp_parm.centroid 在运行过程中被配置接口更新，下一轮循环会自动感知
     * 串口设备或波特率变化，并关闭旧串口、重开新串口。若只是手工改磁盘上的
     * svp_parm.json，则需要重启 svp.exe 后才会重新加载文件。
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

    memset(&result, 0, sizeof(result));
    ret = centroid_detect(&result, threshold, min_area);
    if(ret == 0)
    {
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
  centroid_deinit();
  return NULL;
}
