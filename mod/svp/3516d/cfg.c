#include "cfg.h"

char svp_parm_path[128] = {0};

gsf_svp_parm_t svp_parm = {
  .svp = {
    .md_alg = 0,
    .lpr_alg = 0,
    .yolo_alg = 0,
  },
  .centroid = {
    // 质心功能使用独立的 GSF_ID_SVP_CENTROID_CFG 配置类，不再挤在 GSF_ID_SVP_CFG 中。
    .centroid_alg = 0,

    // 默认抓取主码流算法通道；如果现场 VPSS 绑定不同，只改 svp_parm.json 即可。
    .vpss_grp = 0,
    .vpss_chn = 1,

    // 默认按高亮光斑处理：220 适合激光/强反光点；设为 0 可切换为 OTSU 自动阈值。
    .threshold = 220,
    .min_area = 4,
    // 质心只在最亮点附近的小窗口内计算，默认 64 像素可覆盖常见光斑核心并隔离远处噪声。
    .window_size = 64,

    // 默认 10Hz 输出坐标，既能实时跟踪，又不会让串口和 VPSS 抓帧压力过大。
    .interval_ms = 100,
    .baudrate = 115200,
    .print_no_spot = 0,

    // 清晰度函数使用 max_gray / clarity_gain / clarity_exposure / half_area，SET 后下一帧实时生效。
    .clarity_gain = 1.0,
    .clarity_exposure = 1.0,
    // web 实时预览中的清晰度曲线浮窗尺寸，现场可按屏幕大小在 CENTROID_CFG 中手动调整。
    .clarity_window_w = 360,
    .clarity_window_h = 150,

    // 默认关闭 ISP 自动曝光；质心线程会把曝光和增益切到手动模式，再把 max_gray 控制在饱和值的 0.8 倍附近。
    .ae_enable = 0,
    .ae_isp_pipe = 0,
    .ae_saturation = 255,
    .ae_target_ratio = 0.8,
    .ae_min_exp_us = 100,
    .ae_max_exp_us = 33333,
    .ae_init_exp_us = 0,
    .ae_manual_gain = 1024,
    .uart_dev = "/dev/ttyAMA0",
  },
};

static int json_number_int(cJSON *json, const char *name, int *value)
{
  cJSON *item = json ? cJSON_GetObjectItem(json, name) : NULL;

  if(item && item->type == cJSON_Number)
  {
    *value = item->valueint;
    return 0;
  }

  return -1;
}

static int json_number_double(cJSON *json, const char *name, double *value)
{
  cJSON *item = json ? cJSON_GetObjectItem(json, name) : NULL;

  if(item && item->type == cJSON_Number)
  {
    *value = item->valuedouble;
    return 0;
  }

  return -1;
}

static void json_parm_migrate_old_centroid_cfg(cJSON *json, gsf_svp_parm_t *cfg)
{
  cJSON *svp = json ? cJSON_GetObjectItem(json, "svp") : NULL;

  /* 早期版本把 centroid_alg、clarity_gain、clarity_exposure 临时放在 svp 段。
   * 现在质心提取已经有独立的 GSF_ID_SVP_CENTROID_CFG 配置类，加载旧 svp_parm.json
   * 时把这些旧字段迁移到 centroid 段，避免升级后用户原来的开关和清晰度参数丢失。
   */
  if(!svp)
  {
    return;
  }

  json_number_int(svp, "centroid_alg", &cfg->centroid.centroid_alg);
  json_number_double(svp, "clarity_gain", &cfg->centroid.clarity_gain);
  json_number_double(svp, "clarity_exposure", &cfg->centroid.clarity_exposure);
}

int json_parm_load(char *filename, gsf_svp_parm_t *cfg)
{
  if(access(filename, 0)) return -1;
  FILE *f=fopen(filename,"rb");fseek(f,0,SEEK_END);long len=ftell(f);fseek(f,0,SEEK_SET);
	char *data=(char*)malloc(len+1);fread(data,1,len,f);fclose(f);
  data[len] = '\0';
  cJSON* json = cJSON_Parse(data);
  free(data);
  if (json)
	{
    sjb_bind_gsf_svp_parm_t(json, 1, cfg, 0, 0);
    json_parm_migrate_old_centroid_cfg(json, cfg);
    cJSON_Delete(json);
	}
  return 0;
}

int json_parm_save(char *filename, gsf_svp_parm_t *cfg)
{
  FILE *f=fopen(filename,"wb");
  cJSON* out = cJSON_CreateObject();
  sjb_bind_gsf_svp_parm_t(out, 0, cfg, 0, 0);
  char* print = cJSON_Print(out);
  if(print)
  {
	  fprintf(f, "%s", print);
	  fflush(f);fdatasync(fileno(f));fclose(f);
	  free(print);
  }
  cJSON_Delete(out);
  return 0;
}
