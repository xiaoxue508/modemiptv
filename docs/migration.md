# 迁移记录：iptvd（路由器）→ modemiptv（光猫）

背景与裁剪依据。设备：中国联通青岛 IPTV，中兴 ZX279511 ONU。
拓扑：光猫 `192.168.1.1`（LAN br0）↔ 路由器 OpenWrt `192.168.1.10`
（`network.modem`，物理同口）；路由器 LAN `192.168.123.1/24`，
PPPoE 上网。播放器/r2h 在路由器侧。

## 为什么搬

- r2h 需要**平台签名**（channels 表 2h 刷一次、DES 签名 AuthInfo），
  以及 EPG/回看签名，全部依赖「源 IP ∈ IPTV 网段」的平台鉴权。
- 52 台源绑定频道（池B 11 + 池C 41）要求 RTSP 连接源 ∈ IPTV 子网。
- 路由器已无 IPTV 地址（VID2574 桥接删除、lan4 退役）；
  **光猫 LAN→IPTV 转发被厂商焊死**（转发流量被以 ICMP admin-prohibited
  在桥层拦截，FORWARD 计数 0，iptables 无法改动）——数据面中继必须
  在光猫本机发起（本期先不做中继，只迁控制面）。
- 光猫新建 IPoE 路由连接 `nbif1 = 10.156.31.108/22`，gw 10.156.28.1，
  平台/CDN 全部可达（wget 实测 TCP 可达）。

## 裁剪表（相对 iptvd 0.1.0）

| 项 | 路由器版 | 光猫版 | 原因 |
|---|---|---|---|
| ka 保活线程（ping + ifdown/ifup 弹跳） | 有 | **删除** | 光猫 ping 无 `-W`；无 ifup/ifdown；链路由 cspd 自管 |
| `bounce_iface` 配置项 | 有 | **删除** | 随 ka 删除 |
| uplink 策略路由（table1001 + pref1000 + 5 条平台 /24） | 默认开 | **`uplink_policy=0` 关** | 见下「路由」 |
| `uplink_gw()` | 有 | **删除** | 仅 ka 使用 |
| 5 精确路由/默认路由写入 | 每 60s 重写 | **不写** | ZTE 已维护 table12 |
| 默认路径 | `/etc/iptvd`、`/etc/iptvd.conf` | `/userconfig/iptv`、`/userconfig/iptv/iptvd.conf` | /etc 只读 |
| m3u/EPG/回看/生成器 URL | `192.168.123.1:5150` | `192.168.1.1:5150` | 服务端换位；`r2h` 仍是路由器 `192.168.123.1:5141` |
| session 文件名 | `session_router.json` | `session.json` | 新部署，无兼容包袱 |
| luci-app / ipk / OpenWrt init | 有 | **不迁** | 光猫无 procd；改用 rcS 钩子 |
| 中继模块（relay.c，52 台救回） | — | **本期不做** | 用户指示先跳过 |
| 登录/签名/频道表 TTL 重刷/EPG/回看/:5150/CLI 子命令 | 有 | 保留 | 控制面核心 |

## 路由（光猫侧零路由）

- `ip rule 20014: from 10.156.31.108 lookup table.IGD.WD1.WCD4.WCIP1`
  （ZTE/cspd 自动维护，连接变更自动换表）。
- 该表（table 12）自带 `default via 10.156.28.1 dev nbif1` +
  平台/CDN 全部网段（60.212.113/114/115.x、124.132.240/241/242.x、
  119.180.21.x 等）。
- 因此 `uplink_policy=0`：`uplink_ensure()` 只做
  ①取 `nbif1` 地址（http.c `CURLOPT_INTERFACE` 绑源）②`stbip=auto`
  回填 `10.156.31.108`。**不写任何路由/规则**，cspd 的表也不被干扰。
- 路由器版的 `uplink_policy=1` 代码保留（备胎），默认 0。
- 遗留的手工路由（测试期 `ip route add 124.132.240.0/24 ...`）只在
  RAM，重启即消，可不管。

## 设备事实（实测）

- armv7l，`CPU Features: swp half fastmult edsp tls` —— **无 VFP**，
  必须软浮点（`arm-linux-musleabi`，CI 里 readelf 拒绝 `VFP_args`）。
