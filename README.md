# modemiptv

ZTE ZX279511 ONT（armv7 软浮点、kernel 3.4、uClibc 时代设备）上的
IPTV 控制面守护进程：EAS 登录/DES 签名/频道表/EPG/回看签名/M3U，
以 HTTP `:5150` 服务路由器侧的 r2h 与播放器。

代码 fork 自 `iptvd`（路由器版，OpenWrt/immortalwrt ipk + luci-app），
按光猫场景裁剪，见 [docs/migration.md](docs/migration.md)。

## 构建（GitHub Actions）

push 即触发 `.github/workflows/build.yml`：

1. **host-test**：ubuntu gcc + libcurl 编译，跑 `selftest`
   （DES 签名 golden 向量、GBK、JSON、urlencode、UTC+8 时间）。
2. **build-arm**：musl.cc `arm-linux-musleabi`（**软浮点**，设备无 VFP）
   静态编译 zlib + libcurl(仅 http) + `iptvd`，
   `readelf -A` 拒绝 hard-float 产物，`qemu-arm-static` 冒烟自测。

产物：Artifacts → `iptvd-armv7/`
（`iptvd-armv7` 二进制、`iptvd.conf`、`S99iptv`、打包 tar.gz）。

## 部署

目标机文件布局：

```
/userconfig/iptv/            # mtd5 jffs2 常年 rw，7.5MB —— 二进制+配置+session/channels
/tmp/iptvd/                  # tmpfs —— EPG 编译产物（易失）
/var/iptvd.log               # tmpfs —— 守护日志（重启清零）
/etc/rcS.d/S99iptv           # 开机钩子（rootfs 只读，一次性 remount-rw 写入）
```

步骤：

1. **推文件**：`deploy/push2onu.py <本地文件> <远端路径>`
   （telnet 流式通道，原理见 migration.md「传输通道」）。
   把 `iptvd-armv7` → `/userconfig/iptv/iptvd`（chmod +x）、
   `iptvd.conf` → `/userconfig/iptv/iptvd.conf`。
2. **装开机钩子**（一次性，rootfs 会回到只读）：
   ```
   mount -o remount,rw /
   # 推 S99iptv -> /etc/rcS.d/S99iptv, chmod +x
   mount -o remount,ro /
   ```
3. **手起**：`/userconfig/iptv/iptvd --config /userconfig/iptv/iptvd.conf serve &`
4. **首测**：`iptvd login`（平台是否接受源/stbip=10.156.31.108 是第一道关），
   `iptvd status`，PC `curl http://192.168.1.1:5150/status`。

## 运维

| 事 | 命令（telnet 光猫） |
|---|---|
| 状态 | `/userconfig/iptv/iptvd status`（或 `--json`） |
| 日志 | `tail -20 /var/iptvd.log`（无 head，busybox） |
| 重启 | `killall iptvd; /userconfig/iptv/iptvd ... serve &` |
| 改配置 | `iptvd config set key=val`（或直接改 iptvd.conf） |
| 手动刷表 | `iptvd login` → `iptvd channels` |
| 路由器侧收听 | r2h `external_m3u=http://192.168.1.1:5150/full.m3u`；播放器列表 `http://192.168.1.1:5150/playlist.m3u` |

坑：

- r2h `upstream_interface_rtsp='pppoe-wan'` 必须清空，否则连不上 192.168.1.1。
- 光猫 busybox 无 `head`/`nc`/`base64`/`md5sum`，`ping` 无 `-W`。
- rootfs（/etc）只读：`mount -o remount,rw /` 可临时打开，用完恢复 ro。
