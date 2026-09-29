# Qt5 QProcess 在 Windows 上安全托管完整进程树

## 现象

`QProcess::kill()` 只终止根进程；Python、Blender 或工具链启动的后代仍会运行。Release GUI 又没有共享控制台，不能可靠靠 `CTRL_C_EVENT` 清理。

## 根因

普通启动后再加入 Job Object 存在竞态：根进程可在分配前创建逃逸子进程。仅结束根进程也不会递归结束 Windows 进程树。

## 解决

以 `CREATE_SUSPENDED` 创建根进程，先加入带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object，再恢复主线程。分配失败时绝不恢复用户代码；取消使用 `TerminateJobObject`，关闭句柄作为整树清理后备路径。

## 验证

确定性测试覆盖创建/配置/分配 Job 失败均 fail-closed，并用 detached 后代哨兵验证取消、根进程自然退出和 runner 析构后都无存活后代。
