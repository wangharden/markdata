trade_app 设计目的：不连 TDF → mmap 打开 shm → 校验 Header → 每秒/每 3 秒按 symbol_id 读快照（SeqLock）→ heartbeat/epoch 异常降级


引入新的行情适配器：实现一个 ShmMarketDataApi 类继承自 IMarketDataApi。这个类在 connect() 中通过名字打开 md_gateway 创建的共享内存段，验证 header 中的 magic/version 和 symbol_count，并把符号目录映射成一个 std::unordered_map<symbol, index> 供后续查找。成功映射后返回 true，失败则返回 false。

读取快照：ShmMarketDataApi::get_snapshot(symbol) 根据符号映射找到对应的 snapshot entry，然后用 seqlock 协议读这条快照：先检查 entry.seq 为偶数再复制 320B 的 payload，再次检查 seq 是否未变；如果 seq 是奇数或前后不一致则重读，保证读到的数据是一致的。读出的 payload 填充成现有的 MarketSnapshot 结构，并直接返回。get_limits(symbol) 直接从快照的 high_limit 和 low_limit 字段返回涨停/跌停价。

连接状态和心跳：is_connected() 读取 header 中的 heartbeat 时间戳，与本地时间比较，心跳在阈值内则认为网关活着。disconnect() 仅需 munmap 共享内存。

不再订阅行情：之前的 TdfMarketDataApi 通过 CSV 订阅并在内部维护一个 snapshot_cache_ 映射；现在订阅、回调和快照更新都放在 md_gateway，所以交易端不需要调用 set_csv_path() 或 subscribe()。main 启动时直接构建 std::shared_ptr<ShmMarketDataApi> market，调用 market->connect(shm_name, 0, "", "") 即可。

更新上下文和模块：AppContext 中的 market 指针从 TdfMarketDataApi 改为 ShmMarketDataApi。各策略通过 api_->get_snapshot(symbol) 获取行情时走的是新的共享内存实现；接口签名不变，因此 TradingMarketApi、TradingManager、CloseSellStrategy 等无需修改业务逻辑，只需在创建 API 时注入新的 ShmMarketDataApi 实例。

其他接口：不需要实现 get_history_ticks() 或 get_auction_data() 等暂未用到的接口，可以返回空数据或 std::pair{0,0} 保持兼容。