# iptvd

山东联通 IPTV 控制面的路由器原生实现（C），替代原 NAS 容器里的
`epg.py` + `srcbox_bridge.py` + `gen_full_m3u.py`。

- 目标平台：ImmortalWrt 24.10.4 `mediatek/filogic`（aarch64_cortex-a53）
- 依赖：`libc`、`libcurl4`
- 数据目录：`/etc/iptvd`（session/channels，持久）+ `/tmp/iptvd`（EPG 缓存，易失）
- 配置：`/etc/iptvd.conf`（key=value）
- 端口：默认 5150（黄金对照期用 5160）

## 构建

GitHub Actions（`.github/workflows/build.yml`）自动完成：

1. `unit-test`：宿主机 gcc 编译 + `iptvd selftest`
   （port-spec T1/T2 sign 黄金向量、GBK、JSON dump、quote_plus、UTC+8 时间）；
2. `build-ipk`：下载官方 24.10.4 mediatek/filogic SDK →
   `feeds update packages` + `feeds install libcurl` →
   `make package/iptvd/compile` → 上传 `iptvd_*.ipk` artifact。

本地手动构建（任意 x86_64 Linux + SDK）：

```sh
./scripts/feeds update packages && ./scripts/feeds install libcurl
make defconfig
make package/iptvd/compile -j$(nproc)
```

## 安装

```sh
opkg install iptvd_*.ipk
/etc/init.d/iptvd enable && /etc/init.d/iptvd start
iptvd selftest
```

## 子命令

`login` / `channels` / `programs` / `tvod` / `live-sdp` /
`playlist` / `epg` / `serve` / `status` / `sign` / `selftest`

移植规格与黄金对照清单见 `docs/port-spec.md`（私有仓库侧）。
