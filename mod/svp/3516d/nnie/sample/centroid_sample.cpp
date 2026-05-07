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

static cv::Rect centroid_make_window(cv::Point center, int image_w, int image_h, int window_size)
{
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;

  if(image_w <= 0 || image_h <= 0)
  {
    return cv::Rect();
  }

  /* 这里的 window_size 是“质心局部计算窗口”的边长。
   * 算法仍然先在整幅图里找最亮点，避免窗口一开始就错过光斑；
   * 找到最亮点以后，再把参与矩计算的像素限制在这个小窗口内。
   *
   * 如果窗口太小，只能看到饱和核心，坐标会抖；如果窗口太大，远处反光和噪声又会进来。
   * 因此这里做一个基础兜底：下限 8 像素，上限不超过当前图像最大边。
   * 具体默认值和 web 可配置范围在 centroid_cfg 中维护。
   */
  window_size = centroid_clamp_int(window_size, 8, image_w > image_h ? image_w : image_h);
  if((window_size & 1) == 0)
  {
    window_size += 1;
  }

  x0 = center.x - window_size / 2;
  y0 = center.y - window_size / 2;
  x1 = x0 + window_size;
  y1 = y0 + window_size;

  if(x0 < 0)
  {
    x1 -= x0;
    x0 = 0;
  }
  if(y0 < 0)
  {
    y1 -= y0;
    y0 = 0;
  }
  if(x1 > image_w)
  {
    x0 -= x1 - image_w;
    x1 = image_w;
  }
  if(y1 > image_h)
  {
    y0 -= y1 - image_h;
    y1 = image_h;
  }

  x0 = centroid_clamp_int(x0, 0, image_w);
  y0 = centroid_clamp_int(y0, 0, image_h);
  x1 = centroid_clamp_int(x1, x0, image_w);
  y1 = centroid_clamp_int(y1, y0, image_h);

  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

int centroid_detect(centroid_result_t *result, int threshold, int min_area, int window_size)
{
  VIDEO_FRAME_INFO_S *pstFrame = NULL;
  cv::Mat intensity;
  cv::Mat centroid_intensity;
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

  /* 光斑可能是白光，也可能是红/绿/蓝激光。为了不漏掉单色激光，检测和曝光判断
   * 继续取三个颜色通道的最大值作为强度图：只要任意颜色通道饱和，就能被当作亮斑候选区域。
   *
   * 但质心定位不能直接用这个最大通道图。现场图像里常见右侧红色光晕/镜头反射，
   * 这些区域的 R 通道可能很亮，却不是光斑主峰；如果参与矩计算，红色 X 会被明显拉偏。
   * 因此下面额外生成一张亮度图 centroid_intensity，后续只用它的高亮核心计算质心。
   * threshold 在 1~254 之间时使用固定阈值，适合已知激光亮度的场景；
   * threshold 为 0 或越界时使用 OTSU 自动阈值，适合现场亮度变化较大的场景。
   */
  {
    cv::Mat channels[3];
    cv::split(s_image, channels);
    cv::max(channels[0], channels[1], intensity);
    cv::max(intensity, channels[2], intensity);
    cv::cvtColor(s_image, centroid_intensity, cv::COLOR_RGB2GRAY);
  }
  if(threshold > 0 && threshold < 255)
  {
    cv::threshold(intensity, binary, threshold, 255, cv::THRESH_BINARY);
  }
  else
  {
    cv::threshold(intensity, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
  }

  /* 先记录整幅图的最大灰度，用于没有检测到有效光斑时仍能观察现场最大亮度。
   * 真正用于清晰度函数和质心定位的 max_gray/half_area 会在选出最大光斑连通域后，
   * 重新限制到该光斑区域内部计算。否则画面中其它亮背景或反光点会把 half_area 撑大，
   * 造成 web 上的清晰度曲线看起来被压扁，甚至像是不再更新。
   */
  {
    double min_val = 0.0;
    double max_val = 0.0;

    cv::minMaxLoc(intensity, &min_val, &max_val);
    result->max_gray = static_cast<int>(max_val + 0.5);
  }

  /* 二值图里可能会有热噪声或反光点。这里用连通域找出面积最大的亮斑，
   * 再只在该连通域内部做半峰值亮核统计和质心计算。这样比直接对整幅图求 half_area、
   * 或对整个阈值连通域求矩更稳：背景亮区不会稀释清晰度，光斑低亮光晕和拖尾也不容易
   * 把质心从视觉中心拉偏。
   */
  int label_count = 0;
  cv::Mat labels;
  cv::Mat stats;
  cv::Mat centroids;
  int best_label = -1;
  int best_area = 0;
  int area_limit = min_area > 0 ? min_area : 4;

  label_count = cv::connectedComponentsWithStats(binary, labels, stats, centroids, 8, CV_32S);

  /* 优先选择“包含最亮点”的连通域，而不是简单选择面积最大的连通域。
   * 现场里大面积红色光晕、反射拖尾或背景噪声可能比真实光斑面积更大；
   * 如果先选最大面积，再在里面找最亮点，就可能从一开始选错对象。
   *
   * 这里用亮度图 centroid_intensity 在二值候选区 binary 内寻找最亮点，
   * 它比最大颜色通道更不容易被单纯红色反光误导。只有这个最亮点所在区域
   * 小于 min_area、看起来像孤立热噪声时，才退回旧的“面积最大”策略。
   */
  if(label_count > 1 && cv::countNonZero(binary) > 0)
  {
    double seed_min_val = 0.0;
    double seed_max_val = 0.0;
    cv::Point seed_max_loc;

    cv::minMaxLoc(centroid_intensity, &seed_min_val, &seed_max_val, NULL, &seed_max_loc, binary);
    if(seed_max_val > 0.0 &&
       seed_max_loc.x >= 0 && seed_max_loc.x < labels.cols &&
       seed_max_loc.y >= 0 && seed_max_loc.y < labels.rows)
    {
      int seed_label = labels.at<int>(seed_max_loc);
      if(seed_label > 0 && seed_label < label_count)
      {
        int seed_area = stats.at<int>(seed_label, cv::CC_STAT_AREA);
        if(seed_area >= area_limit)
        {
          best_label = seed_label;
          best_area = seed_area;
        }
      }
    }
  }

  for(int label = 1; best_label < 0 && label < label_count; label++)
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
    cv::Mat window_mask;
    cv::Mat local_spot_mask;
    cv::Mat half_mask;
    cv::Mat core_mask;
    cv::Mat centroid_core_mask;
    cv::Mat centroid_labels;
    cv::Mat centroid_stats;
    cv::Mat centroid_centers;
    cv::Mat centroid_mask;
    cv::Mat weighted = cv::Mat::zeros(centroid_intensity.size(), centroid_intensity.type());
    double spot_min_val = 0.0;
    double spot_max_val = 0.0;
    double core_min_val = 0.0;
    double core_max_val = 0.0;
    cv::Point core_max_loc;
    cv::Rect local_window;
    int local_area = 0;
    int core_label_count = 0;
    int core_label = -1;

    /* 清晰度定义里的“当前最大灰度值”和“超过当前最大灰度值 50% 的像元数量”
     * 应该对应当前被选中的光斑，而不是整幅图。这里用 spot_mask 把统计范围限制在
     * 最大亮斑连通域内，避免其它反光点、白墙、字幕或噪声参与清晰度分母。
     */
    /* 先在最大候选光斑内找到亮度图的最亮点，再以这个点为中心生成局部窗口。
     * 后续真正参与 max_gray、half_area 和质心矩计算的区域，是
     * “最大候选光斑 ∩ 局部窗口”。这样既不会错过主光斑，又能把远处噪声挡在窗口外。
     */
    cv::minMaxLoc(centroid_intensity, &core_min_val, &core_max_val, NULL, &core_max_loc, spot_mask);
    if(core_max_val > 0.0)
    {
      local_window = centroid_make_window(core_max_loc, centroid_intensity.cols, centroid_intensity.rows, window_size);
      window_mask = cv::Mat::zeros(spot_mask.size(), spot_mask.type());
      if(local_window.width > 0 && local_window.height > 0)
      {
        window_mask(local_window).setTo(255);
        cv::bitwise_and(spot_mask, window_mask, local_spot_mask);
      }
    }
    if(local_spot_mask.empty() || cv::countNonZero(local_spot_mask) <= 0)
    {
      /* 正常情况下局部窗口一定包含最亮点，因此 local_spot_mask 不会为空。
       * 这里保留兜底，是为了防止异常配置或边界坐标导致本帧直接丢失结果。
       */
      local_spot_mask = spot_mask;
    }
    local_area = cv::countNonZero(local_spot_mask);

    cv::minMaxLoc(intensity, &spot_min_val, &spot_max_val, NULL, NULL, local_spot_mask);
    result->max_gray = static_cast<int>(spot_max_val + 0.5);
    if(spot_max_val > 0.0)
    {
      cv::threshold(intensity, half_mask, spot_max_val * 0.5, 255, cv::THRESH_BINARY);
      cv::bitwise_and(half_mask, local_spot_mask, core_mask);
      result->half_area = cv::countNonZero(core_mask);
    }

    /* 质心定位使用亮度图中的高亮主峰核心，而不是最大颜色通道图。
     * 这里先在当前光斑连通域里找亮度最大点，再从 75% 峰值以上的区域中，只保留
     * “包含该最亮点”的连通域。这样可以过滤掉镜头反射、红色光晕、拖尾等与主峰分离的亮区。
     * 如果 75% 峰值核心太小，则退回 50% 峰值核心；再不行才退回局部窗口内的候选光斑区域。
     */
    cv::minMaxLoc(centroid_intensity, &core_min_val, &core_max_val, NULL, &core_max_loc, local_spot_mask);
    if(core_max_val > 0.0)
    {
      cv::threshold(centroid_intensity, centroid_core_mask, core_max_val * 0.75, 255, cv::THRESH_BINARY);
      cv::bitwise_and(centroid_core_mask, local_spot_mask, centroid_core_mask);
      if(cv::countNonZero(centroid_core_mask) < 4)
      {
        cv::threshold(centroid_intensity, centroid_core_mask, core_max_val * 0.50, 255, cv::THRESH_BINARY);
        cv::bitwise_and(centroid_core_mask, local_spot_mask, centroid_core_mask);
      }
    }

    if(!centroid_core_mask.empty() && cv::countNonZero(centroid_core_mask) > 0)
    {
      core_label_count = cv::connectedComponentsWithStats(centroid_core_mask,
                                                          centroid_labels,
                                                          centroid_stats,
                                                          centroid_centers,
                                                          8,
                                                          CV_32S);
      if(core_max_loc.x >= 0 && core_max_loc.x < centroid_labels.cols &&
         core_max_loc.y >= 0 && core_max_loc.y < centroid_labels.rows)
      {
        core_label = centroid_labels.at<int>(core_max_loc);
      }
      if(core_label > 0 && core_label < core_label_count)
      {
        centroid_mask = (centroid_labels == core_label);
      }
      else
      {
        centroid_mask = centroid_core_mask;
      }
    }
    else
    {
      centroid_mask = result->half_area > 0 ? core_mask : local_spot_mask;
    }

    centroid_intensity.copyTo(weighted, centroid_mask);

    cv::Moments m = cv::moments(weighted, false);
    if(m.m00 > 0.0)
    {
      result->valid = 1;
      result->area = local_area;
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
