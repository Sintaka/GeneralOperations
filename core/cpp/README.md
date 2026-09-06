# core/cpp —— C++ 内核占位（go_core_cpp）

## 定位

本目录是"脚本启动器" monorepo 里 **未来并行 / 高性能 C++ 后端的槽位**。

monorepo 的分工：`ui/` 是多前端（不认识具体内核实现），`core/` 是内核层。
`core/python` 已有脚本内核；`core/cpp` 目前只是占位——放了一两个简单、真实的
纯函数和自测，用来证明 **这个槽位存在、能独立 configure / build / test**，
不是完整后端。等性能需求真正到来（EXR 批量翻转并行化等），后端从这里生长，
前端的接入方式不变。

硬性约定：**零第三方依赖、零 Qt 头**。core 层不认识 UI；Qt 相关包装永远住在
`ui/` 侧。注释（中文）只解释"为什么"，实现保持最小。

## 当前内容

| 文件 | 函数 | 对应 python 内核脚本 |
|---|---|---|
| `include/go/core/flip.hpp` | `go::core::flip_horizontal_rgba8` | `img.FlipImage_Horizontal` |
| `include/go/core/udim.hpp` | `go::core::udim_mirror` | `img.Zbrush_UDIM_Correction`（数学核心） |

- `void flip_horizontal_rgba8(std::uint8_t* pixels, int width, int height,
  std::ptrdiff_t stride_bytes)`
  行主序 RGBA8 缓冲的水平翻转：原位、逐行独立、行序不变。`stride_bytes`
  必须 >= `width*4`（允许行尾填充），不足时拒绝——不动缓冲、不报错，
  参数报告是调用方的职责。python 版以文件为单位处理，C++ 版下沉为内存
  纯函数，未来按行分片即可并行。
- `int udim_mirror(int udim)`
  UDIM 行内水平镜像：`UDIM = 1001 + 10*row + col`（col 0..9），镜像取
  `col' = 9 - col`。这是 Zbrush 导出方向修正的数学核心。合法 UDIM 取标准
  网格 1001..2000（第 0 行 1001..1010，第 1 行 1011..1020……）；范围外
  原值返回。映射是对合：镜像两次回到原值（如 1001 <-> 1010、1005 <-> 1006、
  1011 <-> 1020）。python 版按实际导入的 tile 集合镜像；本函数先给标准网格
  上的纯函数，按集合镜像的封装留给正式后端。

`tests/go_core_cpp_tests.cpp` 是无框架自测（简单断言宏），覆盖奇/偶宽翻转、
带 stride 填充的翻转、多行首尾行不被误翻、stride 不足被拒绝、`udim_mirror`
已知值 / 对合性 / 非法输入原样返回。

## 如何运行测试

推荐：从仓库根按 preset 构建后（测试已在默认构建里），运行

```
ctest --test-dir build/<preset> --output-on-failure -R go_core_cpp
```

或直接跑测试可执行 `build/<preset>/core/cpp/go_core_cpp_tests(.exe)`，
成功输出 `go_core_cpp_tests: all ok`。

独立验证（不依赖仓库根工程；构建目录放在仓库外）：

```
cmake -S core/cpp -B <仓库外的临时目录> -G Ninja
cmake --build <仓库外的临时目录>
ctest --test-dir <仓库外的临时目录> --output-on-failure
```

单独 configure 会有一条关于缺少 `project()` 的 dev 警告——因为 project()
由根 CMakeLists 统一声明，这是有意为之，可忽略。

## 接入方式（未来前端如何链 go_core_cpp）

根 CMakeLists `add_subdirectory(core/cpp)` 之后，任何前端 target 直接：

```cmake
target_link_libraries(某前端 PRIVATE go_core_cpp)
```

即可获得 `include/go/core` 的头文件搜索路径（当前 target 是 INTERFACE 库：
头文件即实现）。占位期的演进路径已定：将来引入 `.cpp` 实现时把 target 从
`INTERFACE` 改为 `STATIC`，**target 名保持 `go_core_cpp` 不变**，依赖方的
链接写法一行不用改。
