#ifndef __CENTROID_SAMPLE_H__
#define __CENTROID_SAMPLE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 质心检测结果。
 * valid 为 1 表示检测到满足面积阈值的亮斑，x/y 是以图像左上角为原点的像素坐标。
 * area 是二值亮斑连通域面积，pts 来自海思视频帧时间戳，便于上位机做时序对齐。
 */
typedef struct {
  int valid;
  int chn;
  int w;
  int h;
  float x;
  float y;
  double area;
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
int centroid_detect(centroid_result_t *result, int threshold, int min_area);

/* 释放 VPSS 抓帧资源。 */
int centroid_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
