"""Quick smoke test: fetch a few comments for a song."""
import logging
from netease_comments import NeteaseClient, ResourceType

logging.basicConfig(level=logging.INFO)

SONG_ID = 33894312  # 经典老歌《海阔天空》id (example)

with NeteaseClient() as client:
    summary = client.get_resource_summary(ResourceType.SONG, SONG_ID)
    print("total comments:", summary.get("totalCount"))
    print("---")
    for i, c in enumerate(client.iter_comments(ResourceType.SONG, SONG_ID, limit=5)):
        print(f"[{i+1}] {c['user']['nickname']}: {c['content'][:60]}  (+{c.get('likedCount',0)})")
