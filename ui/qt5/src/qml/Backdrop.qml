// 全窗口背景：近黑渐变底 + 几团极低透明度的柔光。
//
// 【与上一版的区别】上一版是紫蓝高饱和渐变 + alpha 0.43~0.75 的彩色光斑。
// 现在照鹰角启动器的近黑配色，光斑 alpha 全部压到 0.10 以下（见 Theme.blobs）。
// 光斑在这一版里不再承担"提供颜色观感"的职责，只负责给毛玻璃提供亮度梯度 ——
// 纯平底色模糊前后完全一样，那毛玻璃就白做了。
//
// 光斑用单个 Canvas 一次画完，而不是堆若干 RadialGradient 元素 ——
// Qt5 的 QtQuick 基础库没有径向渐变元素（那在 QtGraphicalEffects 里，
// 且每个都会拖一个 ShaderEffect）。
//
// 【Canvas 固定逻辑尺寸，绝不跟随窗口】上一版 Canvas anchors.fill: parent
// 且 onWidthChanged/onHeightChanged 里 requestPaint()，注释写着"只有尺寸
// 变了才需要重画"——但 live resize 期间尺寸每帧都在变，等于每帧在主线程
// （Cooperative 策略）跑一遍全窗 JS 径向渐变重绘，拖拽缩放窗口肉眼可见卡顿。
// 现在画布恒为 CANVAS_W x CANVAS_H、只画一次，外面套一层 cover 式等比缩放
// （scale = max(容器宽/1600, 容器高/900)，中心对齐），resize 时只有 scale
// 这个数值在变——纹理一个字节都不重画。
//
// 【取舍】光斑坐标本来就是相对值（cx*width 等），r 原来按窗口对角线取，
// 现在固定用画布对角线：窗口宽高比偏离 16:9 越多，构图与旧版偏差越大
// （光斑相对窗口的位置/大小会有百分之几到百分之十几的差别）。这些光斑
// alpha 全部 < 0.10、只给毛玻璃提供亮度梯度，且本身是软渐变、纹理放大
// 到 2x+ 也不出像素感，构图微差不可感知——换掉每帧全窗重绘，值得。
//
// 【为什么没有噪点纹理】鹰角真实背景在主视觉图上叠了一层 pattern3.png
// （405x100，alpha 峰值只到 4/255 ≈ 0.016）。我们没有照做，原因是成本与
// 收益不成比例：
//   1. 面板内部：20px 模糊会直接抹掉高频细节，1.6% 的颗粒在玻璃后面
//      根本不可见 —— 模糊的定义就是消除高频。
//   2. 面板外部：确实能看见一点，但要么是 3px 点阵下的 ~7.6 万次
//      rect fill，要么是 68 万像素的 ImageData JS 循环，且每次 resize
//      都要重来一遍，会造成可感知的卡顿。
// 为一个 1.6% 不透明度的效果付这个代价不值得。如果以后真要加，
// 正确做法是 ShaderEffect 里做 hash noise（GLSL 1.10 写法，桌面 GL 和
// GL ES/ANGLE 都能跑），而不是 Canvas 逐点画。

import QtQuick 2.15
import App 1.0

Rectangle {
    id: root

    gradient: Gradient {
        GradientStop { position: 0.0; color: Theme.bgTop }
        GradientStop { position: 1.0; color: Theme.bgBottom }
    }

    // 固定画布的逻辑尺寸。16:9 是最常见的桌面比例，光斑构图按它设计；
    // 窗口更大/更小/比例不同时走 cover 缩放，画布本身恒定不变。
    readonly property int canvasW: 1600
    readonly property int canvasH: 900

    // cover 式缩放容器：scale 绕中心（transformOrigin 默认 Center），
    // 又是 centerIn 锚定，所以放大/缩小都保持居中裁切，恒有 >= 容器的覆盖面。
    Item {
        anchors.centerIn: parent
        width: root.canvasW
        height: root.canvasH
        // 每帧只算一次乘除的绑定，比 Canvas 重画便宜几个数量级。
        scale: Math.max(root.width / root.canvasW, root.height / root.canvasH)

        Canvas {
            anchors.fill: parent
            // 画布尺寸恒定，这个 requestPaint 只会在创建后发生一次。
            // （ Cooperative：主线程排队执行，一次性成本无所谓。）
            renderStrategy: Canvas.Cooperative

            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();

                // 对角线取固定画布的，不再跟随窗口（取舍见文件头说明）。
                const diag = Math.sqrt(width * width + height * height);

                for (let i = 0; i < Theme.blobs.length; ++i) {
                    const b = Theme.blobs[i];
                    const cx = b.cx * width;
                    const cy = b.cy * height;
                    const r  = b.r * diag * 0.5;

                    const g = ctx.createRadialGradient(cx, cy, 0, cx, cy, r);
                    // 【别改成 Qt.color()】Qt5 的 QML 全局对象没有这个函数
                    // （Qt6 才加入），调它就是 "Property 'color' is not a function"。
                    // 这里也不需要任何 QML 颜色 API：canvas 的 2D 上下文原生吃
                    // CSS 颜色串，把 #rrggbb 拆开拼 rgba() 同时解决透明度注入。
                    // Theme.blobs 里的 color 是我们自己写死的 #rrggbb 格式。
                    const hex = b.color.slice(1);
                    const rr = parseInt(hex.slice(0, 2), 16);
                    const gg = parseInt(hex.slice(2, 4), 16);
                    const bb = parseInt(hex.slice(4, 6), 16);
                    g.addColorStop(0.0, "rgba(" + rr + "," + gg + "," + bb + "," + b.alpha + ")");
                    g.addColorStop(1.0, "rgba(" + rr + "," + gg + "," + bb + ",0)");

                    ctx.fillStyle = g;
                    ctx.beginPath();
                    ctx.ellipse(cx - r, cy - r, r * 2, r * 2);
                    ctx.fill();
                }
            }
        }
    }
}
