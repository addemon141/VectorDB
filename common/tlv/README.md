# TLV（服务节点 ↔ DataNode / Leader ↔ Follower）

一期阻塞 TCP + 线程，每任务一连接。帧布局（小端）：

| 字段 | 类型 | 说明 |
|------|------|------|
| magic | u32 | `0x56444231` (`VDB1`) |
| type | u16 | 见 `MsgType` |
| flags | u16 | bit0 = RESUME |
| total_len | u64 | 逻辑对象总字节；控制帧可为 0 |
| offset | u64 | 本 chunk 在对象内偏移 |
| chunk_len | u32 | payload 长度 |
| crc32 | u32 | payload CRC-32 |
| payload | bytes | chunk_len 字节 |

控制消息 payload 为 UTF-8 JSON（小对象）。数据面为原始字节。
