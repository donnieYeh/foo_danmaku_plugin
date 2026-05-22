"""Local web demo for netease-comments SDK.

Run:
    python examples/demo_app.py --port 7860

Then open:
    http://127.0.0.1:7860

This demo intentionally uses only the Python standard library for the web UI.
"""
from __future__ import annotations

import argparse
import html
import json
import logging
import threading
import time
import urllib.parse
import webbrowser
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from netease_comments import NeteaseClient, ResourceType

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("demo_app")

PAGE_SIZE = 50
DEFAULT_LIMIT = 30

CSS = """
:root { color-scheme: light dark; }
* { box-sizing: border-box; }
body {
  margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Arial, "Microsoft YaHei", sans-serif;
  background: #0f172a; color: #e5e7eb;
}
a { color: #93c5fd; }
.container { max-width: 1100px; margin: 0 auto; padding: 28px 20px 60px; }
.hero {
  border: 1px solid rgba(148,163,184,.25); border-radius: 22px; padding: 28px;
  background: radial-gradient(circle at 20% 20%, rgba(239,68,68,.20), transparent 32%),
              linear-gradient(135deg, rgba(30,41,59,.95), rgba(15,23,42,.94));
  box-shadow: 0 20px 80px rgba(0,0,0,.35);
}
h1 { margin: 0 0 8px; font-size: 34px; letter-spacing: -.03em; }
.sub { color: #94a3b8; margin-bottom: 22px; line-height: 1.7; }
form { display: grid; grid-template-columns: 1.2fr .8fr .7fr .35fr; gap: 12px; align-items: end; }
label { display: block; font-size: 13px; color: #cbd5e1; margin-bottom: 6px; }
input, select, button {
  width: 100%; height: 44px; border-radius: 12px; border: 1px solid rgba(148,163,184,.35);
  background: rgba(15,23,42,.80); color: #f8fafc; padding: 0 13px; font-size: 15px;
}
button { cursor: pointer; border: none; background: #ef4444; font-weight: 700; box-shadow: 0 10px 28px rgba(239,68,68,.28); }
button:hover { background: #dc2626; }
.grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; margin-top: 18px; }
.card { border: 1px solid rgba(148,163,184,.22); border-radius: 16px; background: rgba(15,23,42,.68); padding: 16px; }
.metric { font-size: 24px; font-weight: 800; }
.metric-label { color: #94a3b8; font-size: 13px; margin-top: 6px; }
.section { margin-top: 22px; }
.song { display: flex; gap: 14px; align-items: center; }
.cover { width: 72px; height: 72px; border-radius: 16px; object-fit: cover; background: #1e293b; }
.song-title { font-size: 22px; font-weight: 800; margin-bottom: 5px; }
.song-meta { color: #94a3b8; line-height: 1.6; }
.comments { display: grid; gap: 12px; margin-top: 14px; }
.comment { border: 1px solid rgba(148,163,184,.18); border-radius: 16px; padding: 14px 16px; background: rgba(30,41,59,.72); }
.comment-head { display: flex; justify-content: space-between; gap: 12px; color: #93c5fd; font-size: 14px; margin-bottom: 8px; }
.content { white-space: pre-wrap; line-height: 1.7; color: #f1f5f9; }
.error { border-color: rgba(248,113,113,.55); background: rgba(127,29,29,.40); color: #fecaca; }
.footer { margin-top: 28px; color: #64748b; font-size: 13px; }
@media (max-width: 820px) { form { grid-template-columns: 1fr; } .grid { grid-template-columns: 1fr 1fr; } }
"""


def esc(value: Any) -> str:
    return html.escape("" if value is None else str(value), quote=True)


def fmt_time(ms: Any) -> str:
    try:
        return datetime.fromtimestamp(int(ms) / 1000).strftime("%Y-%m-%d %H:%M")
    except Exception:
        return "-"


def artist_text(song: dict[str, Any]) -> str:
    return " / ".join(a.get("name", "") for a in (song.get("ar") or song.get("artists") or []))


def album_name(song: dict[str, Any]) -> str:
    return (song.get("al") or song.get("album") or {}).get("name", "")


def album_pic(song: dict[str, Any]) -> str:
    return (song.get("al") or song.get("album") or {}).get("picUrl", "")


