# gate_result 代码审查报告（对照《设计.md》《施工设计稿.md》）

审查范围：`gate_result/` 下的实现代码（`md_gate_main.cpp`, `struct_def.h`, `marketdata_payload.h`, `shm_writer.*`, `shm_reader.*`, `CMakeLists.txt`）。

结论摘要（按优先级）：

1. **热路径偏离设计（高风险 / 性能）**：TDF 回调写入路径使用 `std::mutex + std::unordered_map + std::string`（见 `md_gate_main.cpp:618`、`md_gate_main.cpp:621`），与设计稿“回调不做 mutex/map 频繁操作”的要求不符；可能导致抖动（jitter）与尾延迟放大。
2. **trade_app 侧落地缺失（中风险 / 交付）**：当前仅提供 `ShmReader`/`ShmWriter` 与 `md_gate` 示例主程序；设计稿要求的 `trade_app` 接入点（`ShmMarketDataApi`、epoch/heartbeat 降级语义落地）不在 `gate_result/` 内实现。
3. **回调线程模型未“定死”（中风险 / 正确性+性能）**：设计强调“单写者”约束；当前用 `write_mu_` 强行串行化（可能是正确性兜底），但缺少对“回调是否多线程”的显式判断/说明与结构化方案（SPSC/MPSC + writer 线程）。
4. **TDF 消息长度/条目大小未校验（中风险 / 内存安全）**：`HandleData()` 基于 `nItemCount` 遍历并 `reinterpret_cast`，但未校验 `nDataLen/nItemSize`（见 `md_gate_main.cpp:608` 起），在异常/损坏消息下存在 OOB 读风险（虽概率低，但属于典型“边界未封口”问题）。
5. **进程停止/资源关闭存在潜在竞态（中风险 / 稳定性）**：`g_stop` 用 `volatile bool` 跨线程/信号修改（见 `md_gate_main.cpp:28`、`SignalHandler`），且 shutdown 未与 SDK 回调线程建立“停止-回收”同步，存在“回调仍在跑但 writer 已 unmap/close”的理论风险。

---

## Phase 1：Compliance Check（设计符合性审查）

本节逐条对照《设计.md》《施工设计稿.md》的“必须写清/必须做到”项，指出偏离点并判断性质（合理优化 vs 错误实现）。

### 1) 进程划分与职责

设计要求：

- `md_gateway` 唯一登录 TDF，写共享内存快照；`trade_app` 只读 shm 做决策与交易（《设计.md》1、5、6 章）。

实现现状：

- `gate_result` 提供可执行程序 `md_gate`（`gate_result/CMakeLists.txt`），承担 “连接 TDF + 写 shm + heartbeat”。
- `trade_app` 侧仅提供 `ShmReader`/`ValidateHeader` API 骨架（`shm_reader.*`），未提供完整 `ShmMarketDataApi` 与降级/重启检测逻辑。

偏离评估：

- **偏离：命名/工程交付**（`md_gateway` vs `md_gate`，以及缺少 `trade_app` 侧接入示例）。属于**交付未完成**而非实现错误；但会影响后续验收（设计稿明确 trade_app 行为与降级语义）。

### 2) symbol 映射（wind_code -> symbol_id）与 symbol_count=3000 约束

设计要求：

- 订阅 CSV 构建 `wind_code -> symbol_id`，`symbol_count=3000`，不足补空、超出报错（《设计.md》1、5；《施工设计稿.md》1、3.2、5.1）。

实现现状：

- `Init()` 解析 CSV，构建 `sym2id_`（`md_gate_main.cpp`，`sym2id_` 初始化在 `Init()`，并检查 `wind_codes_.size() > opt_.symbol_count` 时失败）。
- shm header 的 `symbol_count` 取自 `--symbol-count`（默认 3000），并初始化整个 snapshot table（`shm_writer.cpp:74` 起）。

偏离评估：

- **轻微偏离：symbol_count 可配置**。设计文本写死 3000 作为基线，代码允许 `<3000`。这通常是**合理优化/工程灵活性**，但需要在文档/验收中明确“trade_app 必须按 header.symbol_count 工作”，避免读端假设 3000 固定。

### 3) SHM 布局与 ABI 校验字段

设计要求（最小集）：

