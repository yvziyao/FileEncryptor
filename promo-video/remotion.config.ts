import { Config } from '@remotion/cli/config';

// 用系统自带的 Edge 渲染，避免 Remotion 首次运行时下载 Chrome Headless Shell
// （国内网络下载 googleapis 的 Chromium 经常失败）。
Config.setBrowserExecutable('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe');
Config.setVideoImageFormat('jpeg');
Config.setOverwriteOutput(true);
Config.setCodec('h264');
Config.setCrf(20);
