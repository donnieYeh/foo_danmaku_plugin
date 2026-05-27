# qqmusic_client — 设计文档

> 版本：v1.1  
> 日期：2026-05-27  
> 状态：已实现（API 兼容性修复）

---

## 1. 背景与目标

`qqmusic_client` 是与 `netease_client` 并列的第三方音乐平台 provider DLL，
实现 `music_client` 定义的 `MusicProviderVTable` 接口（Layer-3 driver contract），
为 `foo_danmaku` / `music_client` 提供 **QQ 音乐** 的搜索和评论能力。

| 目标 | 说明 |
|---|---|
| 零第三方运行时依赖 | 只依赖 Windows 系统 DLL（WinHTTP、BCrypt） |
| 稳定的 C ABI 接口 | 兼容 music_client Layer-2 契约，同时保留独立 qqmusic_* API |
| 匿名访问可用 | 无 Cookie 即可访问大多数公开歌曲的评论 |
| Cookie 认证 | 支持传入完整 QQ 音乐 Cookie 字符串（自动提取 p_skey/skey 计算 g_tk） |

---

## 2. 整体架构

```
windsurf/
├── music_client/
│   └── include/
│       ├── music_client.h       ← Layer-2 API（静态库）
│       └── music_provider.h     ← Layer-3 driver contract（原始定义）
│
└── qqmusic_client/              ← 本项目
    ├── DESIGN.md
    ├── build.ps1
    ├── include/
    │   ├── qqmusic_client.h     ← 独立公开 C API（dllexport/dllimport）
    │   └── music_provider.h     ← Layer-3 契约副本（build.ps1 参考来源）
    ├── src/
    │   ├── logger.h / .cpp      ← 日志（namespace qqmusic）
    │   ├── json.h / .cpp        ← 轻量 JSON 解析（新增 object_raw）
    │   ├── http.h / .cpp        ← WinHTTP GET 客户端（目标：c.y.qq.com）
    │   ├── api.h / .cpp         ← 搜索 + 评论业务逻辑
    │   ├── qqmusic_client.cpp   ← 导出 C API
    │   └── provider_entry.cpp   ← MusicProviderVTable 适配器
    └── test/
        ├── test_qqmusic.cpp     ← 独立控制台测试程序
        └── test_build.ps1
```

---

## 3. 公开 C API

文件：`include/qqmusic_client.h`

| 函数 | 说明 |
|---|---|
| `qqmusic_create(cookie)` | 创建实例；cookie 可为 NULL（匿名） |
| `qqmusic_destroy(h)` | 释放所有资源 |
| `qqmusic_search_song(h, kw, out_mid, len)` | 搜索歌曲，返回 songmid |
| `qqmusic_search_song_with_cover(...)` | 搜索歌曲 + 专辑封面 URL |
| `qqmusic_get_comments_by_keyword(h, name, artist, limit, cb, ud)` | 关键词搜索后获取评论 |
| `qqmusic_get_comments_by_id(h, mid, limit, cb, ud)` | 直接按 songmid 获取评论（多页） |
| `qqmusic_get_comments_by_id_paged(h, mid, offset, size, cb, ud, &n)` | 单页评论（分页流式接口） |
| `qqmusic_last_error(h)` | 最近一次错误描述 |
| `qqmusic_set_global_log(cb, ud)` | 注册全局日志回调 |

---

## 4. QQ 音乐 API 对接

### 4.1 搜索

#### ⚠️ 端点迁移说明（2026-05）

原 `soso/fcgi-bin/client_search_cp` 端点已被服务端封禁（持续返回非预期错误码或空响应）。
当前实现切换到 **smartbox**（搜索建议）端点，并以独立的 **song detail** 调用拉取专辑信息。
即一次搜索 = **两跳 HTTP**。

> `check_api_code` 仍保留 `code==0` 与 `code==200` 双判定，作为防御性兼容（smartbox 实际返回 `code:0`）。

#### Step 1 — smartbox（拿 id / mid）

```
GET https://c.y.qq.com/splcloud/fcgi-bin/smartbox_new.fcg
    ?is_xml=0
    &key={URL编码的关键词}
    &g_tk=5381&loginUin=0&hostUin=0
    &format=json&inCharset=utf-8&outCharset=utf-8
    &notice=0&platform=yqq.json&needNewCode=1
Referer: https://y.qq.com/
```

响应结构：

```json
{"code":0,"data":{"song":{"count":N,"itemlist":[
    {"id":"97773","mid":"004Z8Ihr0JIu5s","name":"...","singer":"..."},
    ...
]}}}
```

| JSON 路径 | 说明 |
|---|---|
| `data.song.count` | 命中数量；`0` 视为未找到 |
| `data.song.itemlist[0].id` | **JSON 字符串**形式的数字 ID（评论 API 的 `topid`） |
| `data.song.itemlist[0].mid` | 字母数字混合 mid，如 `"001OLkXf2nqxZ9"` |

> 注意：smartbox 响应**不包含**专辑 mid，因此封面 URL 需要 Step 2。
> 此外 `id` 字段为字符串，解析时先用 `json::num` 尝试，失败则 `json::str` + `std::stoll` 兜底。

#### Step 2 — song detail（拿 album mid → 封面 URL，非致命）

