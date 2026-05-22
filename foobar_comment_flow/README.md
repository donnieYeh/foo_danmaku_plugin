# FooBar Danmaku

为 foobar2000 音乐播放器开发的弹幕插件，从网易云音乐获取评论并以弹幕形式滚动显示。

## 功能特性

- 独立透明悬浮窗口显示弹幕，屏幕底部 200px 高度
- 自动获取当前播放歌曲的网易云音乐评论
- 播放状态监控，歌曲切换时自动更新评论
- 可调节透明度、滚动速度、弹幕数量
- **快捷键 Ctrl+Shift+D** 切换弹幕显示/隐藏

## 项目结构

```
foo_danmaku/
├── foo_danmaku.cpp         # 插件入口，注册组件
├── foo_danmaku.h
├── foo_danmaku.sln/vcxproj # Visual Studio 项目文件
├── ui/
│   ├── danmaku_ui.cpp/h    # UI 元素实现（透明悬浮窗口）
├── core/
│   ├── danmaku_engine.cpp/h   # 弹幕渲染引擎（GDI+）
│   ├── danmaku_engine_types.h  # 弹幕数据结构
│   └── playback_monitor.cpp/h # foobar2000 播放状态监控
└── netEase/
    ├── api.cpp/h          # 网易云音乐 API（搜索+获取评论）
    └── signature.cpp/h    # 签名算法
```

## 构建步骤

### 前提条件

1. **foobar2000 SDK**
   - 当前仓库的 `foo_danmaku\SDK\` 目录已按构建脚本引用 SDK。

2. **Visual Studio 2022**
   - 安装 C++ 桌面开发工作负载
   - 当前 `build.ps1` 默认使用 VS 2022 Build Tools + Windows 10 SDK 的 Win32 工具链。

### 编译

```powershell
cd foo_danmaku
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

构建成功后输出：

```text
foo_danmaku\build\foo_danmaku.dll
```

### 安装

1. 复制 `foo_danmaku\build\foo_danmaku.dll` 到 foobar2000 组件目录，例如：
   ```
   C:\Program Files\foobar2000\components\
   ```

2. 重启 foobar2000，在 UI 元素列表中找到 "FooBar Danmaku" 并添加到布局

3. 如果 foobar2000 报告组件位数不匹配，请确认安装的是 32 位 foobar2000。本脚本当前产物为 Win32 DLL。

## 使用方法

1. 播放音乐时，弹幕窗口自动获取并显示网易云音乐热门评论
2. 按 **Ctrl+Shift+D** 切换弹幕显示/隐藏
3. 高赞评论显示为红色，稍高显示为橙色，普通评论为白色

## 运行验收清单

1. foobar2000 启动后没有组件加载错误。
2. 在布局编辑 / UI Element 列表中可以看到并添加 `FooBar Danmaku`。
3. 播放一首带有 `%title%` 信息的歌曲，切歌时插件应尝试按“标题 + 艺术家”搜索网易云评论。
4. 评论获取成功后，屏幕底部出现透明悬浮弹幕区域，弹幕从右向左滚动。
5. 按 **Ctrl+Shift+D** 可隐藏/恢复弹幕。
6. 若没有弹幕，优先检查网络访问、歌曲匹配、以及 foobar2000 控制台中的错误信息。

## 技术实现

### 弹幕渲染
- 使用 GDI+ 绘制文字，横向滚动（从右到左）
- 多轨道随机分配，避免垂直重叠
- 双缓冲绘制减少闪烁

### 网易云音乐 API
- 歌曲搜索：`https://music.163.com/api/search/get`
- 评论获取：`https://music.163.com/api/v1/resource/comments/R_SO_4_{songId}`
- 自动提取歌曲名+艺术家名作为关键词搜索

## 已知限制

- 网易云音乐 API 需要签名验证，当前实现使用简化方法
- 部分歌曲可能匹配不到评论
- 弹幕与歌词时间轴同步功能尚在开发中
- 当前插件以透明顶层悬浮窗口显示弹幕，不嵌入 foobar2000 面板内部绘制。

## 许可证

MIT License