def render_page(query: dict[str, str] | None = None, result: dict[str, Any] | None = None, error: str | None = None) -> bytes:
    query = query or {}
    name = query.get("name", "海阔天空")
    artist = query.get("artist", "Beyond")
    limit = query.get("limit", str(DEFAULT_LIMIT))

    body = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        "<meta name='viewport' content='width=device-width,initial-scale=1'>",
        "<title>网易云评论 SDK Demo</title>",
        f"<style>{CSS}</style></head><body><main class='container'>",
        "<section class='hero'>",
        "<h1>网易云音乐评论 SDK Demo</h1>",
        "<div class='sub'>输入歌曲元信息，Demo 会调用 <code>find_song()</code> 定位歌曲，再调用 <code>iter_comments()</code> 抓取评论。</div>",
        "<form method='get' action='/'>",
        f"<div><label>歌曲名</label><input name='name' value='{esc(name)}' placeholder='海阔天空'></div>",
        f"<div><label>歌手，可留空</label><input name='artist' value='{esc(artist)}' placeholder='Beyond'></div>",
        f"<div><label>评论条数</label><select name='limit'>" + "".join(
            f"<option value='{n}' {'selected' if str(n)==str(limit) else ''}>{n}</option>" for n in (10, 30, 50, 100)
        ) + "</select></div>",
        "<div><button type='submit'>抓取</button></div>",
        "</form></section>",
    ]

    if error:
        body.append(f"<section class='section card error'><b>请求失败：</b>{esc(error)}</section>")

    if result:
        song = result["song"]
        comments = result["comments"]
        elapsed = result["elapsed"]
        total_likes = sum(int(c.get("likedCount") or 0) for c in comments)
        avg_len = round(sum(len(c.get("content") or "") for c in comments) / max(len(comments), 1), 1)
        pic = album_pic(song)
        body.extend([
            "<section class='section card song'>",
            f"<img class='cover' src='{esc(pic)}' alt='cover' onerror=\"this.style.display='none'\">",
            "<div>",
            f"<div class='song-title'>{esc(song.get('name'))}</div>",
            f"<div class='song-meta'>ID: {esc(song.get('id'))} ｜ 歌手: {esc(artist_text(song))} ｜ 专辑: {esc(album_name(song))}</div>",
            "</div></section>",
            "<section class='grid'>",
            f"<div class='card'><div class='metric'>{len(comments)}</div><div class='metric-label'>本次获取评论</div></div>",
            f"<div class='card'><div class='metric'>{total_likes}</div><div class='metric-label'>评论点赞合计</div></div>",
            f"<div class='card'><div class='metric'>{avg_len}</div><div class='metric-label'>平均字数</div></div>",
            f"<div class='card'><div class='metric'>{elapsed:.2f}s</div><div class='metric-label'>接口耗时</div></div>",
            "</section>",
            "<section class='section'><h2>评论预览</h2><div class='comments'>",
        ])
        for c in comments:
            user = c.get("user") or {}
            body.append(
                "<article class='comment'>"
                f"<div class='comment-head'><span>{esc(user.get('nickname', '匿名'))}</span>"
                f"<span>👍 {esc(c.get('likedCount', 0))} ｜ {esc(fmt_time(c.get('time')))}</span></div>"
                f"<div class='content'>{esc(c.get('content', ''))}</div>"
                "</article>"
            )
        body.append("</div></section>")

    body.extend([
        "<div class='footer'>提示：匿名访问可能触发网易云风控；如遇 -460，可稍后再试或降低频率。</div>",
        "</main></body></html>",
    ])
    return "".join(body).encode("utf-8")


class DemoHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802 - stdlib hook
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path not in ("/", "/index.html"):
            self.send_error(404)
            return

        params = {k: v[-1] for k, v in urllib.parse.parse_qs(parsed.query).items()}
        result = None
        error = None

        # 首次打开页面也展示默认效果；用户传空 name 时不请求。
        name = (params.get("name") or "海阔天空").strip()
        artist = (params.get("artist") or "Beyond").strip() or None
        try:
            limit = max(1, min(int(params.get("limit") or DEFAULT_LIMIT), 100))
        except ValueError:
            limit = DEFAULT_LIMIT

        if name:
            started = time.perf_counter()
            try:
                with NeteaseClient(min_interval=0.1, max_interval=0.2) as client:
                    song = client.find_song(name, artist=artist)
                    if not song:
                        raise LookupError(f"未找到歌曲：{name} / {artist or ''}")
                    comments = list(
                        client.iter_comments(
                            ResourceType.SONG,
                            song["id"],
                            limit=limit,
                            page_size=min(PAGE_SIZE, limit),
                        )
                    )
                result = {"song": song, "comments": comments, "elapsed": time.perf_counter() - started}
            except Exception as exc:  # Demo: show error in UI
                log.exception("demo request failed")
                error = repr(exc)

        payload = render_page({"name": name, "artist": artist or "", "limit": str(limit)}, result, error)
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt: str, *args: Any) -> None:
        log.info("%s - %s", self.address_string(), fmt % args)


def main() -> None:
    parser = argparse.ArgumentParser(description="Run local Web demo for netease-comments SDK")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7860)
    parser.add_argument("--no-open", action="store_true", help="Do not open browser automatically")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), DemoHandler)
    url = f"http://{args.host}:{args.port}"
    print(f"Demo running: {url}")
    print("Press Ctrl+C to stop.")
    if not args.no_open:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping demo server...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
