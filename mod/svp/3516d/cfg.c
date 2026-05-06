#include "cfg.h"

char svp_parm_path[128] = {0};

gsf_svp_parm_t svp_parm = {
  .svp = {
    .md_alg = 0,
    .lpr_alg = 0,
    .yolo_alg = 0,
    .centroid_alg = 0,
  },
  .centroid = {
    // 默认抓取主码流算法通道；如果现场 VPSS 绑定不同，只改 svp_parm.json 即可。
    .vpss_grp = 0,
    .vpss_chn = 1,

    // 默认按高亮光斑处理：220 适合激光/强反光点；设为 0 可切换为 OTSU 自动阈值。
    .threshold = 220,
    .min_area = 4,

    // 默认 10Hz 输出坐标，既能实时跟踪，又不会让串口和 VPSS 抓帧压力过大。
    .interval_ms = 100,
    .baudrate = 115200,
    .print_no_spot = 0,
    .uart_dev = "/dev/ttyAMA0",
  },
};

int json_parm_load(char *filename, gsf_svp_parm_t *cfg)
{
  if(access(filename, 0)) return -1;
  FILE *f=fopen(filename,"rb");fseek(f,0,SEEK_END);long len=ftell(f);fseek(f,0,SEEK_SET);
	char *data=(char*)malloc(len+1);fread(data,1,len,f);fclose(f);
  cJSON* json = cJSON_Parse(data);
  free(data);
  if (json)
	{
    sjb_bind_gsf_svp_parm_t(json, 1, cfg, 0, 0);
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
