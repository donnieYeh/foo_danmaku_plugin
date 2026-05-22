# netease_client — 设计文档

> 版本：v1.0  
> 日期：2026-05-22  
> 状态：设计定稿，待实施

---

## 1. 背景与目标

### 1.1 背景

`foobar_comment_flow` 项目中的 `foo_danmaku` foobar2000 插件需要从网易云音乐获取歌曲评论，
目前通过调用 Python 脚本 (`fetch_comments.py`) 实现，存在以下问题：

- 运行时依赖 Python + `httpx` + `pycryptodome`，用户需自行安装
- `_popen` 的工作目录问题导致脚本路径不稳定
- 无法被其他 C/C++ 项目复用

### 1.2 目标

将网易云音乐评论获取能力实现为**独立的 Windows 原生 DLL**：

| 目标 | 说明 |
|---|---|
| 零第三方运行时依赖 | 只依赖 Windows 系统 DLL（WinHTTP、BCrypt） |
| 稳定的 C ABI 接口 | 可被 C/C++/C#/Python ctypes 等任意语言调用 |
| 完整 weapi 加密 | 移植 `crypto.py` 的 AES-CBC × 2 + 自定义 RSA |
| 可复用 | 与 foobar2000 插件解耦，独立版本管理 |

---

## 2. 整体架构

```
windsurf/
├── netease_client/              ← 本项目（独立库）
│   ├── DESIGN.md
│   ├── include/
│   │   └── netease_client.h     ← 唯一公开头文件（纯 C 接口）
│   ├── src/
│   │   ├── crypto.cpp           ← weapi 加密（BCrypt）
│   │   ├── http.cpp             ← HTTP 客户端（WinHTTP）
│   │   ├── json.cpp             ← 轻量 JSON 解析
│   │   ├── api.cpp              ← 搜索 + 评论业务逻辑
│   │   └── netease_client.cpp   ← 导出 C API + 错误管理
│   └── build.ps1
│
└── foobar_comment_flow/
    └── foo_danmaku/
        ├── SDK/netease_client/  ← 引用构建产物
        │   ├── netease_client.h
        │   ├── netease_client.lib
        │   └── netease_client.dll
        └── ...
```

---

## 3. 公开 C API 设计

文件：`include/netease_client.h`

### 3.1 设计原则

- **纯 C 接口**：避免 C++ ABI 兼容性问题（name mangling、异常、STL 布局）
- **不透明句柄**：调用方不感知内部实现，便于后续替换底层
- **回调模型**：评论数据通过回调逐条推送，避免大块内存在 DLL 边界传递
- **错误码 + 错误消息**：整数错误码用于程序判断，字符串用于日志/UI 显示

### 3.2 接口定义