- header 必须包含：`magic/abi_version/header_bytes/total_bytes/endian`、`writer_start_ns/heartbeat_ns`、快照区定位字段、`md_status/last_err/last_md_ns`（《设计.md》3、6；《施工设计稿.md》3.2）。
- SnapshotEntry：`seq + payload(320B) + 对齐 padding`，建议 entry=384B（《设计.md》3.3）。

实现现状：

- ABI 类型集中在 `struct_def.h`：`ShmHeader`、`SnapshotEntry`、`MarketData320`，并通过 `static_assert` 固定尺寸与对齐（`struct_def.h`）。
- `header.h` 仅为别名兼容层（`header.h`）。
- `ShmWriter::InitHeader_()` 填充 magic/version/offset/bytes 等字段（`shm_writer.cpp:244` 起）。
- `ShmReader::ValidateHeader()` 校验 magic/version/endian/entry_bytes/payload_bytes/offset 范围等（`shm_reader.cpp:93` 起）。

偏离评估：

- **基本符合设计**（布局与字段齐全，且明确预留 event ring / symbol_dir）。
- **改进点**：`ValidateHeader()` 存在“total_bytes==0 时不直接失败”的宽松路径（`shm_reader.cpp:105`），在 magic 正确但 total_bytes 异常的边界下可能放过不一致状态；建议收紧为 `total_bytes > 0`。
- **合规性提醒**：读端 `Open()` 未强制调用 `ValidateHeader()`，属于“接口层允许误用”，与设计强调的“trade_app 启动必须校验 header”存在落差（见 Phase 2）。

### 4) SeqLock 读写协议与单写者约束

设计要求：

- 写：`seq` odd → memcpy payload → `seq` even（release）；读：两次读 seq（acquire）一致才算成功（《设计.md》4；《施工设计稿.md》3.3）。
- **单写者**：SeqLock 不支持多线程同时写同一 entry；若回调多线程，需 SPSC/MPSC 或双缓冲（《设计.md》4.1/4.2；《施工设计稿.md》2.2、5.2）。

实现现状：

- `struct_def.h` 提供 seqlock helper：writer `fetch_add_relaxed + store_release`，reader `load_acquire + memcpy + load_acquire`（`struct_def.h` SeqLock helpers）。
- `HandleData()` 外层使用 `write_mu_` 串行化所有写入（`md_gate_main.cpp:618`），从而在“回调多线程”情况下也能维持单写者。

偏离评估：

- **协议实现符合设计**（读写伪代码一致）。
- **偏离：热路径 mutex**。设计建议“回调不做 mutex/map 频繁操作”；当前用 mutex 作为“单写者兜底”属于**正确性优先的实现**，但在 HFT 语境下通常视为**性能错误**（抖动/尾延迟）。建议按 Phase 2 的并发架构建议改造。

### 5) 心跳、状态与故障语义

设计要求：

- gateway 持续刷新 `heartbeat_ns`；断线时 `md_status != OK` 但 heartbeat 仍刷新；`last_md_ns` 仅在有行情时更新（《设计.md》1、6.3）。

实现现状：

- `Run()` 单独循环刷新 heartbeat（`md_gate_main.cpp:559` 起）。
- 连接失败时仍运行 heartbeat（`main()` 里 `ConnectTdf()` 失败也进入 `Run()`）。
- `HandleSystem()` 在 disconnect/login 结果时更新 `md_status`（`md_gate_main.cpp:675` 起）。
- 每条行情更新 `last_md_ns`（`md_gate_main.cpp:671`）。

偏离评估：

- **符合设计意图**（“进程活着但行情断了”可由 trade 侧区分）。
- **轻微偏离**：`last_err` 仅在 `TDF_OpenExt` 失败时写入；disconnect 等场景未写更细的错误码（可作为增强）。

---

## Phase 2：Technical Deep Dive（HFT 视角深入审查）

### 1) Hardware + OS Tuning（缺失项与建议）

现状（代码层）：

- 未设置：CPU 亲和性、实时调度优先级、mlock/预触页、NUMA 绑定、hugepage/THP 建议、核心隔离等。
- `Create()` 路径通过 `memset` 预触了 writer 侧页面（`shm_writer.cpp:233`），但 reader 侧无预触。

建议（按收益/风险排序）：