```
GET https://c.y.qq.com/v8/fcg-bin/fcg_play_single_song.fcg
    ?songmid={mid}
    &tmeAppID=qqmusic
    &format=json&inCharset=utf-8&outCharset=utf-8
    &notice=0&platform=yqq.json&needNewCode=0
Referer: https://y.qq.com/
```

响应：`{"code":0,"data":[{"album":{"mid":"..."}, ...}]}`

| JSON 路径 | 用途 |
|---|---|
| `data[0].album.mid` | 拼接封面 URL |

封面 URL 格式：`https://y.gtimg.cn/music/photo_new/T002R300x300M000{album.mid}_1.jpg`

> Step 2 失败仅记日志，不影响搜索结果返回——`SongInfo.id` / `SongInfo.mid` 仍可用于后续评论拉取。

### 4.2 评论（单页）

```
GET https://c.y.qq.com/base/fcgi-bin/fcg_global_comment_h5.fcg
    ?g_tk={g_tk}
    &loginUin={uin}&hostUin=0
    &format=json&inCharset=utf-8&outCharset=utf-8
    &notice=0&platform=yqq.json&needNewCode=0
    &cid=205360772&reqtype=2&biztype=1
    &topid={songmid}
    &cmd=8&needmusiccrit=0
    &pagenum={page}&pagesize={size}
    &lasttime=0&ct=24&cv=4747474
Referer: https://y.qq.com/
```

- `pagenum`：0 起始页码（由 vtable 传入的 `offset / page_size` 换算）
- 匿名访问：`g_tk=5381`，`loginUin=0`
- 认证访问：从 Cookie 中提取 `p_skey`（优先）或 `skey` 计算 g_tk

### 4.3 g_tk 计算

```cpp
unsigned int hash = 5381u;
for (wchar_t c : skey)
    hash += (hash << 5) + (unsigned int)(unsigned short)c;
g_tk = (int)(hash & 0x7FFFFFFF);
```

### 4.4 评论 JSON 解析

支持两种响应结构（新旧接口格式兼容）：

```
格式 A（直接）:  data.commentlist[]  /  data.hotcommentlist[]
格式 B（嵌套）:  data.comment.commentlist[]  /  data.comment.hotcommentlist[]
```

每条评论字段：`rootcommentcontent`（正文）、`nick`（昵称）、`praisenum`（点赞数）

> ⚠️ 注意：QQ 音乐 `fcg_global_comment_h5.fcg` 的评论正文字段是
> `rootcommentcontent`，**不是** `content`。后者不存在，读取会得到空字符串。

### 4.5 分页行为与契约遵守

**QQ Music 匿名限制**：`fcg_global_comment_h5.fcg` 匿名访问每页固定返回 **10 条**，
与请求的 `pagesize` 无关。

**`MusicProviderVTable::get_comments_paged` 契约要求**：

| 规则 | 说明 |
|---|---|
| `offset` 是逻辑评论下标，非 HTTP 参数 | provider 负责将其映射到 API 的 pagenum |
| `page_num = offset / actual_page_size` | actual_page_size = 10（QQ Music 匿名实际值），**不是** `page_limit` |
| `delivered == 0` 才是流结束信号 | `delivered < page_limit` **不代表**结束，调用方须继续翻页 |

本 provider 在 `api.cpp` 中用 `kQQApiPageSize = 10` 固定分页基数，
确保 `offset=0→page_num=0`，`offset=10→page_num=1`，以此类推。

---

## 5. 构建

```powershell
# 构建 x64 DLL
.\build.ps1

# 构建 Win32 DLL
.\build.ps1 -Platform Win32

# 构建并运行测试（默认：晴天 周杰伦）
.\test\test_build.ps1

# 自定义测试
.\test\test_build.ps1 -Title "夜曲" -Artist "周杰伦"
```

产物目录：`build\x64\qqmusic_client.dll` / `build\x64\qqmusic_client.lib`

链接依赖：`winhttp.lib`、`bcrypt.lib`、`kernel32.lib`

---

## 6. music_client 集成

`qqmusic_client.dll` 通过 `music_provider_vtable()` 导出符合 `MusicProviderVTable`
的函数指针表，可被 `music_client` 以 Layer-3 provider 方式动态加载：

```cpp
// Layer-2 (music_client)
music_client_load_provider(h, L"C:\\...\\qqmusic_client.dll", NULL);

// search_song 返回 song_id + 封面图片字节（URL 由 music_client 内部下载）
void* cover_data = nullptr; int cover_size = 0;
music_client_search_song(h, L"晴天 周杰伦", song_id, 64, &cover_data, &cover_size);
music_client_free(cover_data);

// 分页拉取评论；delivered==0 才是流结束
music_client_get_comments_paged(h, song_id, 0, 100, cb, userdata, &n);
```

---

## 7. 错误码映射

| QQMUSIC_* | MUSIC_ERR_* | 说明 |
|---|---|---|
| 0 | 0 | 成功 |
| -1 | -1 | 参数错误 |
| -2 | -2 | 网络失败 |
| -4 | -4 | 歌曲未找到 |
| -5 | -5 | API 返回错误 |
| -9 | -9 | 内部错误 |

---

*文档结束*
