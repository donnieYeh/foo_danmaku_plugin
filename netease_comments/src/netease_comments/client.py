"""High-level client for fetching NetEase Cloud Music comments."""
from __future__ import annotations

import logging
import random
import time
from typing import Any, Iterator, Optional

import httpx

from .crypto import encrypt_weapi
from .types import OrderType, ResourceType

log = logging.getLogger(__name__)

_BASE = "https://music.163.com"
_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)


class NeteaseError(RuntimeError):
    def __init__(self, code: int, msg: str):
        super().__init__(f"[{code}] {msg}")
        self.code = code
        self.msg = msg


class NeteaseClient:
    """Synchronous NetEase comments client.

    Parameters
    ----------
    cookie : optional raw cookie string (MUSIC_U=...; __csrf=...). Anonymous works for most resources.
    proxy  : optional proxy URL, e.g. http://127.0.0.1:7890
    timeout: HTTP timeout in seconds.
    min_interval / max_interval: random sleep between paged requests (anti-rate-limit).
    """

    def __init__(
        self,
        cookie: Optional[str] = None,
        proxy: Optional[str] = None,
        timeout: float = 15.0,
        min_interval: float = 0.4,
        max_interval: float = 1.2,
    ) -> None:
        headers = {
            "User-Agent": _UA,
            "Referer": f"{_BASE}/",
            "Origin": _BASE,
            "Content-Type": "application/x-www-form-urlencoded",
        }
        if cookie:
            headers["Cookie"] = cookie
        self._http = httpx.Client(
            base_url=_BASE,
            headers=headers,
            timeout=timeout,
            proxy=proxy,
            follow_redirects=True,
        )
        # 匿名时也要带个 NMTID，否则部分接口拒绝
        if not cookie:
            self._http.cookies.set("NMTID", "".join(random.choices("0123456789abcdef", k=32)))
            self._http.cookies.set("os", "pc")
            self._http.cookies.set("appver", "2.9.7")
        self.min_interval = min_interval
        self.max_interval = max_interval

    # ------------- low level -------------
    def _weapi(self, path: str, payload: dict[str, Any]) -> dict[str, Any]:
        body = encrypt_weapi(payload)
        url = path if path.startswith("/") else f"/{path}"
        for attempt in range(5):
            try:
                r = self._http.post(f"/weapi{url}", data=body)
                r.raise_for_status()
                data = r.json()
            except (httpx.HTTPError, ValueError) as e:
                wait = 2 ** attempt + random.random()
                log.warning("request failed (%s), retry in %.1fs", e, wait)
                time.sleep(wait)
                continue
            code = data.get("code", 200)
            if code == 200:
                return data
            if code == -460:  # 风控
                wait = 30 + 30 * attempt
                log.warning("hit anti-bot (-460), sleeping %ss", wait)
                time.sleep(wait)
                continue
            raise NeteaseError(code, data.get("message") or data.get("msg") or "unknown")
        raise NeteaseError(-1, "max retries exceeded")

    # ------------- public -------------
    def get_comments_page(
        self,
        resource: ResourceType,
        resource_id: int | str,
        *,
        order: OrderType | str = OrderType.TIME,
        page_size: int = 100,
        cursor: int = -1,
        page_no: int = 1,
    ) -> dict[str, Any]:
        """Fetch a single page via /v2/resource/comments (cursor-based, unbounded depth)."""
        if isinstance(order, str):
            order = {"recommend": OrderType.RECOMMEND, "hot": OrderType.HOT, "time": OrderType.TIME}[order]
        thread_id = f"{resource.thread_prefix}{resource_id}"
        # 新版统一端点：/v2/resource/comments （payload 携带 threadId）
        payload = {
            "threadId": thread_id,
            "pageNo": page_no,
            "pageSize": page_size,
            "cursor": cursor,
            "sortType": int(order),
            "showInner": True,
            "csrf_token": "",
        }
        return self._weapi("/v2/resource/comments", payload)

    def iter_comments(
        self,
        resource: ResourceType,
        resource_id: int | str,
        *,
        order: OrderType | str = OrderType.TIME,
        page_size: int = 100,
        limit: Optional[int] = None,
    ) -> Iterator[dict[str, Any]]:
        """Iterate ALL comments via cursor pagination. Yields normalized comment dicts."""
        cursor = -1
        page_no = 1
        fetched = 0
        while True:
            data = self.get_comments_page(
                resource, resource_id,
                order=order, page_size=page_size,
                cursor=cursor, page_no=page_no,
            )
            block = data.get("data") or data
            comments = block.get("comments") or []
            if not comments:
                log.info("no more comments at page %d (cursor=%s)", page_no, cursor)
                return
            for c in comments:
                yield c
                fetched += 1
                if limit and fetched >= limit:
                    return
            has_more = block.get("hasMore", True)
            # cursor 为本页最后一条的 time（毫秒时间戳）
            cursor = comments[-1].get("time", cursor)
            if not has_more:
                log.info("hasMore=false, finished at %d comments", fetched)
                return
            page_no += 1
            time.sleep(random.uniform(self.min_interval, self.max_interval))

    # ------------- search by metadata -------------
    # type code for /cloudsearch: 1=song 10=album 100=artist 1000=playlist 1002=user 1004=mv 1006=lyric 1014=video 1018=综合
    _SEARCH_TYPE = {
        ResourceType.SONG: 1,
        ResourceType.ALBUM: 10,
        ResourceType.PLAYLIST: 1000,
        ResourceType.MV: 1004,
        ResourceType.VIDEO: 1014,
    }

    def search(
        self,
        keyword: str,
        *,
        resource: ResourceType = ResourceType.SONG,
        limit: int = 10,
        offset: int = 0,
    ) -> dict[str, Any]:
        """Search NetEase by keyword. Returns the raw 'result' block.

        Example: client.search("海阔天空 Beyond")['songs'][0]['id']
        """
        if resource not in self._SEARCH_TYPE:
            raise ValueError(f"search not supported for {resource}")
        payload = {
            "s": keyword,
            "type": self._SEARCH_TYPE[resource],
            "limit": limit,
            "offset": offset,
            "total": "true",
            "csrf_token": "",
        }
        # 优先试 cloudsearch（数据更全），失败则回退 /search/get（匿名可用）
        try:
            data = self._weapi("/cloudsearch/get/web", payload)
            if data.get("code") == 200 or "result" in data:
                return data.get("result") or {}
        except NeteaseError as e:
            if e.code != 50000005:
                raise
            log.debug("cloudsearch denied, falling back to /search/get")
        data = self._weapi("/search/get", payload)
        result = data.get("result") or {}
        # 旧接口 songs[*] 用 artists/album，统一规范为新接口的 ar/al
        for s in (result.get("songs") or []):
            if "ar" not in s and "artists" in s:
                s["ar"] = s["artists"]
            if "al" not in s and "album" in s:
                s["al"] = s["album"]
        return result

    def find_song(
        self,
        name: str,
        artist: Optional[str] = None,
        album: Optional[str] = None,
        *,
        strict: bool = False,
    ) -> Optional[dict[str, Any]]:
        """Locate a single song by metadata. Returns the best match or None.

        Matching logic:
        - if strict: name (and artist/album if given) must equal case-insensitively
        - else: first result that contains all provided fields as substrings
        """
        q = " ".join(x for x in (name, artist, album) if x)
        res = self.search(q, resource=ResourceType.SONG, limit=20)
        songs = res.get("songs") or []
        if not songs:
            return None

        def norm(s: str) -> str:
            return (s or "").strip().lower()

        want_name = norm(name)
        want_artist = norm(artist) if artist else None
        want_album = norm(album) if album else None

        for s in songs:
            sn = norm(s.get("name"))
            sa = " / ".join(norm(a.get("name")) for a in (s.get("ar") or []))
            sb = norm((s.get("al") or {}).get("name"))
            if strict:
                if sn != want_name:
                    continue
                if want_artist and want_artist not in sa:
                    continue
                if want_album and sb != want_album:
                    continue
                return s
            else:
                if want_name not in sn and sn not in want_name:
                    continue
                if want_artist and want_artist not in sa:
                    continue
                if want_album and want_album not in sb:
                    continue
                return s
        # 宽松兜底：只有“纯歌名搜索”才返回 top1。
        # 如果用户提供了 artist/album，而前面的循环没有命中，说明元信息不匹配，不应误返回无关歌曲。
        if strict or want_artist or want_album:
            return None
        return songs[0]

    def iter_comments_by_meta(
        self,
        name: str,
        artist: Optional[str] = None,
        album: Optional[str] = None,
        *,
        strict: bool = False,
        **kwargs: Any,
    ) -> Iterator[dict[str, Any]]:
        """Convenience: locate song by (name, artist, album) then iterate its comments.

        Extra kwargs are passed to iter_comments (order, page_size, limit).
        Raises LookupError if no match.
        """
        song = self.find_song(name, artist, album, strict=strict)
        if not song:
            raise LookupError(f"song not found: name={name!r} artist={artist!r} album={album!r}")
        sid = song["id"]
        ars = " / ".join(a.get("name", "") for a in (song.get("ar") or []))
        log.info("resolved -> id=%s | %s - %s", sid, song.get("name"), ars)
        yield from self.iter_comments(ResourceType.SONG, sid, **kwargs)

    def get_resource_summary(
        self,
        resource: ResourceType,
        resource_id: int | str,
    ) -> dict[str, Any]:
        """Cheap call to validate id exists and get total comment count.

        Uses page_size=1 with hot order; returns the raw 'data' block.
        """
        data = self.get_comments_page(resource, resource_id, order=OrderType.HOT, page_size=1, cursor=-1)
        return data.get("data") or data

    def close(self) -> None:
        self._http.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
