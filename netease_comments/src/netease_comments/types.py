"""Common enums and type aliases."""
from enum import Enum


class ResourceType(str, Enum):
    """Resource type -> threadId prefix mapping for NetEase comments."""

    SONG = "song"        # R_SO_4_
    MV = "mv"            # R_MV_5_
    PLAYLIST = "playlist"  # A_PL_0_
    ALBUM = "album"      # R_AL_3_
    DJ = "dj"            # A_DJ_1_
    VIDEO = "video"      # R_VI_62_
    EVENT = "event"      # A_EV_2_

    @property
    def thread_prefix(self) -> str:
        return {
            "song": "R_SO_4_",
            "mv": "R_MV_5_",
            "playlist": "A_PL_0_",
            "album": "R_AL_3_",
            "dj": "A_DJ_1_",
            "video": "R_VI_62_",
            "event": "A_EV_2_",
        }[self.value]

    @property
    def api_type_code(self) -> int:
        """For /comment/new style routing if used."""
        return {
            "song": 0, "mv": 1, "playlist": 2,
            "album": 3, "dj": 4, "video": 5, "event": 6,
        }[self.value]


class OrderType(int, Enum):
    RECOMMEND = 1  # 推荐
    HOT = 2        # 热度
    TIME = 3       # 时间倒序（深翻必用）