- libc uClibc 0.9.32.1，**无 libcurl** → 静态链接（musl + 静态 curl/zlib）。
- busybox 1.17.2 applets：有 `wget/tftp/tar/gzip/awk/sed/hexdump/grep/sort`；
  **无 `head/dd/nc/base64/md5sum/telnet/iptables`**，`ping` 无 `-W`（有 `-T`）。
  输出截断用 `sed -n '1,40p'` 或 `awk 'NR<=40'`。
- 存储：
  - `/` rootfs jffs2 **ro**（`mount -o remount,rw /` 可开，32640k，8.3MB 余）
  - `/userconfig` mtd5 jffs2 **rw**（7.5MB 余）→ 二进制/配置/session/channels
  - `/usr/local/ct` mtd6 jffs2 rw（3.7MB 余，目前空）
  - `/var` tmpfs 20MB；`/tmp` 可写（指向 /var/tmp）→ EPG 缓存、日志
  - `/usr/tmp` jffs2 **ro**；`/upgtempfile` tmpfs 100MB（升级用，别碰）
- 启动链：`inittab sysinit → /etc/init.d/rcS`：
  `mount -a → defaults → S00tagparam → S01userconfig（挂 /userconfig）→
  for /etc/rcS.d/S??*（S02wlan/S20tsmac/S30network/S43BSPDriver/S99modules）
  → 厂商配置DB恢复 → pc&（cspd）`。
  **rcS.d 与 /etc 均 ro** → 钩子靠一次性 remount-rw 写入 `S99iptv`
  （文件本身落到 flash，之后 /etc 回 ro，重启由内核 cmdline 再 ro）。
  无 crond。cspd 在 rcS.d 循环**之后**才起 → nbif1 晚于 S99iptv，
  由 uplink_thread(60s)/channels worker 自愈重试。
- vsftpd 在跑（`vsftpd -max_client 5 -max_per_ip 5 -max_rate 250000`），
  用户配置 `/var/tmp/ftp_password.log`（user=MD5, userRight 曾为 READONLY，
  实验改 READWRITE 后仍 530 —— 密码未知/读别处），**FTP 上传通道放弃**。

## 传输通道（光猫没有 sshd/nc/base64，怎么把二进制送进去）

穷举结论：

| 通道 | 结果 |
|---|---|
| modem→路由器 TCP | ✗ zone-wan/input 实测 TCP connect fail（ICMP 反而通，原因未深究，不影响） |
| modem→PC（192.168.123.218） | ✗ `Network is unreachable`（经网关路由异常，未修） |
| 路由器 uhttpd docroot 放 /www | ✗ 路由器红线：不写 flash |
| FTP 上传（vsftpd :21） | ✗ 匿名 530、已知凭据 530（密码未知） |
| 路由器 → modem telnet 通道 | ✓ **采用**（telnetd 长连，双向流） |

**采用方案：telnet 流式推送**（`deploy/push2onu.py`）：
登录后 `stty raw -echo`，协商 telnet BINARY（否则 0x00/0x0D/0xFF
无法携带），数据侧做 IAC 双写；对端 `tar -x`（tar 尾部双零块自终止）
或 `head -c`/`wc -c` 校验后 `stty sane` 回显恢复。
兜底：`stty -echo` + 十六进制行 + busybox `printf` 内建解码
（每行 ~800 字节，无 tty 上限问题）。

## 路由器侧集成（联调清单）

- r2h `external_m3u = http://192.168.1.1:5150/full.m3u`
- 播放器列表 `http://192.168.1.1:5150/playlist.m3u`（组播经 r2h）
- **清空 r2h `upstream_interface_rtsp`**（原 `pppoe-wan` 绑源会导致
  连不上 192.168.1.1，由路由表决定出口即可）
- 通路：r2h(路由器本机)→192.168.1.1 = 路由器 OUTPUT ✓；
  播放器(LAN)→192.168.1.1 = forward lan→wan ACCEPT ✓（WebUI 80 同路已证）

## 未决问题（部署后按序验证）

1. 平台是否接受 stbip/源 = 10.156.31.108 的登录（第一道关，`iptvd login`）。
2. `/full.m3u` 单播组的签名 URL（userip=10.156.31.108）经 r2h 从 pppoe
   源访问：池A 121 是否裸链/全参都能通；不通则单播组退回裸链形态
   （121 通、52 仍 401，等中继期）。
3. EPG 首次全量构建时长（ttl_epg=6h，xmltv_wait_s=180 可调）。
4. 重启回归：S99iptv → cspd → nbif1 → 自动登录 → :5150 可达。