1. **CPU pinning**：将 TDF 回调线程与心跳/管理线程绑到不同物理核；若策略读端也在同机，隔离核避免干扰。
2. **内存锁定**：`mlockall(MCL_CURRENT|MCL_FUTURE)`（Linux）避免页错误；并对 shm 映射区做一次顺序 pre-touch。
3. **NUMA 策略**：同 NUMA node 部署 gateway/trade，或显式 `numactl --cpunodebind/--membind`。
4. **TLB/大页**：对 snapshot table 做 `madvise(MADV_HUGEPAGE)` 或启用 THP（谨慎评估）；条目总量 ~1.1MB，适合大页减少 TLB miss。
5. **时钟源与频率**：生产环境锁定 CPU governor=performance，禁用深度 C-state；必要时选用 TSC 稳定时钟并做校准。

### 2) Network ingress/egress path（TDF 接入路径）

现状：

- 网络接入由 TDF SDK 封装，应用侧仅设置 `TDF_SetEnv` 的超时/心跳参数（`md_gate_main.cpp:496` 起）。
- 未见 socket 级别调优（多数情况下 SDK 内部处理，但你需要确认）。

建议：

- 明确 TDF 数据通道类型（TCP/UDP/multicast）与线程模型；若是 UDP/multicast，重点检查：NIC RSS/IRQ 亲和、ring buffer 尺寸、丢包计数、SO_RCVBUF、busy-poll（`SO_BUSY_POLL`）等。
- 如策略对“到达时间戳”敏感：建议在 SDK 入口处采 `recv_ns`（当前已写入 payload，`md_gate_main.cpp:660`），并定义其语义（monotonic vs realtime；单机对比 vs 跨机对齐）。

### 3) Data model and serialization（POD/ABI/二进制布局）

检查点：共享内存结构体必须是 POD / 固定布局；不得包含 `std::string/std::vector` 等动态成员。

现状：

- `ShmHeader` / `SnapshotEntry` / `MarketData320` 均为纯 C 风格字段 + `alignas`，并用 `static_assert` 固定尺寸（`struct_def.h`）。
- payload 使用固定 320B 的 `MarketDataPayloadV1`（`marketdata_payload.h`），无指针/动态成员；写入时 memcpy 到 `MarketData320::bytes`。

结论：

- **POD/固定布局：通过**。不存在 `std::vector/std::string` 写入 shm 导致 ABI 破坏的问题。

风险与改进建议：

1. **严格 C++ 内存模型风险（UB 争议）**：SeqLock + 非原子 payload 在 ISO C++ 中可能被视为数据竞争（`struct_def.h` 已有注释）。如果要“标准语义严格无数据竞争”，建议：
   - entry 双缓冲（两个 payload + active/version），或
   - payload 按 8B 切块用原子读写（性能与实现复杂度需权衡）。
2. **跨编译器/跨语言**：当前 payload 由 C++ struct 直接 memcpy，默认假设同 ABI；若未来需要跨语言读（Rust/Go/Python），建议补充 `offsetof` 静态断言，或定义明确的序列化布局（字段偏移/字节序）。

### 4) Concurrency architecture（并发架构与抖动源）

现状：

- 设计稿建议的 “回调线程入队 + writer 线程批量 flush shm（单写者）” 未实现。
- 回调路径做了多处可能产生抖动的操作：
  - 全局 `g_map_mu_` 查找 self（`md_gate_main.cpp:589`、`md_gate_main.cpp:600`）。
  - `write_mu_` 串行化 + `std::unordered_map<std::string, uint32_t>::find` + `std::string` 构造（`md_gate_main.cpp:618`、`md_gate_main.cpp:621`）。
  - 每条记录多次 `memset/memcpy`（`md_gate_main.cpp:629` 起）。

判定：

- **偏离设计的核心点**：设计强调“回调不再写 map+mutex 缓存”；当前虽然最终写入 shm，但**仍在回调热路径使用 mutex+哈希表+字符串**，与 HFT 目标（低抖动）冲突。

建议改造路径（建议从低风险到高收益迭代）：

1. **消除 g_map_ 锁**：单连接场景下用 `static MdGateApp* g_app` 或 SDK 提供的 user-data（如有）替代 `unordered_map<THANDLE,*>`。
2. **消除 per-tick string/hash**：
   - 将 wind_code 解析为 “市场+6位代码” 的紧凑 key（例如 `uint32_t` code + 1 bit market），使用平坦数组/开地址哈希（固定容量 3000）。
   - 或预构建 `unordered_map<string_view, id>` 并在回调中用 bounded length 的 `string_view`（仍有 hash 成本，但避免分配）。
