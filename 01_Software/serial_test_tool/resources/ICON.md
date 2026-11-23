# 应用图标：双向链路

2026-09-20 用户选定：蓝色圆角底，白色右向发送箭头、青色左向接收箭头，尾部触点表达硬件连接。

- `app-icon.png`：透明 RGBA 原图，1254×1254，保留作为后续导出的源文件。
- `app-icon.ico`：16、20、24、32、40、48、64、128、256 像素，共9档；Qt窗口与Windows EXE使用同一份文件。
- `app.rc`：EXE原生图标资源；CMake声明ICO为编译依赖。
- `resources.qrc`：内置ICO；`main.cpp`在创建窗口前设置应用图标。

不需要用户另外复制图标文件，图标随程序一起内嵌。普通构建不需要Python或图像生成工具。

## 重新封装ICO

仅在修改PNG源图后执行；Python需要Pillow。在工程目录运行：

```powershell
python -c "from PIL import Image; im=Image.open('resources/app-icon.png'); im.save('resources/app-icon.ico',format='ICO',sizes=[(s,s) for s in [16,20,24,32,40,48,64,128,256]])"
```

## 生成来源

使用内置 imagegen，以上一轮用户已确认的概念图为编辑参考，未使用API/CLI回退。原图文件为 `exec-21e7dc8b-2634-4e82-a3a0-24f973b86bc2.png`。ICO仅做格式和尺寸封装。

最终图像生成提示词：

> Extract and finalize ONLY the large LEFT blue rounded-square application icon from the approved '串口测试工具 / 双向链路' concept presentation. Preserve the approved design exactly: cobalt-blue rounded-square tile with subtle vertical blue gradient, a bold white right-pointing arrow on top with two short white vertical contacts at its left tail, a bold pale-cyan left-pointing arrow below with two cyan vertical contacts at its right tail. Preserve proportions, silhouette, spacing and colors of this approved icon. Produce ONE perfectly square high-resolution PNG app icon asset, straight on, on a genuinely transparent alpha background, tile centered and occupying about 92% of canvas width and height, equal small transparent margins, smooth clean rounded corners. Remove the surrounding presentation board, all Chinese text, all miniature icons, all background panels, all shadows outside the blue tile. No glow, no vignette, no new design details, no lettering, no watermark. This is the final isolated transparent icon for encoding as a Windows ICO and Qt window icon, not a new concept.
