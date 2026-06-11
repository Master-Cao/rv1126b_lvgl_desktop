# rv1126b_lvgl_desktop

在 RV1126B 上运行的 LVGL 桌面程序，将 OCR、目标检测、实例分割、人脸识别、车牌识别等算法封装为页面式嵌入式应用。

## 功能

- LVGL 桌面壳：状态栏、应用卡片、页面切换
- 算法应用：OCR、YOLOv8 检测/分割、人脸、车牌、毛发检测等
- 相机预览与结果展示
- 板端一键安装脚本

## 目录结构

```
├── lvgl_desktop/     # 桌面 UI 与应用页面
├── services/         # 相机服务与算法实现
├── models/           # RKNN 模型（需自行放置，见 models/README.md）
├── scripts/          # 板端安装与启动脚本
├── sys/              # 系统工具（GPIO、CPU、trace 等）
├── lvgl8/            # LVGL 移植层
├── common/           # 通用工具
├── build.sh          # 交叉编译脚本
└── CMakeLists.txt
```

## 编译

默认使用 `/opt/atk-dlrv1126b-toolchain` 工具链：

```bash
./build.sh          # Release 增量构建
./build.sh -c       # 清理后重新构建
./build.sh -d       # Debug 构建
./build.sh -j 8     # 8 路并行
```

## 板端部署

将编译产物与模型文件部署到板端，详见 `scripts/install_board.sh` 与 `models/README.md`。

## License

MIT
