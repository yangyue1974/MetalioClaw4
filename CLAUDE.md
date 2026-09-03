# 给接手的 Claude

这是 `CloudZao/MetalioClaw4`(Metalio Claw4 厂商固件,xiaozhi fork)在 yangyue1974 名下的 fork。
方向:**厂商 App 基本全扔,Claw 不要,留硬件层 + 小智对话 + 配网 + 相机 + 设置 + 蓝牙 + SD 卡 + GPS,写自己的一批 App。**

产品文档、设备事实、坑、留/删清单都在另一个仓 `yangyue1974/ESPTTEST`:
`docs/p4.md`(硬件)、`docs/metalio-firmware-map.md`(App 框架和删减清单)、`docs/pitfalls.md`。先读那边。

## 构建

- **ESP-IDF 必须 v5.5.4**,`sdkconfig` 入库,`idf.py build` 直接编,不要 set-target。
- 一个终端只 source 一个 IDF 版本。ESPTTEST 里的 `tools/build-metalio.sh` 会在干净环境里编 + 烧。
- 分区 `partitions/v1/32m_single.csv`(ota_0 / ota_1 各 10M + resources 8M + coredump)。**已不是原厂布局**,回原厂只能整块写回备份。

## 加 App

`main/display/screen/<name>_screen/` 一个类 `static lv_obj_t* Create()`,图标 `main/xingzhi-assets/ic_app_home_theme{1..4}_<suffix>.png`,
`home_screen.cc` 的 `kApps[]` 加一行,`main/CMakeLists.txt` 的 SOURCES 和 INCLUDE_DIRS 各加一处。样板见 ESPTTEST 文档。

## 跟上游

`upstream` = `https://github.com/CloudZao/MetalioClaw4.git`。我们的改动都在 main 上叠在厂商 commit 之上。
