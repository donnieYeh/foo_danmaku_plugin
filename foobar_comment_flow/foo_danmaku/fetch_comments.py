import sys
import json
from netease_comments import NeteaseClient, ResourceType

keyword = sys.argv[1] if len(sys.argv) > 1 else "晴天"

client = NeteaseClient()
song = client.find_song(keyword, strict=False)
if not song:
    sys.stdout.buffer.write(json.dumps({"error": "song not found", "keyword": keyword}, ensure_ascii=False).encode('utf-8'))
    sys.exit(1)

song_id = song["id"]
song_name = song.get("name", "")
artists = " / ".join(a.get("name", "") for a in (song.get("ar") or []))

comments = []
for c in client.iter_comments(ResourceType.SONG, song_id, page_size=30, limit=30):
    comments.append({
        "content": c.get("content", ""),
        "nickname": c.get("user", {}).get("nickname", ""),
        "likedCount": c.get("likedCount", 0)
    })

result = {
    "songId": str(song_id),
    "songName": song_name,
    "artistName": artists,
    "comments": comments
}

sys.stdout.buffer.write(json.dumps(result, ensure_ascii=False).encode('utf-8'))
client.close()