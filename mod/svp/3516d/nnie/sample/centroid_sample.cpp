#include <cstdio>
#include <cstring>
#include <sys/select.h>
#include <opencv2/opencv.hpp>

#include "centroid_sample.h"
#include "vpss_capture.h"

/* 这里沿用 yolo_sample.cpp 的抓帧方式：VpssCapture 负责从 VPSS 通道取
 * VIDEO_FRAME_INFO_S，再通过海思 IVE 做 YUV 到 RGB 的转换，最终给算法层一个
 * cv::Mat。质心算法只关心亮度分布，因此不直接操作 VPSS 的 YUV 内存。
 */
static VpssCapture s_vcap;
static cv::Mat s_image;
static int s_vcap_fd = -1;
static int s_vpss_grp = -1;
static int s_vpss_chn = -1;

int centroid_init(int VpssGrp, int VpssChn)
{
  if(s_vcap_fd > 0)
  {
    centroid_deinit();
  }

  s_vpss_grp = VpssGrp;
  s_vpss_chn = VpssChn;
  s_vcap_fd = s_vcap.init(VpssGrp, VpssChn);
  if(s_vcap_fd <= 0)
  {
    printf("centroid_init failed, VpssGrp:%d, VpssChn:%d, fd:%d\n",
           VpssGrp, VpssChn, s_vcap_fd);
    s_vcap_fd = -1;
    return -1;
  }

  printf("centroid_init ok, VpssGrp:%d, VpssChn:%d, fd:%d\n",
         VpssGrp, VpssChn, s_vcap_fd);
  return 0;
}

static int wait_vpss_frame(void)
{
  fd_set read_fds;
  struct timeval to;

  if(s_vcap_fd <= 0)
  {
    return -1;
  }

  FD_ZERO(&read_fds);
  FD_SET(s_vcap_fd, &read_fds);
  to.tv_sec = 1;
  to.tv_usec = 0;

  return select(s_vcap_fd + 1, &read_fds, NULL, NULL, &to);
}

int centroid_detect(centroid_result_t *result, int threshold, int min_area)
{
  VIDEO_FRAME_INFO_S *pstFrame = NULL;
  cv::Mat intensity;
  cv::Mat binary;
  int ret = 0;

  if(!result)
  {
    return -1;
  }

  memset(result, 0, sizeof(*result));
  result->chn = s_vpss_chn;

  ret = wait_vpss_frame();
  if(ret < 0)
  {
    printf("centroid vpss select failed.\n");
    return -1;
  }
  else if(ret == 0)
  {
    return 1;
  }

  /* 注意：这里必须复用同一个 cv::Mat。
   * VpssCapture::YUV2Mat() 内部会把海思 MMZ 内存映射给传入的 Mat。如果每帧都
   * 传一个新的临时 Mat，底层会持续申请新的 MMZ 缓冲区，最终触发
   * "mmz_userdev: ioctl_mmb_alloc failed"。因此 s_image 是模块级持久对象，
   * 分辨率不变时只申请一次 MMZ，后续每帧复用同一块缓存。
   */
  ret = s_vcap.get_frame_lock(s_image, &pstFrame);
  if(ret != 0 || s_image.empty())
  {
    printf("centroid vpss capture failed, ret:%d, empty:%d\n", ret, s_image.empty());
    if(pstFrame)
    {
      s_vcap.get_frame_unlock(pstFrame);
    }
    return -1;
  }

  result->w = s_image.cols;
  result->h = s_image.rows;
  if(pstFrame)
  {
    result->pts = pstFrame->stVFrame.u64PTS;
  }

  /* 光斑可能是白光，也可能是红/绿/蓝激光。单纯转灰度时，红/蓝激光的亮度
   * 会被灰度权重压低，固定阈值容易漏检。因此这里取三个颜色通道的最大值
   * 作为强度图：只要任意颜色通道饱和，就能被当作亮斑候选区域。
   * threshold 在 1~254 之间时使用固定阈值，适合已知激光亮度的场景；
   * threshold 为 0 或越界时使用 OTSU 自动阈值，适合现场亮度变化较大的场景。
   */
  {
    cv::Mat channels[3];
    cv::split(s_image, channels);
    cv::max(channels[0], channels[1], intensity);
    cv::max(intensity, channels[2], intensity);
  }
  if(threshold > 0 && threshold < 255)
  {
    cv::threshold(intensity, binary, threshold, 255, cv::THRESH_BINARY);
  }
  else
  {
    cv::threshold(intensity, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
  }

  /* 二值图里可能会有热噪声或反光点。这里用连通域找出面积最大的亮斑，
   * 再只对该连通域做灰度加权矩计算。这样比直接对整幅二值图求矩更稳：
   * 多个小噪声点不会把质心拉偏，亮斑内部的灰度分布也会参与亚像素定位。
   */
  int label_count = 0;
  cv::Mat labels;
  cv::Mat stats;
  cv::Mat centroids;
  int best_label = -1;
  int best_area = 0;
  int area_limit = min_area > 0 ? min_area : 4;

  label_count = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);
  for(int label = 1; label < label_count; label++)
  {
    int area = stats.at<int>(label, cv::CC_STAT_AREA);
    if(area >= area_limit && area > best_area)
    {
      best_area = area;
      best_label = label;
    }
  }

  if(best_label >= 0)
  {
    cv::Mat spot_mask = (labels == best_label);
    cv::Mat weighted = cv::Mat::zeros(intensity.size(), intensity.type());
    intensity.copyTo(weighted, spot_mask);

    cv::Moments m = cv::moments(weighted, false);
    if(m.m00 > 0.0)
    {
      result->valid = 1;
      result->area = best_area;
      result->x = static_cast<float>(m.m10 / m.m00);
      result->y = static_cast<float>(m.m01 / m.m00);
    }
  }

  s_vcap.get_frame_unlock(pstFrame);
  return 0;
}

int centroid_deinit(void)
{
  if(s_vcap_fd > 0)
  {
    s_vcap.destroy();
  }

  s_vcap_fd = -1;
  s_vpss_grp = -1;
  s_vpss_chn = -1;
  s_image.release();
  return 0;
}
