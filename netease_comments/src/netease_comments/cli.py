"""CLI: netease-comments fetch --type song --id 33894312 --out comments.jsonl"""
from __future__ import annotations

import json
import logging
import sys
from pathlib import Path
from typing import Optional

try:
    import typer
    from rich.logging import RichHandler
except ImportError:
    print("Install CLI extras: pip install 'netease-comments[cli]'", file=sys.stderr)
    raise

from .client import NeteaseClient
from .types import OrderType, ResourceType

app = typer.Typer(add_completion=False, help="NetEase Cloud Music comments crawler.")


@app.command()
def fetch(
    type: str = typer.Option("song", "--type", "-t", help="song|mv|playlist|album|dj|video|event"),
    id: str = typer.Option(..., "--id", "-i", help="Resource ID"),
    out: Path = typer.Option(Path("comments.jsonl"), "--out", "-o", help="Output JSONL file"),
    order: str = typer.Option("time", "--order", help="time|hot|recommend"),
    page_size: int = typer.Option(100, "--page-size"),
    limit: Optional[int] = typer.Option(None, "--limit", help="Max comments to fetch"),
    cookie: Optional[str] = typer.Option(None, "--cookie", help="Raw Cookie string"),
    proxy: Optional[str] = typer.Option(None, "--proxy"),
    resume: bool = typer.Option(False, "--resume", help="Append + dedupe by commentId"),
    verbose: bool = typer.Option(False, "--verbose", "-v"),
):
    """Fetch all comments and dump as JSONL."""
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(message)s",
        handlers=[RichHandler(rich_tracebacks=True)],
    )
    resource = ResourceType(type)

    seen: set[int] = set()
    mode = "a" if resume and out.exists() else "w"
    if resume and out.exists():
        with out.open("r", encoding="utf-8") as f:
            for line in f:
                try:
                    seen.add(json.loads(line)["commentId"])
                except Exception:
                    pass
        typer.echo(f"resume: loaded {len(seen)} existing comments")

    client = NeteaseClient(cookie=cookie, proxy=proxy)
    n = 0
    with out.open(mode, encoding="utf-8") as f:
        try:
            for c in client.iter_comments(
                resource, id, order=order, page_size=page_size, limit=limit,
            ):
                cid = c.get("commentId")
                if cid in seen:
                    continue
                seen.add(cid)
                f.write(json.dumps(c, ensure_ascii=False) + "\n")
                n += 1
                if n % 100 == 0:
                    typer.echo(f"fetched {n} comments ...")
                    f.flush()
        except KeyboardInterrupt:
            typer.echo("interrupted by user")
    client.close()
    typer.echo(f"done. wrote {n} new comments to {out}")


@app.command()
def search(
    keyword: str = typer.Argument(..., help="Search keyword, e.g. '海阔天空 Beyond'"),
    type: str = typer.Option("song", "--type", "-t", help="song|album|playlist|mv|video"),
    limit: int = typer.Option(10, "--limit"),
):
    """Search NetEase resources by keyword."""
    client = NeteaseClient()
    res = client.search(keyword, resource=ResourceType(type), limit=limit)
    key = {"song": "songs", "album": "albums", "playlist": "playlists",
           "mv": "mvs", "video": "videos"}[type]
    rows = res.get(key) or []
    for r in rows:
        if type == "song":
            ars = " / ".join(a.get("name", "") for a in (r.get("ar") or []))
            album = (r.get("al") or {}).get("name", "")
            typer.echo(f"id={r['id']:>12}  {r['name']}  -  {ars}  [{album}]")
        else:
            typer.echo(f"id={r.get('id')}  {r.get('name')}")
    client.close()


@app.command("fetch-by-meta")
def fetch_by_meta(
    name: str = typer.Option(..., "--name", "-n", help="Song name"),
    artist: Optional[str] = typer.Option(None, "--artist", "-a"),
    album: Optional[str] = typer.Option(None, "--album"),
    strict: bool = typer.Option(False, "--strict", help="Require exact match"),
    out: Path = typer.Option(Path("comments.jsonl"), "--out", "-o"),
    order: str = typer.Option("time", "--order"),
    page_size: int = typer.Option(100, "--page-size"),
    limit: Optional[int] = typer.Option(None, "--limit"),
    cookie: Optional[str] = typer.Option(None, "--cookie"),
    proxy: Optional[str] = typer.Option(None, "--proxy"),
    verbose: bool = typer.Option(False, "--verbose", "-v"),
):
    """Locate a song by (name, artist, album) then fetch its comments."""
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(message)s",
        handlers=[RichHandler(rich_tracebacks=True)],
    )
    client = NeteaseClient(cookie=cookie, proxy=proxy)
    song = client.find_song(name, artist, album, strict=strict)
    if not song:
        typer.echo(f"no song matched: name={name!r} artist={artist!r} album={album!r}", err=True)
        raise typer.Exit(2)
    sid = song["id"]
    ars = " / ".join(a.get("name", "") for a in (song.get("ar") or []))
    typer.echo(f"matched: id={sid} | {song['name']} - {ars}")
    n = 0
    with out.open("w", encoding="utf-8") as f:
        for c in client.iter_comments(
            ResourceType.SONG, sid, order=order, page_size=page_size, limit=limit,
        ):
            f.write(json.dumps(c, ensure_ascii=False) + "\n")
            n += 1
            if n % 100 == 0:
                typer.echo(f"fetched {n} ...")
                f.flush()
    client.close()
    typer.echo(f"done. wrote {n} comments to {out}")


@app.command()
def info(
    type: str = typer.Option("song", "--type", "-t"),
    id: str = typer.Option(..., "--id", "-i"),
):
    """Show total comment count for a resource."""
    client = NeteaseClient()
    data = client.get_resource_summary(ResourceType(type), id)
    typer.echo(json.dumps(
        {"total": data.get("totalCount"), "hot": len(data.get("hotComments") or [])},
        ensure_ascii=False, indent=2,
    ))
    client.close()


if __name__ == "__main__":
    app()
