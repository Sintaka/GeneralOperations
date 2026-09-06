// 全窗口背景：近黑渐变底 + 几团极低透明度的柔光。
//
// 【与上一版的区别】上一版是紫蓝高饱和渐变 + alpha 0.43~0.75 的彩色光斑。
// 现在照鹰角启动器的近黑配色，光斑 alpha 全部压到 0.10 以下（见 Theme.blobs）。
// 光斑在这一版里不再承担"提供颜色观感"的职责，只负责给毛玻璃提供亮度梯度 ——
// 纯平底色模糊前后完全一样，那毛玻璃就白做了。
//
// 光斑用单个 Canvas 一次画完，而不是堆若干 RadialGradient 元素 ——
// Qt5 的 QtQuick 基础库没有径向渐变元素（那在 QtGraphicalEffects 里，
// 且每个都会拖一个 ShaderEffect），Canvas 只在尺寸变化时重绘一次，更省。
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

    Canvas {
        anchors.fill: parent
        // 背景是静态的，只有尺寸变了才需要重画。
        renderStrategy: Canvas.Cooperative

        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();

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

        onWidthChanged:  requestPaint()
        onHeightChanged: requestPaint()
    }
}
