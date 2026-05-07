#ifndef __CENTROID_SAMPLE_H__
#define __CENTROID_SAMPLE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 质心检测结果。
 * valid 为 1 表示检测到满足面积阈值的亮斑，x/y 是以图像左上角为原点的像素坐标。
 * area 是二值亮斑连通域面积，pts 来自海思视频帧时间戳，便于上位机做时序对齐。
 * max_gray 是当前帧强度图最大灰度值，half_area 是超过 max_gray 50% 的像元数量；
 * clarity 由线程层结合增益设定值和曝光时间计算，web 端用它绘制随时间变化的清晰度曲线。
 */
typedef struct {
  int valid;
  int chn;
  int w;
  int h;
  float x;
  float y;
  double area;
  int max_gray;
  int half_area;
  double clarity;
  unsigned long long pts;
} centroid_result_t;

/* 初始化 VPSS 抓帧通道。
 * VpssGrp/VpssChn 应与当前视频流水线里已经启用的 VPSS 组和通道一致。
 */
int centroid_init(int VpssGrp, int VpssChn);

/* 从 VPSS 抓取一帧并提取最亮连通域的灰度加权质心。
 * threshold: 1~254 使用固定阈值；其它值使用 OTSU 自动阈值。
 * min_area : 小于该面积的亮斑会被当作噪声丢弃。
 * 返回值：0 表示本轮处理完成，result.valid 决定是否检测到亮斑；
 *        1 表示本轮没有等到 VPSS 帧；-1 表示抓帧或处理出错。
 */
/* window_size 表示以最亮点为中心参与质心计算的小窗口边长，单位为像素。
 * 先用全局阈值找到候选光斑和最亮点，再只在这个局部窗口内做灰度加权矩计算，
 * 可以避免远处反光、红色光晕、拖尾或热噪声被大窗口一起计入，导致质心被拉偏。
 */
int centroid_detect(centroid_result_t *result, int threshold, int min_area, int window_size);

/* 释放 VPSS 抓帧资源。 */
int centroid_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
