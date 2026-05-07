//os
#include <errno.h>
#include <stdio.h>
#include <libgen.h>
#include <string.h>
 
//top
#include "inc/gsf.h"

//mod
#include "svp.h"
#include "mod/app/inc/app.h"

//myself
#include "cfg.h"
#include "msg_func.h"

#define CENTROID_WINDOW_DEFAULT 64
#define CENTROID_WINDOW_MIN 8
#define CENTROID_WINDOW_MAX 512
#define CENTROID_CLARITY_WINDOW_W_DEFAULT 360
#define CENTROID_CLARITY_WINDOW_H_DEFAULT 150
#define CENTROID_CLARITY_WINDOW_W_MIN 180
#define CENTROID_CLARITY_WINDOW_H_MIN 90
#define CENTROID_CLARITY_WINDOW_W_MAX 960
#define CENTROID_CLARITY_WINDOW_H_MAX 540

static void svp_clarity_param_normalize(double *gain, double *exposure)
{
  /* 清晰度函数为 max_gray / gain / exposure / half_area。
   * gain 和 exposure 都在分母上，web 端实时 SET 时如果误填 0 或负数，
   * 这里统一兜底为 1.0，避免算法线程下一帧计算时出现除零。
   */
  if(*gain <= 0.0)
  {
    *gain = 1.0;
  }
  if(*exposure <= 0.0)
  {
    *exposure = 1.0;
  }
}

static int svp_clamp_int(int value, int min_value, int max_value)
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

static void svp_centroid_param_normalize(gsf_svp_centroid_t *centroid)
{
  if(!centroid)
  {
    return;
  }

  svp_clarity_param_normalize(&centroid->clarity_gain, &centroid->clarity_exposure);

  /* window_size 是质心局部计算窗口边长。
   * 线程会先在整幅图里找到最亮点，再只在这个窗口里做矩计算；
   * 这里限制范围，避免 web 端误填 0 或特别大的数，让算法退回“大窗口被噪声拉偏”的状态。
   */
  if(centroid->window_size <= 0)
  {
    centroid->window_size = CENTROID_WINDOW_DEFAULT;
  }
  centroid->window_size = svp_clamp_int(centroid->window_size,
                                        CENTROID_WINDOW_MIN,
                                        CENTROID_WINDOW_MAX);

  /* clarity_window_w/h 只影响 web 端清晰度曲线浮窗的显示尺寸，不参与算法本身。
   * 这里做范围限制，是为了防止窗口太小导致坐标轴文字重叠，或窗口太大盖住整个视频。
   * web 端绘制时还会根据当前 canvas 大小再做一次限制，保证小屏幕上不会越界。
   */
  if(centroid->clarity_window_w <= 0)
  {
    centroid->clarity_window_w = CENTROID_CLARITY_WINDOW_W_DEFAULT;
  }
  if(centroid->clarity_window_h <= 0)
  {
    centroid->clarity_window_h = CENTROID_CLARITY_WINDOW_H_DEFAULT;
  }
  centroid->clarity_window_w = svp_clamp_int(centroid->clarity_window_w,
                                             CENTROID_CLARITY_WINDOW_W_MIN,
                                             CENTROID_CLARITY_WINDOW_W_MAX);
  centroid->clarity_window_h = svp_clamp_int(centroid->clarity_window_h,
                                             CENTROID_CLARITY_WINDOW_H_MIN,
                                             CENTROID_CLARITY_WINDOW_H_MAX);

  /* ae_enable 表示 ISP 自动曝光开关：
   * 0 表示关闭 ISP 自动曝光/自动增益，并由质心线程按 max_gray 写手动曝光；
   * 1 表示不接管曝光，让 ISP 使用接管前的曝光配置。
   *
   * 手动曝光闭环使用当前帧 max_gray 做判断：
   * 目标最大灰度 = ae_saturation * ae_target_ratio，默认就是 255 * 0.8 = 204。
   * 线程层会在每帧检测完最大亮度后，按“目标灰度 / 当前最大灰度”的比例调曝光，
   * 因此当前亮度只要高于 0.8 饱和附近就会降曝光，不再等到真正饱和才动作。
   * 因为这些参数会直接写到 ISP 手动曝光接口，这里在 SET/GET 时统一兜底，
   * 避免 web 端误填 0、负数或上下限颠倒，导致板端曝光被写成不可用值。
   */
  centroid->ae_enable = centroid->ae_enable ? 1 : 0;
  if(centroid->ae_isp_pipe < 0)
  {
    centroid->ae_isp_pipe = 0;
  }
  centroid->ae_saturation = svp_clamp_int(centroid->ae_saturation, 1, 255);
  if(centroid->ae_target_ratio <= 0.0)
  {
    centroid->ae_target_ratio = 0.8;
  }
  if(centroid->ae_target_ratio < 0.1)
  {
    centroid->ae_target_ratio = 0.1;
  }
  if(centroid->ae_target_ratio > 0.95)
  {
    /* 目标值不允许贴近 1.0，给传感器噪声和曝光响应留一点余量，避免刚好顶到饱和。
     */
    centroid->ae_target_ratio = 0.95;
  }
  if(centroid->ae_min_exp_us <= 0)
  {
    centroid->ae_min_exp_us = 100;
  }
  if(centroid->ae_max_exp_us <= 0)
  {
    centroid->ae_max_exp_us = 33333;
  }
  if(centroid->ae_max_exp_us < centroid->ae_min_exp_us)
  {
    centroid->ae_max_exp_us = centroid->ae_min_exp_us;
  }
  if(centroid->ae_init_exp_us < 0)
  {
    centroid->ae_init_exp_us = 0;
  }
  if(centroid->ae_init_exp_us > 0)
  {
    centroid->ae_init_exp_us = svp_clamp_int(centroid->ae_init_exp_us,
                                             centroid->ae_min_exp_us,
                                             centroid->ae_max_exp_us);
  }
  if(centroid->ae_manual_gain < 1024)
  {
    centroid->ae_manual_gain = 1024;
  }
  if(centroid->ae_manual_gain > 262144)
  {
    centroid->ae_manual_gain = 262144;
  }
}

