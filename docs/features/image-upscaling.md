# Real-ESRGAN 图片放大与 Pillow 保底

返回 [功能总索引](../feature-map.yaml)。

## 覆盖状态

截至 2026-09-29，放大脚本的工具查找、懒下载、SHA-256 校验、运行期缓存和 Pillow 保底路径已对照源码核对。真实 PNG 已验证 GitHub 下载及本机 Vulkan 失败时的非 AI 保底；其他 Vulkan 设备仍需分别运行验收。

## 入口与关键实现

- [core/python/scripts/Image/ReSize/img.RealESRGAN_Upscale.py](../../core/python/scripts/Image/ReSize/img.RealESRGAN_Upscale.py)：查找可用工具；缺失时报告下载进度，下载固定的官方 Windows zip，校验哈希后安全解压到用户 runtime 缓存；退出码为零但出现 Vulkan 错误或对非黑原图产出全黑图时，也用 Pillow LANCZOS 与适度锐化生成非 AI 结果。
- [tests/test_realesrgan_lazy.py](../../tests/test_realesrgan_lazy.py)：懒下载、缓存路径、安全校验与 Pillow fallback 的测试入口；本机若有忽略的 `.sandbox/fixtures/realesrgan-user-input.png`，还会用真实图片验证保底输出尺寸。
- [docs/RELEASE.md](../RELEASE.md)：Windows zip 不预置第三方模型工具；运行时路径与联网要求。

## 运行期路径

工具先兼容查找旧发行包里的 `tools/realesrgan/`，再查默认的
`%LOCALAPPDATA%\GeneralOperations\runtime\tools\realesrgan/`；设置
`GO_RUNTIME_HOME` 可以替换 runtime 根目录。两处均无可用 exe 时，脚本才在线获取
官方工具包，校验成功后缓存供后续调用。下载、校验、Vulkan 启动或 AI 输出不可用时，
脚本使用已声明的 Pillow 依赖进入非 AI 保底路径。
