# FileEncryptor 宣传视频（Remotion）

用 [Remotion](https://remotion.dev) 做的宣传片：既能讲功能，也有**实际使用演示**
（拖放加密、密码框输入、进度、加密完成提示、安全删除、命令行）。

## 生成视频

```bash
npm install
npx remotion studio        # 预览、逐帧调整（浏览器里实时看）
npx remotion render Promo out/promo.mp4    # 渲染
npx remotion still Promo out/cover.png --frame=60   # 导出封面
```

渲染会调用**系统自带的 Edge**（见 `remotion.config.ts`），
不需要下载 Chrome Headless Shell——国内网络下那一步经常失败。

## 分镜

| 时间 | 内容 |
|---|---|
| 0:00 | 开场：Logo + 主张 |
| 0:04 | 痛点：网盘明文、送修、设备丢失 |
| 0:09 | **演示 A**：拖入文件 → 设置密码（含确认、显示密码、诱饵文件）→ 进度条 → 加密完成 |
| 0:35 | 可否认加密：真/假密码双槽对称 |
| 0:46 | **演示 B**：安全删除，三次覆写进度 + 四阶段说明 |
| 0:54 | **演示 C**：命令行 `enc.exe ...` 逐行输出 |
| 1:04 | 核心能力网格 |
| 1:12 | 结尾：下载入口与地址 |

时长与逐幕帧数在 `src/Promo.tsx` 末尾的 `D` 常量里调整。

## 改文案 / 配色

- 配色：`src/Promo.tsx` 顶部的 `C`
- App 界面复刻：`WinFrame` / `AppButton` / `DemoEncrypt` 里的 `PasswordDialog` 片段
- 字体：`FONT` / `MONO`（默认用系统 Segoe UI / 微软雅黑，无需联网加载）

## 为什么不用录屏

录屏会带窗口边框、鼠标、系统弹窗（实测 Edge 还会弹"同步浏览数据"）。
这里所有画面都是用 React 画的，**确定性输出**，1080p 干净、可反复重渲。
