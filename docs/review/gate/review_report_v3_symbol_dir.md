# gate_result symbol_dir 打通复审（v3）

审阅日期：2026-02-03  
审阅范围：`gate_result/` 中与 `symbol_dir` 相关的改动（`struct_def.h`, `shm_writer.*`, `shm_reader.cpp`, `md_gate_main.cpp`）。  
对照基准：`gate_result/design/gate/设计.md`、`gate_result/design/gate/施工设计稿.md`（关键决策点“symbol_id 的权威来源：方案 B”）。

---

## 1) 结论摘要

你已经把 **symbol_dir（id→wind_code）端到端打通**：

- gate：在 SHM 中分配并发布 `symbol_dir`（固定 entry=16B），并在启动时把 CSV 顺序的 wind_code 写入对应 slot。
- reader：`ValidateHeader()` 增加了 symbol_dir 的边界/重叠校验。
- trade（在 `trade_result`）：已实现按 symbol_dir 构建 `wind_code -> symbol_id` 映射（见对应 trade 侧复审报告）。

这使得“trade 使用自己的 500 CSV（或甚至不依赖 CSV）”成为可能，解决了之前**CSV 顺序不一致导致读错 slot 的静默错误**风险。

---

## 2) Compliance Check（对照设计）

### 2.1 symbol_dir 在设计稿中的位置与目标

设计稿把 symbol_dir 定义为可选扩展区，并在施工设计稿中明确为“symbol_id 权威来源”的方案 B：  
trade 启动时从 shm 构建映射，避免 CSV 不一致风险。

你的实现符合该目标。

### 2.2 SHM 布局与 ABI（是否偏离）

实现布局实际上变为：

- `[ShmHeader | symbol_dir | SnapshotEntry entries[]]`

而设计文档示意图写的是：

- `[ShmHeader | entries[] | (optional) symbol_dir]`

评估：

- 这是**可接受的实现偏离**：因为读端通过 `snapshot_offset/symbol_dir_offset` 定位，不依赖“固定顺序”；且未改变 `ShmHeader` 字段语义。
- 但建议更新文档示意图/TotalSize 推导（否则容易误导后来维护者以为 symbol_dir 在 entries 之后）。

---

## 3) Technical Deep Dive（关键点复核）

### 3.1 ABI/POD/二进制布局

- `symbol_dir` 采用固定大小 entry（`kSymbolDirEntryBytes=16`），为纯字节数组区，不含指针/动态容器，ABI 安全。
- `ShmHeader` 中 `symbol_dir_offset/symbol_dir_bytes/symbol_key_type` 字段保持 POD。

结论：通过。

### 3.2 mmap/shm_open 边界与重叠校验

你在 `ShmReader::ValidateHeader()` 增加了 symbol_dir 校验：

- `dir_end <= total_bytes`
- `symbol_dir_bytes >= symbol_count * entry_bytes`
- `snapshot_offset >= dir_end`（避免重叠）

结论：通过（属于“必须有”的防线）。

### 3.3 gate 发布时机与一致性

当前 gate 在 `Create()` 后立即写入 symbol_dir：

- 写入数据源：`wind_codes_`（来自 gate CSV）
- 写入范围：`i < wind_codes_.size()`，其余 slot 留空（shm 初始 memset 为 0）

评估：

- 符合预期：空 slot 表示“该 id 未被订阅/未配置”，trade 侧可忽略。

需要注意的边缘风险（低概率，但属于“严格性”问题）：

- symbol_dir 写入没有 seqlock/版本号；如果 trade 在 gate 正在写 symbol_dir 的瞬间打开 shm 并读取目录，理论上可能读到某条 entry 的中间态。
  - 现实中写入 48KB 量级极快，风险很低；
  - 若要把启动语义做到更硬，可考虑“发布就绪”机制（例如：先写目录，再把 `flags` 的 has_symbol_dir 位最后写入并让 trade 以此为准；或增加一个 `symbol_dir_ready_seq` 原子字段——但这会触及 ABI 演进）。

### 3.4 flags/版本语义

writer 侧已设置：

- `flags = bit0(has_snapshot) | bit1(has_symbol_dir)`

但 reader 当前不依赖 flags，只依赖 offset/bytes 非 0；这是可行的（flags 主要是可观测性/自检）。

---

## 4) 建议（最小改动、最高价值）

1. 更新 `gate_result/design/gate/设计.md` 的布局示意图，使其与当前实现一致（Header 后先 symbol_dir 再 entries）。  
2. 在 gate 启动日志打印：`symbol_dir_offset/symbol_dir_bytes/snapshot_offset`（便于现场排障，确认两端读的是同一 ABI）。  
3. 在 trade 侧增加“订阅需求 symbol 是否都能通过 symbol_dir 映射”的启动自检（避免策略请求了 gate 未订阅的 symbol）。  

