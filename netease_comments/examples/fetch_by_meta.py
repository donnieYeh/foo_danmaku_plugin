"""Fetch comments by song metadata: name + artist + optional album."""
from netease_comments import NeteaseClient

with NeteaseClient() as client:
    song = client.find_song("海阔天空", artist="Beyond")
    print("matched:", song["id"], song["name"], "/".join(a["name"] for a in song["ar"]))

    for i, c in enumerate(client.iter_comments_by_meta("海阔天空", artist="Beyond", limit=10), 1):
        print(f"[{i}] {c['user']['nickname']}: {c['content'][:60]}")