3. **明确回调线程模型**：
   - 若回调单线程：移除 `write_mu_`，避免锁抖动。
   - 若回调多线程：按设计上 SPSC/MPSC 方案实现 **单写者 writer 线程**（回调只 push symbol_id 或指向临时缓冲的索引）。
4. **写入路径复制次数**：允许直接写入 `SnapshotEntry::payload.bytes`（或 writer 线程合并写入），减少中间 `MarketData320` 的一次 copy。

### 5) Memory management（mmap/shm_open RAII、越界风险、生命周期）

#### 5.1 RAII 与资源释放

现状：

- `ShmWriter` / `ShmReader` 析构调用 `Close()`，对 `mmap/munmap` 与 `shm_open/close` 做了基本回收（`shm_writer.cpp:155`、`shm_reader.cpp:67`）。

结论：

- **RAII 基本通过**，未见明显 fd/映射泄漏路径。

#### 5.2 shm 创建/权限/覆盖语义

现状：

- `Create()` 使用 `shm_open(O_CREAT|O_RDWR, 0666)`，无 `O_EXCL`（`shm_writer.cpp:103`），并在映射后对整段 `memset` 清零（`shm_writer.cpp:233`）。

风险：

- **覆盖风险**：若 shm 名称已存在且 trade_app 正在读取，新启动的 gateway 会直接清零并重置 header，读端可能看到短暂的 “magic 不匹配/字段全 0” 状态；在未实现“读端自动重连/等待稳定”逻辑前，容易造成误判与策略抖动。
- **权限过宽**：`0666` 允许其他用户进程读写（在生产环境通常不期望）。

建议：

- 生产环境建议 `0600`（或由部署用户/组控制）；
- 若要避免覆盖：使用 `O_CREAT|O_EXCL` 或采用 “writer_start_ns epoch + 双 shm 名称切换/版本化命名” 策略；
- trade_app 侧应实现：检测 magic/version/epoch 的变化后延迟重试并重建映射（设计稿已要求，但代码未落地）。

#### 5.3 越界/变长消息处理（TDF）

现状：

- `HandleData()` 根据 `pAppHead->nItemCount` 遍历，并将 `pData` cast 为 `TDF_MARKET_DATA*`（`md_gate_main.cpp:608` 起）；**未校验** `nDataLen`、`nItemSize`。
- `std::string wind_code(m[i].szWindCode)` 假设 `szWindCode` 以 `\\0` 结尾；若 SDK 给的是非终止数组，可能读越界（概率取决于 SDK 保证）。

建议（强烈）：

- 在进入循环前增加以下防御性校验（非热路径分支，收益远大于成本）：
  - `msg->pAppHead != nullptr`
  - `msg->pAppHead->nItemSize == sizeof(TDF_MARKET_DATA)`
  - `msg->nDataLen >= sizeof(TDF_APP_HEAD) + count * sizeof(TDF_MARKET_DATA)`（或按 SDK 定义修正）
- 对 wind_code 读取使用 bounded length（`strnlen` + 拷贝到固定 buffer），避免依赖 `\\0` 终止。

#### 5.4 symbol_id 越界写

现状：

- `ShmWriter::UpdateSnapshot()` 不检查 `symbol_id` 合法性（`shm_writer.h:45`），一旦上游传入错误 id 将直接写越界。

建议：

- 在 debug 构建加入 `assert(symbol_id < header_->symbol_count)`；
- 或在 release 构建加入轻量分支保护（牺牲极小吞吐换取 “不内存破坏”）。

---

## 建议的验收补充（便于下一阶段落地）

1. 增加 `trade_app` 侧最小示例：`Open() + ValidateHeader() + ReadSnapshotSpin()` + heartbeat/epoch 检测状态机（对应设计稿 3.4/3.5/6.2）。
2. 明确并记录 TDF 回调线程模型；据此选择 “无锁直写” 或 “入队+单 writer”。
3. 为 hot path 建立基准：在 3000 标的、全市场快照频率下测 `p99/p999` 回调处理时间与 `trade_app` 读取尾延迟；以数据驱动去除 mutex/string/hash。

