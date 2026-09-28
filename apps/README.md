# 协议栈应用示例

这些应用链接 `pro-stack`，使用公开 `n*` API；不是 Linux 内核 socket 的包装。网卡收发、ARP 和 TCP 定时器必须由 packet owner 持续推进。接口及限制见 [公开 socket API](../docs/SOCKET_API.md)。

| 目录 | 用途 |
| --- | --- |
| `stack-demo/` | 原有启动入口：Main 桥接 NIC/ring，独立 worker 处理协议，应用运行在额外 EAL lcore。 |
| `tcp-echo/` | 非阻塞并发 TCP echo 服务端与阻塞客户端，处理短读短写。由 stack-demo 编译。 |
| `udp-echo/` | 阻塞 UDP echo，使用完整数据报缓冲区。由 stack-demo 编译。 |
| `socket-demo/` | 单 owner + 普通 pthread，公开非阻塞 API、水平触发 `nepoll`、并发 TCP echo、UDP echo、异步 connect。 |

## 构建

从仓库根目录运行，先更新公共库，再链接示例：

```sh
make -C pro-stack library
make -C apps/stack-demo
make -C apps/socket-demo
```

DPDK 环境准备沿用根目录 README。`socket-demo` 使用端口 0、单个 RX/TX queue、MTU 1500；只需要一个 EAL lcore，应用使用 pthread。IP/端口由应用参数提供，不修改 `config.h`。`LOCAL_IP` 必须是测试链路上给本协议栈使用的独立 IPv4 地址。

## 非阻塞示例

以下 PCI 地址、CPU 编号、IP 和端口均需替换为实际环境。程序不会绑定驱动、添加路由或配置内核地址；`--` 前为 EAL 参数，之后为应用参数。每条命令单独运行：

```sh
# TCP echo：至多 32 个并发连接，用户态每连接缓存 4 KiB。
./apps/socket-demo/build/socket-demo -l 2 -a 0000:01:00.0 -- \
  tcp-echo 192.168.10.200 9000

# UDP echo：包括空数据报；不支持发送 IPv4 分片，超 MTU 回复会被明确丢弃。
./apps/socket-demo/build/socket-demo -l 2 -a 0000:01:00.0 -- \
  udp-echo 192.168.10.200 9001

# TCP 客户端：对端须提供 echo 服务；发送一条消息，校验回复后退出。
./apps/socket-demo/build/socket-demo -l 2 -a 0000:01:00.0 -- \
  tcp-client 192.168.10.200 9000 192.168.10.234
```

服务端按 Ctrl-C 或发送 SIGTERM 退出。信号处理器只设置停止标志；应用先关闭 fd 和 poller，再停止 owner，避免关闭命令无人执行。客户端每次无进展等待上限为 5 秒，失败返回非零退出码。服务器是教学示例，没有连接空闲超时；达到 32 个连接时关闭新接入连接。

`main.c` 中三个应用函数可以分别阅读：

- `tcp_server`：`naccept4(..., SOCK_NONBLOCK)`、`ACCEPT`、用 `event.data` 定位连接、短写偏移及缓冲区背压。有待发送字节时才订阅 `WRITE`；对端 FIN 后先读完、回送剩余数据。
- `tcp_client`：首次 connect 的 `EINPROGRESS`、等待 `CONNECTED/ERROR`、读取并清除 `SO_ERROR`。就绪只是提示，实际读写仍必须处理 `EAGAIN`。
- `udp_server`：读完一份数据报后再接收下一份；发送拥塞时保留内容和对端地址，避免把空数据报误认成 EOF。

默认 `TCP_NODELAY=1`，示例不重复设置默认值。服务端在 bind 前设置 `SO_REUSEADDR`。`nepoll` 只支持水平触发，`ERROR/HUP` 无需显式订阅；HUP 不代表缓冲区已经读空。

## 对端检查

`check_peer.py` 只使用 Python 标准库，可复制到同一链路上的对端主机执行：

```sh
# 分别配合正在运行的 tcp-echo / udp-echo。
python3 apps/socket-demo/check_peer.py tcp-echo 192.168.10.200 9000
python3 apps/socket-demo/check_peer.py udp-echo 192.168.10.200 9001

# 先在对端启动一次性 echo 服务，再运行上面的 tcp-client。
python3 apps/socket-demo/check_peer.py client-peer 192.168.10.234 9000
```

TCP 检查先确认一个连接完成 echo，再保持它空闲，同时让另外 8 个并发客户端各回送 128 KiB+17 字节，覆盖小块读取和半关闭；UDP 检查包含空包及 1280 字节报文。它们是功能检查，不是吞吐基准。

没有可用网卡时，可用 `--no-huge --no-pci -m 256 --vdev net_null0` 替换 PCI 参数，检查服务端启动和信号退出；null PMD 不会回送应用报文，不能用于回显验证。

## 桥接架构示例

`stack-demo` 默认启用 TCP 服务端，地址为 `192.168.21.2`，端口为 8888；至少需要 3 个 EAL lcore（Main、owner、TCP 应用）。UDP/客户端开关和 TCP 端口位于 `pro-stack/config.h`，启用额外应用须再提供一个 lcore。UDP 默认端口为 8889。

TCP 服务端使用公开非阻塞 API 与水平触发 `nepoll`，单个应用 lcore 最多服务 32 个活动连接，监听 backlog 为 16。每连接保留 1280 字节回送缓冲；有待发送数据时暂停读取，短写或 `EAGAIN` 后等待写就绪，不阻塞其他连接。每轮最多接受 32 个连接，每个连接事件最多读、写各一次；容量满时关闭新接入连接，没有空闲超时。对端 FIN 后先读完并回送剩余数据，再关闭连接。

TCP 客户端仍使用阻塞调用并循环处理短写。UDP 接收缓冲区覆盖最大 IPv4 UDP payload，避免本栈的短读续取语义把一份数据报拆成多次 echo。原入口继续演示桥接架构；需要可配置地址或信号退出时使用 `socket-demo`。

对端可用同一检查脚本验证原入口的并发 TCP 服务端：

```sh
python3 apps/socket-demo/check_peer.py tcp-echo 192.168.21.2 8888
```

无网卡的确定性回归测试运行实际服务端源码，模拟公开 API 的就绪、短写、背压、EOF、RST、连接复用和失败清理：`make -C test test-tcp-echo`。此测试也包含在默认 `make -C test` 中；真实链路回显需要另行运行上述对端检查。