```c
#ifndef NETEASE_CLIENT_H
#define NETEASE_CLIENT_H

#ifdef NETEASE_CLIENT_EXPORTS
#  define NETEASE_API __declspec(dllexport)
#else
#  define NETEASE_API __declspec(dllimport)
#endif

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 错误码 ─────────────────────────────────────────── */
#define NETEASE_OK              0   // 成功
#define NETEASE_ERR_PARAM      -1   // 参数错误
#define NETEASE_ERR_NETWORK    -2   // 网络请求失败
#define NETEASE_ERR_CRYPTO     -3   // 加密/解密失败
#define NETEASE_ERR_NOTFOUND   -4   // 歌曲未找到
#define NETEASE_ERR_API        -5   // 网易云 API 返回错误码
#define NETEASE_ERR_INTERNAL   -9   // 内部错误

/* ── 句柄 ────────────────────────────────────────────── */
typedef void* NeteaseHandle;

/* ── 生命周期 ─────────────────────────────────────────── */

/**
 * 创建客户端实例。
 * @param cookie  可选，原始 Cookie 字符串（"MUSIC_U=...;__csrf=..."），
 *                传 NULL 则使用匿名访问（多数歌曲可用）。
 * @return 非 NULL 表示成功，失败返回 NULL。
 */
NETEASE_API NeteaseHandle __stdcall netease_create(const wchar_t* cookie);

/**
 * 销毁客户端实例，释放所有内部资源。
 */
NETEASE_API void __stdcall netease_destroy(NeteaseHandle h);

/* ── 评论回调 ─────────────────────────────────────────── */

/**
 * 每获取到一条评论时触发。
 * @param content   评论正文（UTF-16）
 * @param nickname  用户昵称（UTF-16）
 * @param like_count 点赞数
 * @param userdata  调用方传入的上下文指针
 * @return 返回 0 继续获取，返回非 0 中断
 */
typedef int (__stdcall *NeteaseCommentCallback)(
    const wchar_t* content,
    const wchar_t* nickname,
    int            like_count,
    void*          userdata
);

/* ── 核心功能 ─────────────────────────────────────────── */

/**
 * 按歌曲名 + 歌手名搜索并获取评论（最常用入口）。
 * @param h         客户端句柄
 * @param song_name 歌曲名（UTF-16，必填）
 * @param artist    歌手名（UTF-16，可传 NULL）
 * @param limit     最多获取条数（建议 20-100）
 * @param callback  评论回调函数
 * @param userdata  透传给回调的上下文指针
 * @return NETEASE_OK 或错误码
 */
NETEASE_API int __stdcall netease_get_comments_by_keyword(
    NeteaseHandle          h,
    const wchar_t*         song_name,
    const wchar_t*         artist,
    int                    limit,
    NeteaseCommentCallback callback,
    void*                  userdata
);

/**
 * 直接按歌曲 ID 获取评论。
 * @param song_id  网易云歌曲 ID（纯数字字符串）
 */
NETEASE_API int __stdcall netease_get_comments_by_id(
    NeteaseHandle          h,
    const wchar_t*         song_id,
    int                    limit,
    NeteaseCommentCallback callback,
    void*                  userdata
);

/**
 * 搜索歌曲，返回第一个匹配的 ID。
 * @param out_song_id  输出缓冲区（调用方分配）
 * @param buf_wchars   缓冲区大小（wchar_t 个数）
 * @return NETEASE_OK 或错误码
 */
NETEASE_API int __stdcall netease_search_song(
    NeteaseHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_id,
    int            buf_wchars
);

/* ── 错误信息 ─────────────────────────────────────────── */

/**
 * 返回上次操作的错误描述字符串（UTF-16）。
 * 返回的指针在下次调用任何接口前有效。
 */
NETEASE_API const wchar_t* __stdcall netease_last_error(NeteaseHandle h);

#ifdef __cplusplus
}
#endif
#endif /* NETEASE_CLIENT_H */
```

---

## 4. 内部模块设计

### 4.1 crypto.cpp — weapi 加密

**移植自** `netease_comments/src/netease_comments/crypto.py`

#### 算法流程

```
输入: JSON payload (UTF-8 字节串)

step1: sec_key = random 16 个十六进制字符

step2: enc1 = AES-128-CBC(
           plaintext = JSON_bytes,
           key = "0CoJUm6Qyw8W8jud",   // 硬编码常量
           iv  = "0102030405060708"     // 硬编码常量
       )
       enc1_b64 = Base64(enc1)

step3: enc2 = AES-128-CBC(
           plaintext = enc1_b64,
           key = sec_key,
           iv  = "0102030405060708"
       )
       enc2_b64 = Base64(enc2)

step4: enc_sec_key = RSA_raw(
           m = reverse(sec_key).hex_to_bigint(),
           e = 0x10001,
           n = <1024-bit modulus 硬编码>
       )
       # 结果 zero-pad 到 256 个十六进制字符

输出: POST body = "params=<enc2_b64>&encSecKey=<enc_sec_key>"
```

#### Windows BCrypt 实现要点