static void msg_func_cfg(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  if(req->set)
  {
    gsf_svp_t *svp = (gsf_svp_t*)req->data;
    
    svp_parm.svp = *svp;
    
    // change common svp alg;
    json_parm_save(svp_parm_path, &svp_parm);
      
    rsp->err  = 0;
    rsp->size = 0;
  }
  else
  {
    gsf_svp_t *svp = (gsf_svp_t*)rsp->data;
    
    memcpy(svp, &svp_parm.svp, sizeof(svp_parm.svp));
    
    rsp->err  = 0;
    rsp->size = sizeof(svp_parm.svp);
  }
}


static void msg_func_md(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  if(req->set)
  {
    gsf_svp_md_t *mdcfg = (gsf_svp_md_t*)req->data;
    
    // start/stop md_alg(ch);
    rsp->err  = 0;
    rsp->size = 0;
  }
  else
  {
    gsf_svp_md_t *mdcfg = (gsf_svp_md_t*)rsp->data;
    
    //get mdcfg(ch);
    
    rsp->err  = 0;
    rsp->size = sizeof(gsf_svp_md_t);
  }
}

static void msg_func_lpr(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  if(req->set)
  {
    gsf_svp_lpr_t *lprcfg = (gsf_svp_lpr_t*)req->data;
    
    // start/stop lpr_alg(ch);  
    rsp->err  = 0;
    rsp->size = 0;
  }
  else
  {
    gsf_svp_lpr_t *lprcfg = (gsf_svp_lpr_t*)rsp->data;
    
    //get lprcfg(ch);
    
    rsp->err  = 0;
    rsp->size = sizeof(gsf_svp_lpr_t);
  }
}

