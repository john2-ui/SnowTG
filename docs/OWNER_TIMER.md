# Owner-local timer 后端

`owner_timer_*` 是 TCP、socket command、workflow 和 flow 共用的单 owner 定时器接口。
默认后端为 `rte`；可选择 `wheel`，不增加运行参数或场景字段。

```bash
make -C traffic-gen -j8 OWNER_TIMER_BACKEND=rte BUILD_DIR=build-timer-rte
make -C traffic-gen -j8 OWNER_TIMER_BACKEND=wheel BUILD_DIR=build-timer-wheel
make -C test -j8 OWNER_TIMER_BACKEND=wheel BUILD_DIR=build-timer-wheel
```

`test` 会编译并执行测试；`test-owner-timer` 沿用原有行为，只编译该单测。
同一个构建目录切换后端也会检查后端 stamp、重编译 timer 对象并重新链接消费者。
不要并行运行使用相同 DPDK file-prefix 的两个测试套件。
`traffic-gen.build.json` 的 `owner_timer_backend` 与二进制哈希一起记录实际构建选择。
头文件布局不依赖构建开关，但新增内部字段后仍需重新编译所有消费者，不承诺旧二进制 ABI。

## 调度语义

- engine 初始化可以在启动 worker 前完成；只有绑定 lcore 可挂载、取消和轮询。
- 时间轮有 8 层，每层 256 桶，以绝对毫秒 tick 的最高不同字节选择层。
  桶占用位图跳过空时间段；长时间停顿后只处理相关桶和节点。
- 未来截止时间向上取整到毫秒边界，不提前触发。已到期的挂载在下一次 poll 执行；
  回调中立即重挂也留到下一次 poll，避免单次轮询无限执行。
- worker 每 1 ms 轮询，两种后端一致。时间轮量化延迟小于 1 ms，另加轮询、
  worker 调度和其他回调耗时；它不提供硬实时保证。
- 节点在回调前脱离调度器并扣减 active，回调可以释放自身、重新挂载或取消其他节点。
  到期回调之间不提供稳定排序保证。
- engine 在初始化时分配约 17 KiB 的轮结构；热路径没有内存分配和跨核锁。
  到期节点仍计入 capacity，直到执行或取消。退出时取消剩余节点并释放轮结构。
- cycle 截止时间保存在节点中，毫秒换算和 `arm_after_ms` 保持饱和语义。

## flow 超时

TCP/UDP 事务、TCP idle、等待对端 FIN 使用每个 flow 内嵌的一个 timer。
定时器回调仅将 flow 加入所属 map 的到期队列，`tg_flow_expire()` 在 ready event
处理后消费该队列，维持现有事件优先级。没有周期性 socket-map 超时扫描。

重用、解除映射和回池同时撤销已调度及已入队的超时；完成通知和错误分类沿用原有状态机。
`tg_flow_expire()` 的 `now_cycles` 是当前 owner 时间；调用它不会推进 timer 时钟。
单独使用 flow API 的调用者须先初始化 owner engine，并在处理到期队列前轮询 engine。
只修改 `deadline_cycles` 不会重新调度超时；workflow 设置步骤期限时同时调用
`owner_timer_arm_at()`，覆盖新建和重用 flow 的默认期限。

traffic-gen 的 engine 容量为原 socket/workflow/drain 额度加 flow pool 容量，计算时检查溢出。
flow timer 接纳失败会撤销已创建的 socket、映射和 pool 对象，不留下无超时保护的 flow。

## 验证入口

```bash
# 可控时钟测试：跨层、长时间跳跃、取消/重挂、回调内释放以及随机操作对照。
make -C test BUILD_DIR=build-timer-wheel test-owner-timer-wheel

# 使用独立目录执行 sanitizer 套件。
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -C test -j8 OWNER_TIMER_BACKEND=wheel \
  BUILD_DIR=build-timer-wheel-asan SANITIZERS=address,undefined

# 无需网卡的真实 workflow 时钟/步骤超时集成检查（两种后端均应执行）。
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  make -C test OWNER_TIMER_BACKEND=wheel BUILD_DIR=build-timer-wheel-asan \
  SANITIZERS=address,undefined test-workflow-cli

# 可选微基准；替换 wheel 为 rte 做对照，轮换运行顺序并固定 CPU。
make -C test OWNER_TIMER_BACKEND=wheel BUILD_DIR=build-timer-wheel \
  build-timer-wheel/bench_owner_timer
test/build-timer-wheel/bench_owner_timer -l 2 --no-huge --no-pci \
  --file-prefix timer-bench-wheel
```

微基准输出 timer cycles，不等同于硬件核心 cycles；它不能单独决定默认后端。
双机 profile、默认切换门槛与结果见 [性能验证](benchmarks/owner-timer/README.md)。
