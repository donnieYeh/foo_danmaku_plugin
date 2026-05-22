# netease-comments

逆向 weapi 加密的网易云音乐评论抓取 SDK。

## 安装
```bash
pip install -e .[cli]
```

## 使用（库）
```python
from netease_comments import NeteaseClient, ResourceType

client = NeteaseClient()
for c in client.iter_comments(ResourceType.SONG, 33894312, order="time", page_size=100):
    print(c["commentId"], c["user"]["nickname"], c["content"][:40])
```

## 根据歌曲元信息获取评论
```python
from netease_comments import NeteaseClient

with NeteaseClient() as client:
    # 1) 先按元信息定位歌曲
    song = client.find_song("海阔天空", artist="Beyond")
    print(song["id"], song["name"])

    # 2) 再直接迭代评论
    for c in client.iter_comments_by_meta("海阔天空", artist="Beyond", limit=1000):
        print(c["commentId"], c["user"]["nickname"], c["content"])
```

## 使用（CLI）
```bash
netease-comments fetch --type song --id 33894312 --out comments.jsonl --limit 1000

# 搜索歌曲
netease-comments search "海阔天空 Beyond" --limit 5

# 根据歌曲名/歌手名抓评论
netease-comments fetch-by-meta --name "海阔天空" --artist "Beyond" --out comments.jsonl
```

## 运行可视化 Demo
```bash
python examples/demo_app.py --port 7860
```

然后打开：

```text
http://127.0.0.1:7860
```

页面里可以输入歌曲名、歌手名和评论条数，点击抓取后会显示匹配歌曲、评论列表、点赞数和耗时统计。

## 支持资源
song / mv / playlist / album / dj / video

## 特性
- 纯 Python 实现 weapi AES+RSA 加密，无中间服务
- 游标翻页（cursor + time），可突破 5000 条 offset 上限
- 自动重试 + 退避，触发 -460 风控时降速
- 输出 JSONL，便于流式处理与断点续传