static void msg_func_yolo(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  if(req->set)
  {
    gsf_svp_yolo_t *yolocfg = (gsf_svp_yolo_t*)req->data;
    
    // start/stop yolo_alg(ch);  

    svp_parm.yolo = *yolocfg;
    json_parm_save(svp_parm_path, &svp_parm);
    
    if(1)//sync lines to app.exe;
    {    
      GSF_MSG_DEF(gsf_polygons_t, lines, 8*1024);
      lines->polygon_num = 1;
      lines->polygons[0].point_num = yolocfg->det_polygon.polygons[0].point_num;
      memcpy(lines->polygons[0].points, yolocfg->det_polygon.polygons[0].points, sizeof(lines->polygons[0].points));
      int ret = GSF_MSG_SENDTO(GSF_ID_APP_LINES, 0, SET, 0, sizeof(gsf_polygons_t), GSF_IPC_APP, 2000);
    }
    rsp->err  = 0;
    rsp->size = 0;
  }
  else
  {
    gsf_svp_yolo_t *yolocfg = (gsf_svp_yolo_t*)rsp->data;
    

    memcpy(yolocfg, &svp_parm.yolo, sizeof(svp_parm.yolo));
    //get yolocfg(ch);
    
    rsp->err  = 0;
    rsp->size = sizeof(gsf_svp_yolo_t);
  }
}

static void msg_func_centroid(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  gsf_svp_centroid_status_t *status = (gsf_svp_centroid_status_t*)rsp->data;
  extern int centroid_get_latest(gsf_svp_centroid_status_t *status);

  /* web 端只需要读取最近一次质心结果，不需要传入请求体。
   * 这里不直接重新抓帧，避免浏览器轮询频率影响 VPSS 抓帧和算法线程；
   * 质心线程负责持续更新共享状态，本接口只做一次很轻量的拷贝。
   */
  memset(status, 0, sizeof(*status));
  centroid_get_latest(status);

  rsp->err  = 0;
  rsp->size = sizeof(*status);
}

static void msg_func_centroid_cfg(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  if(req->set)
  {
    gsf_svp_centroid_t *centroid = (gsf_svp_centroid_t*)req->data;

    svp_centroid_param_normalize(centroid);

    svp_parm.centroid = *centroid;
    /* 质心提取相关配置全部集中在 GSF_ID_SVP_CENTROID_CFG：
     * 包括算法开关、VPSS 抓帧通道、阈值、串口输出、清晰度归一化参数和自定义曝光参数。
     * 保存到内存后，svp 主循环和质心线程会在下一轮读取到新值。
     */
    json_parm_save(svp_parm_path, &svp_parm);

    rsp->err  = 0;
    rsp->size = 0;
  }
  else
  {
    gsf_svp_centroid_t *centroid = (gsf_svp_centroid_t*)rsp->data;

    svp_centroid_param_normalize(&svp_parm.centroid);
    memcpy(centroid, &svp_parm.centroid, sizeof(svp_parm.centroid));

    rsp->err  = 0;
    rsp->size = sizeof(gsf_svp_centroid_t);
  }
}


static void msg_func_face(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
  int ret = 0;
  rsp->size = 0;
  rsp->err = 0;
  
  if(strlen(req->data))
  {
    extern int person_face_add(char* filename);
    
    char oldname[256] = {0};
    char newname[256] = {0};
    strncpy(oldname, req->data, sizeof(oldname)-1);
    sprintf(newname, "/app/face/list/%s", basename(oldname));
    
    char cmd[256] = {0};
    snprintf(cmd, sizeof(cmd), "mv %s %s", req->data, newname);
    system(cmd);

    printf("mv oldname[%s] => newname:[%s]\n", req->data, newname);
    rsp->err = person_face_add(newname);
    if(rsp->err)
    {
      //unlink(newname);
    }
  }
}




static msg_func_t *msg_func[GSF_ID_SVP_END] = {
    [GSF_ID_SVP_CFG]    = msg_func_cfg,
    [GSF_ID_SVP_MD]     = msg_func_md,
    [GSF_ID_SVP_LPR]    = msg_func_lpr,
    [GSF_ID_SVP_YOLO]   = msg_func_yolo,
    [GSF_ID_SVP_FACE]   = msg_func_face,
    [GSF_ID_SVP_CENTROID] = msg_func_centroid,
    [GSF_ID_SVP_CENTROID_CFG] = msg_func_centroid_cfg,
 };


int msg_func_proc(gsf_msg_t *req, int isize, gsf_msg_t *rsp, int *osize)
{
    if(req->id < 0 || req->id >= GSF_ID_SVP_END)
    {
        return FALSE;
    }
    
    if(msg_func[req->id] == NULL)
    {
        return FALSE;
    }   
    
    msg_func[req->id](req, isize, rsp, osize);
    
    return TRUE;
}
