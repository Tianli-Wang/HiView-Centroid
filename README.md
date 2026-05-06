# 光斑质心提取与串口输出说明

本文档说明 `HIVIEW-master` 中新增的光斑质心提取功能如何编译、打包、升级到 Hi3516D/Hi3516DV300 开发板，以及如何查看串口输出。

## 功能概述

本功能集成在 `svp.exe` 算法进程中，运行流程如下：

1. 从海思 VPSS 通道抓取实时视频帧。
2. 使用 OpenCV 将图像转换为光强图。
3. 根据阈值提取高亮光斑区域。
4. 选择面积最大的亮斑区域。
5. 计算该亮斑区域的灰度加权质心坐标。
6. 通过串口按行输出质心坐标。

默认输出格式如下：

```text
CENTROID,valid,chn,x,y,width,height,area,pts
```

示例：

```text
CENTROID,1,1,1650.33,419.97,1920,1080,47372,53648630
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `CENTROID` | 固定帧头，表示这是一条质心检测结果 |
| `valid` | 检测结果是否有效，`1` 表示检测到光斑，`0` 表示未检测到 |
| `chn` | VPSS 通道号，默认是 `1` |
| `x` | 光斑质心横坐标，单位为像素 |
| `y` | 光斑质心纵坐标，单位为像素 |
| `width` | 当前检测图像宽度 |
| `height` | 当前检测图像高度 |
| `area` | 被判定为光斑的像素面积 |
| `pts` | 视频帧时间戳，来自海思视频帧结构 |

图像坐标系以左上角为原点：

```text
x 向右增大
y 向下增大
```

## 相关代码文件

主要修改文件如下：

```text
mod/svp/3516d/nnie/sample/centroid_sample.h
mod/svp/3516d/nnie/sample/centroid_sample.cpp
mod/svp/3516d/centroid.c
mod/svp/3516d/svp.c
mod/svp/3516d/cfg.c
mod/svp/inc/sjb_svp.ih
ins.sh
```

各文件职责：

| 文件 | 作用 |
| --- | --- |
| `centroid_sample.h` | 质心检测算法 C 接口声明 |
| `centroid_sample.cpp` | VPSS 抓帧、光斑检测、质心计算 |
| `centroid.c` | 质心检测线程、串口初始化、串口输出 |
| `svp.c` | 根据 `centroid_alg` 启动或停止质心线程 |
| `cfg.c` | 质心功能默认配置 |
| `sjb_svp.ih` | SVP JSON 配置结构扩展 |
| `ins.sh` | 16d 升级包打包输出路径调整 |

## 编译环境

需要在 Linux 或 WSL 中安装海思交叉编译链：

```bash
arm-himix200-linux-gcc
arm-himix200-linux-ar
arm-himix200-linux-strip
```

确认工具链可用：

```bash
which arm-himix200-linux-gcc
arm-himix200-linux-gcc -v
```

## 编译与打包

进入工程目录：

```bash
cd /mnt/e/hisi/HIVIEW-master
```

加载 3516d 编译环境：

```bash
source build/3516d
```

编译：

```bash
make
```

如果只想重新编译 SVP 模块：

```bash
make -C mod/svp DEPS=
```

打包 16d 升级包：

```bash
./ins.sh 16d
```

当前脚本会将升级包输出到：

```text
E:\hisi\HIVIEW-master\upg\cam16d.upg
```

在 WSL 中对应路径为：

```bash
/mnt/e/hisi/HIVIEW-master/upg/cam16d.upg
```

## 升级到板端

电脑和开发板需要在同一网段。例如：

```text
Windows 主机: 192.168.0.1
开发板:       192.168.0.2
```

确认网络连通：

```bash
ping 192.168.0.2
```

在 Windows 浏览器打开：

```text
http://192.168.0.2
```

进入升级页面，上传：

```text
E:\hisi\HIVIEW-master\upg\cam16d.upg
```

升级完成后建议重启开发板。

## 启用质心功能

升级后，打开开发板网页：

```text
http://192.168.0.2
```

进入：

```text
Config -> GSF_ID_SVP_CFG
```

点击 `GET`，将返回数据中的：

```json
"centroid_alg": 0
```

改为：

```json
"centroid_alg": 1
```

然后点击 `SET`。

`centroid_alg` 含义：

| 值 | 含义 |
| --- | --- |
| `0` | 关闭光斑质心检测 |
| `1` | 开启光斑质心检测并通过串口输出 |

## 配置参数

质心功能配置结构如下：

```json
{
  "centroid": {
    "vpss_grp": 0,
    "vpss_chn": 1,
    "threshold": 220,
    "min_area": 4,
    "interval_ms": 100,
    "baudrate": 115200,
    "print_no_spot": 0,
    "uart_dev": "/dev/ttyAMA0"
  }
}
```

参数说明：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `vpss_grp` | `0` | VPSS 组号 |
| `vpss_chn` | `1` | VPSS 通道号 |
| `threshold` | `220` | 光斑二值化阈值，`1~254` 为固定阈值，其它值使用 OTSU 自动阈值 |
| `min_area` | `4` | 最小亮斑面积，小于该值认为是噪声 |
| `interval_ms` | `100` | 串口输出间隔，单位毫秒 |
| `baudrate` | `115200` | 串口波特率 |
| `print_no_spot` | `0` | 是否输出未检测到光斑的帧 |
| `uart_dev` | `/dev/ttyAMA0` | 板端串口设备 |

如果想调试未检测到光斑的情况，可以临时设置：

```json
"print_no_spot": 1
```

此时未检测到光斑时也会输出：

```text
CENTROID,0,1,-1,-1,1920,1080,0,pts
```

## 串口查看

默认板端输出串口：

```text
/dev/ttyAMA0
```

默认串口参数：

```text
115200 8N1
```

PC 端使用串口助手、MobaXterm、Xshell、PuTTY 等工具打开对应 USB-TTL 串口即可。

如果看不到输出，可以先在板端执行：

```bash
echo "CENTROID_TEST" > /dev/ttyAMA0
```

如果 PC 串口工具仍看不到 `CENTROID_TEST`，说明当前查看的串口不是 `/dev/ttyAMA0` 对应的物理串口，或者接线、波特率不正确。

查看板端存在的串口设备：

```bash
ls -l /dev/ttyAMA* /dev/ttyS* /dev/ttyUSB* /dev/usbtty* 2>/dev/null
```

如果实际使用的串口不是 `/dev/ttyAMA0`，需要修改：

```json
"uart_dev": "/dev/实际串口设备"
```

## 常见问题

### 1. 没有任何 `CENTROID` 输出

检查 `centroid_alg` 是否已经设置为 `1`：

```text
Config -> GSF_ID_SVP_CFG -> GET
```

如果还是 `0`，改成 `1` 后点击 `SET`。

检查 `svp.exe` 是否运行：

```bash
ps | grep svp
```

### 2. 串口没有输出，但网页配置已经打开

先确认板端串口是否能手动输出：

```bash
echo "CENTROID_TEST" > /dev/ttyAMA0
```

如果 PC 端看不到，优先检查：

```text
串口设备名
USB-TTL 接线
波特率
GND 是否共地
TX/RX 是否交叉
```

### 3. 出现 `mmz_userdev: ioctl_mmb_alloc failed`

早期版本每帧重新申请 VPSS 转换后的 MMZ 图像缓存，长时间运行会耗尽 MMZ。

当前版本已经修复：`centroid_sample.cpp` 中使用持久化 `cv::Mat` 缓存复用 MMZ 内存。

如果板端已经出现该报错，升级修复版本后建议重启开发板，清理旧进程占用的 MMZ 状态。

### 4. 坐标在两个区域之间跳动

说明画面中存在多个高亮区域，且面积接近。当前算法默认取面积最大的亮斑区域。

可尝试：

```text
提高 threshold
增大 min_area
调整光路或背景，减少反光区域
后续增加 ROI 限定，只检测指定区域
```

### 5. `area` 很大

`area` 是二值化后亮斑区域像素数量。如果 `area` 很大，可能说明：

```text
阈值 threshold 太低
背景有大面积过曝
画面里有强反光
光斑散焦过大
```

可以尝试把阈值提高，例如：

```json
"threshold": 240
```

## 当前输出包路径

当前 16d 升级包固定输出到：

```text
E:\hisi\HIVIEW-master\upg\cam16d.upg
```

对应 WSL 路径：

```bash
/mnt/e/hisi/HIVIEW-master/upg/cam16d.upg
```