| 步骤 | Windows API |
|---|---|
| AES-CBC 加密 | `BCryptOpenAlgorithmProvider` + `BCryptGenerateSymmetricKey` + `BCryptEncrypt` |
| PKCS7 填充 | `BCryptEncrypt` flags = `BCRYPT_BLOCK_PADDING` |
| Base64 编码 | `CryptBinaryToStringA` (flags = `CRYPT_STRING_BASE64`) |
| RSA 原始 modpow | `BCryptOpenAlgorithmProvider(BCRYPT_RSA_ALGORITHM)` + `BCryptImportKeyPair` + `BCryptEncrypt(BCRYPT_PAD_NONE)` |
| 随机 sec_key | `BCryptGenRandom` |

#### 关键常量

```cpp
// crypto.cpp 内部
static const BYTE kPresetKey[]  = "0CoJUm6Qyw8W8jud";  // 16 bytes
static const BYTE kIV[]         = "0102030405060708";  // 16 bytes
static const char kPubExponent[] = "010001";           // e = 65537
// kModulus: 256 个十六进制字符，见 crypto.py _MODULUS
```

### 4.2 http.cpp — HTTP 客户端

使用 **WinHTTP**（而非 WinInet），原因：
- 原生支持 TLS 1.2/1.3，适合后台无 UI 调用
- 无浏览器缓存/Cookie 存储副作用
- 连接复用（Session 级别）

#### 类设计

```cpp
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // POST application/x-www-form-urlencoded
    bool post(const std::wstring& url,
              const std::string&  body,
              const Headers&      extra_headers,
              std::string&        out_response);

private:
    HINTERNET m_session;   // WinHttpOpen
    // 复用 Session，每次请求新建 Connect + Request
};
```

#### 固定请求头（模拟浏览器）

```
User-Agent:   Mozilla/5.0 (Windows NT 10.0; Win64; x64) ...
Referer:      https://music.163.com/
Origin:       https://music.163.com
Content-Type: application/x-www-form-urlencoded
Cookie:       NMTID=<random_32hex>; os=pc; appver=2.9.7
```

### 4.3 json.cpp — 轻量 JSON 解析

不引入第三方库，实现以下两个原子操作即可满足需求：

```cpp
// 从 JSON 字符串中提取指定 key 的字符串值
// 支持转义（\n \\ \"）
std::string json_get_string(const std::string& json, const std::string& key);

// 从 JSON 字符串中提取指定 key 的整数值
int json_get_int(const std::string& json, const std::string& key, int default_val = 0);

// 提取 JSON array 中所有 object（用于迭代 comments[]）
// 返回每个 object 的原始字符串，供上层再解析
std::vector<std::string> json_get_array(const std::string& json, const std::string& key);
```

### 4.4 api.cpp — 业务逻辑

移植自 `client.py`，实现：

```
search_song(keyword)
    → POST /weapi/cloudsearch/get/web
      payload: { s, type:1, limit:20, offset:0, csrf_token:"" }
    → 解析 result.songs[0].id
    → fallback: POST /weapi/search/get

get_comments_page(thread_id, page_no, cursor, page_size)
    → POST /weapi/v2/resource/comments
      payload: { threadId, pageNo, pageSize, cursor, sortType:3, csrf_token:"" }
    → 解析 data.comments[]

iter_comments(song_id, limit, callback)
    → thread_id = "R_SO_4_{song_id}"
    → 循环调用 get_comments_page，游标翻页
    → 每条 comment 调用 callback
    → hasMore=false 或 fetched>=limit 时停止
```

#### 反速率限制策略

- 两次翻页之间随机 sleep 400~1200ms（与 Python 版保持一致）
- 遇到 API 返回码 `-460`（风控）：等待 30s 后重试，最多 3 次
- 遇到 `-400` / HTTP 403：直接返回 `NETEASE_ERR_API`

