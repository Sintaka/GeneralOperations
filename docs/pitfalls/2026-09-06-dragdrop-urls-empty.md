# 2026-09-06 DOpus 拖入文件：URL 跨 QML→C++ 桥后丢失

## 现象

从 Directory Opus 拖文件进 QML DropArea：悬停显示"不能拖入"，松手报"没有拖入任何文件"。
而 QML 侧 `JSON.stringify(drag.urls)` 显示 URL 数组完好（标准 `file:///` 三斜杠）。
资源管理器拖入一直正常，仅 DOpus 来源失败。

## 根因（两层）

1. **主因：QML → C++ 的数组跨桥转换丢元素。** `drag.urls`（url 包装类型的数组）
   直接传给 Q_INVOKABLE 的 `QVariantList` 形参，C++ 侧收到**空列表**
   （QML 侧 stringify 有元素，C++ 侧 `urls.size() == 0`）。
   同样的调用改传字面量字符串数组（`String(urls[i])` 拼出来的）就一切正常。
   结论：url 包装类型数组过 `QVariantList` 形参的转换不可靠，字符串可靠。
2. 次因：DOpus 悬停阶段是 OLE 延迟渲染，数据对象只支持格式查询（`hasUrls`），
   `GetData` 要等松手才成功。悬停时校验会误判，需给中性反馈、把裁决
   推迟到 onDropped（微软对 IDropTarget::DragEnter 的建议就是只查格式）。

相关坑：`QUrl("D:/x")` 会把 `D` 解析成 scheme；`QUrl("file://D:/x")` 盘符落
host 位，且解析结果不稳定（实测出现过 `isLocalFile()==true` 但路径是 `//D:/x`
的组合）。URL→路径的归一化必须在原始字符串上做手术，用存在性校验仲裁。

## 解决

- QML 跨桥前先序列化成纯字符串数组：`String(urls[i])` 逐个 push。
- 悬停校验宽松化：validateDrop 对"有 urls 但转不出路径"返回 `uncertain`
  （ok=true、files 空），UI 给中性反馈"松手执行"；真实裁决在 onDropped。
- C++ `toLocalPath` 候选列表 + `QFileInfo::exists` 仲裁：
  标准 URL / 裸路径 / 剥 scheme / 盘符冒号被吞 / UNC 五种形态。

## 验证

`--dropdebug` 双通道日志（平台层 QMimeData + DropZone 校验链路）确认：
rawUrls 完好 → 修复后 validateDrop `urls.size=1`、files 非空 → 脚本成功执行。

## 诊断工具

`GeneralOperationsLauncher.exe --dropdebug`：写 exe 旁的 `dropdebug.log`，
平台层事件过滤器 + DropZone 校验链路双通道记录；诊断完删除日志文件。
