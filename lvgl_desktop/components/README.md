# components

UI 组件按用途分目录管理：

```text
components/
├── common/          # 全应用通用（如相机预览）
├── ocr/             # OCR 专用组件
├── detect/          # 目标检测专用组件
└── camera/          # 相机设置专用组件
```

规则：

- 两个及以上应用会用的控件 → 放 `common/`
- 仅某一个应用使用的控件 → 放对应应用子目录
- 应用页面逻辑在 `apps/`，不在此目录重复实现
