# windsurf — FooBar Danmaku

为 foobar2000 音乐播放器开发的弹幕插件，播放时从网易云音乐实时拉取评论，
以滚动弹幕形式叠加显示在屏幕上。

---

## 三层架构

项目分为三个独立层，**职责严格分离**：

```
┌─────────────────────────────────────────────────────┐
│  Layer 1 │ foo_danmaku.dll                          │
│          │ foobar2000 插件：UI 渲染、播放监控        │
│          │ 静态链接 ↓                                │
├─────────────────────────────────────────────────────┤
│  Layer 2 │ music_client.lib（静态库）                │
│          │ 客户端中枢：抽象接口、封面下载、DLL 派发  │
│          │ 运行时 LoadLibraryW ↓                     │
├─────────────────────────────────────────────────────┤
│  Layer 3 │ netease_client.dll（动态库）              │
│          │ 平台实现：网易云音乐 weapi 加密 + HTTP    │
└─────────────────────────────────────────────────────┘
```

Layer 3 通过统一的 `MusicProviderVTable`（C ABI vtable）接入，
未来接入其他平台（QQ 音乐、Spotify 等）只需新增一个 DLL，**Layer 1 / 2 不用改**。

---

## 打包与部署

### 为什么 `music_client.lib` 不在安装包里

`music_client.lib` 是**静态库**，编译期已完整链接进 `foo_danmaku.dll`，
运行时无需单独存在：

```
编译期（开发机）：

  music_client.lib  ──┐
  foobar2000 SDK   ──┤──► link.exe ──► foo_danmaku.dll
  foo_danmaku.cpp  ──┘

运行期（用户机器）：

  foobar2000.exe
    └── foo_danmaku.dll   ← music_client 代码已在其中
          └── LoadLibraryW("netease_client.dll")  ← 唯一需要单独存在的 DLL
```

安装包 `foo_danmaku-x.x.x.fb2k-component`（标准 zip 改后缀）内部结构：

```
x64/
  foo_danmaku.dll       ← 插件主体（含 music_client 代码）
  netease_client.dll    ← Layer 3 provider，与插件同目录
```

foobar2000 安装时将 `x64/` 内容解压到 `components/`，
两个 DLL 落在同一目录，满足 `LoadLibraryW` 的同目录查找。

---

## 仓库结构

```
windsurf/
├── music_client/                   # Layer 2 — 静态库
│   ├── include/
│   │   ├── music_client.h          # 公开 API（Layer 1 使用）
│   │   └── music_provider.h        # Provider 契约（Layer 3 实现）
│   ├── src/music_client.cpp
│   └── build.ps1
│
├── netease_client/                 # Layer 3 — 网易云 provider
│   ├── include/netease_client.h
│   ├── src/
│   │   ├── api.cpp / crypto.cpp / http.cpp / json.cpp
│   │   ├── netease_client.cpp
│   │   └── provider_entry.cpp      # 适配 MusicProviderVTable
│   ├── build.ps1
│   └── DESIGN.md                   # 内部实现详解
│
└── foobar_comment_flow/            # Layer 1 — foobar2000 插件
    ├── foo_danmaku/
    │   ├── foo_danmaku.cpp         # 插件入口
    │   ├── ui/danmaku_ui.cpp       # 透明悬浮窗 + 弹幕调度
    │   ├── core/danmaku_engine.cpp # DirectWrite 渲染
    │   ├── core/playback_monitor.cpp
    │   ├── SDK/                    # foobar2000 vendor SDK（不入库）
    │   └── build.ps1
    ├── pack.ps1                    # 打包为 .fb2k-component
    └── README.md                   # 插件功能说明
```

---

## 构建步骤

> 前提：Visual Studio 2022 Build Tools + Windows 10 SDK（x64）

```powershell
# 1. 编译 Layer 2 静态库
.\music_client\build.ps1 -Platform x64

# 2. 编译 Layer 3 provider DLL
.\netease_client\build.ps1 -Platform x64

# 3. 编译 Layer 1 插件 DLL
.\foobar_comment_flow\foo_danmaku\build.ps1 -Platform x64

# 4. 打包为标准安装包
.\foobar_comment_flow\pack.ps1 -Platform x64 -Version "1.0.0"
```

产物：`foobar_comment_flow/dist/foo_danmaku-1.0.0.fb2k-component`

---

## 安装

双击 `.fb2k-component` 文件，或拖放到 foobar2000 窗口，
安装程序自动将两个 DLL 复制到 `components/` 目录。

---

## 扩展：接入新音乐平台

1. 新建项目，实现 `music_provider.h` 中的 `MusicProviderVTable`
2. 导出 `music_provider_vtable()` 函数
3. 将产出 DLL 放入 `components/` 目录
4. 在插件初始化时调用 `music_client_load_provider(client, L"yourprovider.dll", nullptr)`

Layer 1 和 Layer 2 **零修改**。

---

## 许可证

MIT License