### 4.5 netease_client.cpp — 导出层

```cpp
// 内部实现结构
struct NeteaseContext {
    HttpClient   http;
    std::wstring last_error;
    std::wstring cookie;
};

// 每个导出函数：
// 1. 校验参数
// 2. 捕获所有 C++ 异常，转为错误码
// 3. 写 last_error 供 netease_last_error() 返回
```

---

## 5. 构建系统

### 5.1 目录产物

```
build/
  x64/
    netease_client.dll    ← 运行时复制到 foobar2000\components\ 旁
    netease_client.lib    ← 链接时使用
    netease_client.pdb    ← 调试符号（Release 也生成，方便崩溃分析）
```

### 5.2 链接库

```
winhttp.lib    ← WinHTTP
bcrypt.lib     ← BCrypt（加密）
```

### 5.3 编译选项

与 `foo_danmaku` 保持一致：`/std:c++17 /MD /O2 /DUNICODE /DNOMINMAX`

---

## 6. foo_danmaku 集成方案

### 6.1 接口替换

删除 `netEase/api.cpp`、`netEase/signature.cpp`，
改为在 `danmaku_ui.cpp` 中使用：

```cpp
#include "netease_client.h"

// DanmakuUIInstance 构造时
m_netease = netease_create(nullptr);

// 回调中
netease_get_comments_by_keyword(
    m_netease,
    title, artist, 50,
    [](const wchar_t* content, const wchar_t* nick, int likes, void* ud) -> int {
        auto* eng = static_cast<DanmakuEngine*>(ud);
        COLORREF c = likes > 1000 ? RGB(255,100,100)
                   : likes > 100  ? RGB(255,200,100)
                   : likes > 10   ? RGB(100,200,255)
                   :                RGB(255,255,255);
        eng->addDanmaku(content, c);
        return 0;
    },
    g_engine
);
```

### 6.2 部署

用户需将两个 DLL 放在同一目录：
```
C:\Program Files\foobar2000\components\
  foo_danmaku.dll
  netease_client.dll
```

---

## 7. 测试计划

### 7.1 单元测试（独立可执行文件）

```
netease_client/tests/
  test_crypto.cpp   — 验证 encrypt_weapi 与 Python 版输出一致
  test_http.cpp     — 验证 WinHTTP POST 可达 music.163.com
  test_api.cpp      — 搜索"晴天"返回正确 song_id
  test_e2e.cpp      — 完整流程：搜索 → 获取评论 → 验证条数 > 0
```

### 7.2 集成测试

- 将 `netease_client.dll` + `foo_danmaku.dll` 一起部署
- foobar2000 播放「晴天 - 周杰伦」，验证弹幕在面板中滚动
- 验证切换歌曲时评论正确刷新

---

## 8. 风险与决策记录

| 风险 | 影响 | 应对 |
|---|---|---|
| 网易云 API 变更（weapi 端点调整） | 无法获取评论 | 参数通过常量集中管理，便于快速修改 |
| BCrypt RSA `BCRYPT_PAD_NONE` 不支持自定义模数 | 加密模块失效 | 备用方案：手写 1024-bit modpow（数字固定，不需要通用大整数库）|
| 风控 -460 长时等待阻塞主线程 | foobar2000 卡顿 | 所有网络操作必须在工作线程中执行 |
| 多实例并发调用 | 数据竞争 | NeteaseContext 中的 HttpClient 加锁保护 |

---

## 9. 实施顺序

1. `include/netease_client.h` — 头文件定稿
2. `src/crypto.cpp` — 加密核心（最关键，需先验证正确性）
3. `src/http.cpp` — HTTP 客户端
4. `src/json.cpp` — JSON 解析工具
5. `src/api.cpp` — 业务逻辑
6. `src/netease_client.cpp` — 导出层
7. `build.ps1` — 构建脚本
8. 修改 `foo_danmaku` 集成

---

*文档结束*
