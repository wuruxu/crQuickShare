# crQuickShare

基于 Chromium 154 的独立 Linux 桌面应用，面向 ChromeOS Quick Share 的局域网文件互传。
主窗口参考 crPlayer：Chromium Views、自定义 36 DIP 标题栏、libcr 最小化／最大化／还原／关闭按钮、拖动与缩放。

当前状态：已实现源码、GN 目标和语言资源，通过 GN 依赖检查与资源校验。
**尚未编译、启动或完成 ChromeOS 实机互通验证，不应视为已经验证兼容的发布版本。**
本目录是独立 Git 仓库。共享模式另有 libcr 和第三方库 GN 配置改动；业务实现仍保留在本目录。

## 首版范围

- IPv4 同一子网，通过 Quick Share 的 `_FC9F5ED42C8A._tcp.local` mDNS 服务发现设备。服务类型使用 `SHA256("NearbySharing")` 的前 6 字节；实例名内部仍使用前 3 字节，二者长度不同。
- 使用 Nearby P2P Point-to-Point 广播格式、endpoint info 和 TCP 长度帧。
- 复用 Chromium 中的 Nearby Connections / Sharing protobuf，以及 UKEY2 P256/SHA512 和 D2D 加密通道；没有自行实现密码算法。
- 双向多文件传输。每次最多 100 个普通文件，总大小上限 1 TiB；不发送目录和符号链接。
- 无 Google 登录；交换匿名 paired-key 消息及 `UNABLE` 结果，然后比较四位验证码，手动同意分享。
- 默认不接收；点击接收后对局域网所有人可见 10 分钟，期间可以停止接收。会话进行时暂停广播，结束后恢复剩余可见时间。
- 一次一个会话，后台处理网络和文件 IO，界面显示文件列表、字节进度、取消和错误状态。
- 接收前验证元数据；确认后检查空间并在独立隐藏临时目录写入，完整接收后不覆盖地改名为 `Transfer-<random>`。拒绝、失败、取消时清理当前未提交目录。进程异常退出后可能留下 `.incoming-*` 目录；不会自动恢复或打开它们。
- 对方支持 payload ACK 时显示传输完成；旧对方在发送结束后关闭连接但不发 ACK 时，单独显示“已发送，未确认送达”。

当前提供 BLE Fast Initiation 辅助唤醒及有限格式的 BLE 广播发现，支持 GATT 广播信息读取，不含 Bluetooth 文件传输、Wi-Fi Direct、热点、WebRTC、中继、联系人、Google 账号、自动接受、断点续传、IPv6、空目录／文本／Wi-Fi 密码分享。无需访问 Google 后端。
网络接口每 3 秒检查，并监听 Linux 链路、IPv4 地址及路由变化；休眠恢复后也会重新加入
mDNS 组播、清除旧网络发现缓存并恢复查询和接收广播。没有可用接口时等待网络恢复，
初始化失败时自动重试。正在传输的通道不因发现重建而主动关闭；实际断网仍会使传输
失败，不提供断点续传。路由器 AP 隔离、组播过滤和主机防火墙会影响发现与传输；需要
UDP 5353 和应用实际监听的 TCP 端口可达。不会自动修改防火墙。

## Linux BLE 设备发现

主窗口可见且没有传输或文件选择对话框时，通过 Chromium BluetoothAdapter / BlueZ
扫描 BLE；不要求 LAN 已就绪，也不要求先选择文件。crFiles 的 Quick Share 弹窗复用同一模块。
仅蓝牙发现的设备显示对应标记，发送按钮禁用；实际发送仍需 LAN 发现提供可用地址。
缺少公开设备名称时，使用同一 BLE 地址对应的 BlueZ Name 作为显示备用；两者都没有时使用本地化通用名称。系统名称不参与端点识别或身份验证。

Fast 标志只用于选择紧凑字段布局，不再据此套用传统广播的 27 字节上限；已观察到
92 字节且 Fast 标志置位的服务数据。按包内长度解析，整体仍限制 512 字节，endpoint info
限制 131 字节，并校验名称及扩展字段。这是兼容性放宽，不代表该 92 字节广播已通过完整解析。

当前解析 FEF3 的 medium V1/V2 Fast Advertisement，以及广播服务数据中直接携带的
完整普通 Advertisement；内部要求 V1 点对点端点结构与有效 Sharing endpoint info。
普通广播还会检查两层 NearbySharing 服务哈希。FE2C 唤醒和仅 UUID 不作为
已识别接收设备显示。支持 V2 原始或 Base64URL 编码的 GATT 索引：先检查 NearbySharing
Bloom filter，再连接 FEF3 服务，读取 `00000000-0000-3000-8000-00000000000N`
广告槽位（N 从 0 起，以十六进制编码）。读取结果仍须通过服务哈希和端点格式校验。
索引本身不作为设备显示；无索引的匿名广播不触发猜测性 GATT 连接。

FC73/DCT V1 解析一字节／两字节 Data Element，验证 NearbySharing 服务哈希、非零
PSM、必需字段、重复字段、UTF-8 名称和长度边界。名称截断时附加省略号。匹配服务的
DCT 设备作为独立 Bluetooth-only 候选显示，每地址一条、最多 128 条、120 秒过期。
其本地 ID 仅用于界面缓存，不伪造 FEF3 的 salt/key hash，也不按同名合并到 LAN；同一
物理设备同时广播 DCT 和 FEF3 时可能显示两个候选。DCT 不提供 LAN 地址，不启用发送。

广播事件中的 FEF3/FC73 服务数据直接参与解析，不要求与 BlueZ 最新缓存一致，避免
交替广播集丢失。FEF3 大于传统紧凑布局大小的服务数据仍受 512 字节总上限约束；
已知 PSM 和 instant-connect 扩展验证边界，但不建立对应连接。仅处理系统交付的完整
服务数据，不自行拼接缺少分片标识的原始片段。

未知 medium/socket/Sharing/DCT 版本拒绝；FEF3 未知扩展位无法确定字段长度时拒绝。
有明确长度的未知 DCT DE 和 Sharing TLV 可跳过，必需字段仍须完整有效，vendor TLV
必须为一字节。`BLE format diagnostics` 按格式与原因分别累计、每类最多每 30 秒输出
一次；只输出原因、长度及计数，不输出广播内容或名称。汇总增加 dct_recognized、
dct_endpoints、extended_recognized、parsed_extensions。extended_recognized 表示服务
数据超过 27 字节，并不证明底层空中包一定采用 Bluetooth Extended Advertising。

一次仅解析一个设备，最多缓存 128 个候选，每次最多按索引读取 15 个槽位，总截止时间
15 秒。连接之间至少间隔 1 秒；同一索引失败或部分失败后 30 秒重试，所有槽位读取完成且无读取错误时 60 秒后刷新。
索引变化立即撤销旧槽位缓存，并允许提前刷新，但同一地址距上次连接至少间隔 5 秒。
超时后如果系统连接回调尚未返回，等待该回调释放连接，避免堆积连接请求。退出扫描、
设备移除、地址或索引变化时终止当前解析，旧请求回调不能覆盖新结果。仅释放本应用的
GATT 引用，使用 Chromium 的连接生命周期管理，不额外调用设备级强制断开接口。

GATT 结果逐槽缓存，广播索引的最近出现时间与槽位成功读取时间分别计时，两者均须在
120 秒有效期内。持续收到索引不会为读取失败的旧内容无限续期。刷新时读取失败的槽位
保留原结果及原到期时间；成功读到空值或无法识别的内容则清除该槽位。超时或断线前
已经读出的有效槽位可以提交，但索引变化、设备移除、停止扫描及 Instant On Lost
会丢弃该请求的结果。跨索引不复用旧身份信息，也不以同名或同地址强行关联。

日志 `BLE GATT header discovered`、`connect requested`、`endpoint resolved`、
`resolution finished` 分别定位索引、连接、成功解析和结束原因；汇总包含 gatt_attempts、
gatt_reads、gatt_candidates、gatt_endpoints、gatt_named_endpoints，以及槽位过期数
gatt_expired_slots、部分刷新数 gatt_partial_refreshes。结束日志记录 read_failures、
slots_updated、slots_cleared；端点计数按缓存槽位统计，界面仍按协议信息去重。没有公开名称的
GATT 记录也可使用同地址的 BlueZ 名称作为显示备用，不会绕过联系人或账户验证。DCT/L2CAP 和 GATT 文件传输仍未实现。
其他格式日志为 `BLE service data ignored`，每 30 秒最多一次。

记录在 120 秒无有效更新后过期，最多缓存 128 个蓝牙地址。端点 ID、盐值、元数据键哈希
及除公开名称标志外的头部位均匹配才去重或关联 LAN 记录，不按名称猜测身份，也不代替验证码确认。
属性通知中服务数据与 RSSI 都没变化时不续期；带匹配 FEF3 数据的 EIR 广播通知可续期。
BlueZ 没有在所有环境中提供逐包通知，因此这仍是基于系统通知的活跃度判断。
Instant On Lost 使用完整 medium 广播 SHA-256 的前 4 字节匹配并移除记录，抑制 60 秒内
旧广播重新出现。GATT 使用已读取槽位的哈希匹配，再撤销对应索引和解析请求；不把
GATT 汇总哈希误当成单条广播哈希。未读取的槽位若命中已知丢失哈希，也不会发布到界面。
`BLE instant on lost` 输出移除数量；汇总中的 `lost_removed`、`lost_suppressed`、
`property_only` 分别统计移除、抑制和未导致续期的重复属性通知。
扫描失败延迟 15 秒重试，蓝牙关闭或移除时清空记录；关闭扫描会释放本应用的 discovery
session，不停止其他应用的扫描。不会主动开启蓝牙或配对。关联诊断记录蓝牙地址、端点 ID 与匹配结果，不输出原始广播、名称或密钥材料。

实机验证：连接和断开局域网分别扫描 Android；开关手机接收页与本机蓝牙；确认仅蓝牙
条目不能发送、LAN 地址出现后仍需验证码确认；观察设备离开后过期、地址变化去重及
关闭窗口后扫描释放。需要真实设备广播，GN 检查不能验证硬件互通。

BLE 扫描使用 LE transport，不在 BlueZ 层限定服务 UUID；界面仍只接受通过协议解析的
Quick Share 广播。扫描启动成功回调之前到达的设备通知会有界缓存并重放，不枚举历史设备。
`BLE discovery summary` 每 15 秒输出通知、FEF3、FC73/DCT、FE2C、解析成功和忽略计数。
这些是设备变更通知计数，不是独立设备数或空中广播包数。通知为零表示应用没有收到可处理的
BlueZ 通知；FEF3 有值但 recognized 为零需要看解析日志；DCT 有值时需结合
dct_recognized 和格式诊断判断是否匹配 Quick Share。计数不能证明某条广播来自指定手机。

## BLE 辅助唤醒

选择待发送文件后，通过 Chromium BluetoothAdapter / BlueZ 广播 Fast Initiation，最长持续两分钟；可点击“寻找附近手机”重新开始或停止。窗口最小化、文件选择对话框打开、开始连接或退出时停止广播。恢复窗口时，仅在原两分钟期限内恢复。不会自动开启系统蓝牙。

广播格式复用 Chromium FastInitiationAdvertiser：服务 UUID `FE2C`，服务数据 `FC 12 8E 00 42`（V1、notify、无 UWB，与 Chromium 发送流程一致）。广播不包含设备名或文件信息。开始寻找时重置 mDNS 查询退避，让已被唤醒的手机尽快被发现。

电脑需要运行 BlueZ、开启蓝牙并支持 LE 广播；手机需要开启蓝牙及 Quick Share，并允许附近任何人分享。保持手机解锁且连接同一局域网进行验收。是否响应 notify 广播取决于手机的系统版本、可见性和后台策略；广播注册成功不代表手机已经被唤醒或已经发布 mDNS；手机可能先显示附近分享提示，需要用户操作。蓝牙不可用时界面提示手动打开手机 Quick Share 接收页面，原有 LAN 收发继续可用。

这里实现的是发送端的唤醒广播，不是 BLE 扫描、蓝牙文件传输或 Wi-Fi Direct。发现和文件传输仍依赖 mDNS 与 TCP，不能绕过网络组播限制。尚未完成本次 BLE 功能的编译及实机验证。

## mDNS 缓存续查

PTR、SRV、TXT 和每个 IPv4 地址分别保存接收时间、原始 TTL、过期时间及续查进度，不再将对方的 TTL 截断到 120 秒。按 RFC 6762 第 5.2 节在记录生命周期的 80–82%、85–87%、90–92%、95–97% 续查；收到新的有效记录后重新计时。续查按入站网卡发送，相同网卡／名称／类型的查询在一秒内合并，线程延迟不会补发一串已错过的查询。

缺失记录单独补查，间隔从一秒指数退避到最多 60 秒，不受全局浏览退避阻塞。一项记录过期时保留其他有效记录，只在服务的 PTR、SRV、TXT 均失效后清除该服务缓存。设备展示仍要求全部必要记录有效；没有添加界面保留过期设备的宽限。

TTL=0 的 goodbye 仅缩短匹配记录的有效期，保留至多一秒宽限，不创建新记录、不延长既有有效期，也不替换新 SRV/TXT 内容。地址 cache-flush 同样保留一秒宽限，新公告可恢复有效期。SRV/TXT 仍是单值缓存，本批不宣称实现完整的多记录 RRSet 处理。

公共发现实现在服务可用／不可用状态变化时输出 `crQuickShare: mDNS service available` 或 `unavailable`。`cache_id` 是本进程缓存条目的临时编号，配合 `interface` 关联日志，不是稳定设备身份。不可用日志报告第一个阻断展示的依赖，不代表其他依赖均正常：

- `record=PTR/SRV/TXT/A; reason=ttl_expired/goodbye/cache_flush`：相应记录自然到期，或撤回／替换宽限到期；正 TTL 更新会清除旧撤回原因。
- `record=TXT; reason=name_not_public`：最新有效 TXT 不再公开名称；`invalid_endpoint_info` 或 `unsupported_info_version` 表示名称材料无效或版本不支持。
- `record=A; reason=srv_target_unresolved`：SRV 改指向新主机，但新主机尚无地址记录；如果地址记录已过期，则报告其过期原因。`srv_target_changed` 标记目标变化。
- `record=NETWORK`：本机网络变化、恢复、通知或 socket 重建导致清空缓存，分别记录 `network_changed`、`network_resume`、`network_notification`、`network_reopened`。

普通失效日志附 PTR/SRV/TXT/A 有效状态、名称材料长度和阻断记录原始 `record_ttl`，不输出设备名、账户信息、DNS 名称或 IP 地址。每个服务／接口独立记录，多网卡上某条服务失效不一定导致整个设备消失；最终展示结合 `Discovery list updated` 判断。可见性日志合计每 30 秒最多 64 条，汇总中的 `visibility_logs_suppressed` 是本进程累计抑制条数。不会为从未可用的服务反复输出不可用事件，缺失记录仍由周期性 `mDNS service diagnostics` 说明。

本批只完成源码及静态检查，未编译或实机验证。建议让手机保持接收页面至少五分钟，观察原先约 30 秒的列表闪断是否消失；关闭手机接收后确认设备正常离线，再打开确认恢复。若仍闪断，保留对应 `available`／`unavailable` 日志以区分真实撤回和续查失败。

## 发现与连接诊断

所有诊断使用 `crQuickShare:` 前缀。BLE 日志记录平台支持、适配器有无／电源状态、注册、注销、系统撤销及重试；`Discovery assistance` 说明选择文件、窗口隐藏、文件选择器、到期／手动停止或连接开始造成的唤醒状态变化。`registered` 只证明本机注册成功，不是手机响应确认。

mDNS 原生后端记录 socket、packet-info、5353 端口绑定和组播加入错误。公共实现每 30 秒输出 `mDNS summary`：查询数、接收包数、非法包数、完成探测的接口数、公告数、被抑制的已知答案数、缓存 RRset 数、可见设备数及收发错误数。`Discovery list updated` 记录最终设备列表变化。`mDNS probing complete` 表示对应接口已完成三次探测；`mDNS name conflict`／`mDNS established name conflict`／`mDNS simultaneous probe deferred` 分别报告改名、重新探测和竞争退让。

连接日志使用进程内递增的 `session`，`Selected discovery record` 将它关联到所选缓存条目的 `cache_id`／`interface`。该接口仅是发现来源，不代表 TCP 已绑定该网卡；备用地址用 `address_index` 区分，不输出 IP、设备名或稳定身份。`TCP attempt` 报告每次地址尝试的开始、成功、失败、取消或超时，附 Linux errno、端口和耗时。随后 `Connection stage` 跟踪请求、UKEY2、连接响应、加密通道、匿名配对、双方确认、文件传输、交付确认和断开收尾。`Connection failure` 指出首次失败的阶段，`Peer TCP closed` 标记 EOF。

`Connection outcome` 在收尾前记录传输结果，收尾日志不覆盖这个结果。`success=1; delivery_confirmed=0` 表示发送完成但未取得交付确认；不能解读为对端已确认保存。日志不输出文件内容、文件路径、验证码、证书内容、账户信息或密钥。BLE／网络状态变化日志按事件输出，mDNS 汇总固定限频，不逐数据块记录传输。

`mDNS service diagnostics` 每 30 秒最多输出 16 个仍有有效 PTR 的远端服务，包含接口、缓存 ID、SRV/TXT 有效状态、IPv4 可用性和 endpoint-info 长度。`reason` 区分 `missing_srv`、`missing_txt`、`zero_port`、`name_unavailable`、`missing_ipv4` 和 `ready`；DNS 缓存不是设备身份认证。

排查时从启动应用开始收集日志，复现问题后保留至少一条 mDNS 汇总；若已连接，按同一 session 串联 TCP attempt、Connection stage、Connection failure/outcome。应用不能仅凭本机日志确定手机是否实际收到 BLE 广播，也不能从一次 TCP 超时确定是防火墙还是对端未响应。

## 第一批协议补齐

- 记录对端 `safe_to_disconnect_version`，按版本启用文件 ACK 和安全断开；关闭时发送 request／ACK，处理双方同时关闭，并为收尾 IO 设置 2 秒上限。该版本号不代表实现了 Nearby 的所有可选重连功能。
- 逐个跟踪发送文件的长度、实际写完的偏移、最后一块和 ACK。提前 ACK、未知 ID 和无效偏移会失败，重复合法 ACK 幂等处理。支持 ACK 的对端若提前断开，不再算作成功；旧端只显示未确认送达。
- 等待本地确认时仍处理远端拒绝、取消和心跳；确认／传输超时独立显示。错误、拒绝和取消尽力发送对应控制消息，再有界关闭，不会清除用户的取消状态。
- 加密发送队列在背压期间同时读取控制消息；接收确认或文件 ACK 尚未写完时，允许有限缓存后续文件帧。未确认前仍不处理文件内容。
- 心跳回复回显 `seq_num`，协商参数设置合理上下限；解析按共享层状态验证必要消息，对证书／进度等可选消息进行有界忽略。
- 处理连接层 `BANDWIDTH_UPGRADE_RETRY`：请求型消息回复仅支持 `WIFI_LAN`，回复型消息不再回包；保持当前加密 TCP 连接，不改变本地确认状态。每分钟最多处理 32 条，避免无界重试。
- 同目录内重名文件自动追加编号；不同目录内的同名文件保留。`parent_folder` 通过相对路径校验后，用于在独立接收批次中还原目录树。
- mDNS 支持 QU 单播回复及传统 DNS 查询回复，按入站网卡发送；缓存按接口隔离，保留多个 IPv4 地址并在连接失败时有界重试。A 记录处理 cache-flush 的一秒宽限，PTR 生命周期独立，goodbye 延迟一秒过期。公告改为可见时发送两次，浏览查询退避至最多 60 秒。
- 新增超时提示的英文及 13 种语言翻译。

发布前 probing、重名冲突仲裁、known-answer suppression 和 SRV/TXT 多记录缓存现已由公共实现处理，详见下文。网络变化会重建组播成员关系并重新探测；重叠子网的 TCP 出站接口绑定仍未实现。当前发现层只覆盖 Quick Share 所需的 IPv4 DNS-SD，不是完整的通用 mDNS 实现。DNS 行为参考 [RFC 6762](https://www.rfc-editor.org/rfc/rfc6762.html)。

本批未实现断点续传和自动重连；收到要求恢复已有附件或流附件的分享响应会明确失败。安全断开超时不会把未取得文件 ACK 的发送变成成功。接收方以本地文件提交结果为准，最后 ACK 写失败也不会删除已提交文件。

## 构建接入

在 Chromium `src` 根目录操作，使用独立输出目录，避免更改其他项目构建参数。
配置及编译命令仅供后续使用，本次开发没有执行编译。

```sh
gn gen out/crquickshare --args='is_debug=false is_component_build=false symbol_level=0 chrome_pgo_phase=0 enable_libcr_shared=false root_extra_deps=["//crQuickShare"]'
autoninja -C out/crquickshare crQuickShare
```

也可使用随项目提供的 Makefile 片段：

```sh
make -f crQuickShare/chromium_src_Makefile cqs-configure
make -f crQuickShare/chromium_src_Makefile cqs
```

默认保留静态构建。共享模式使用业务 EXE + `libcr.so`，配置命令如下：

```sh
make -f crQuickShare/chromium_src_Makefile CQS_SHARED=true cqs-configure
make -f crQuickShare/chromium_src_Makefile CQS_SHARED=true cqs
```

共享模式默认输出到 `out/crquickshare-shared`，静态模式默认输出到 `out/crquickshare`，可用 `CQS_OUT` 指定目录。配置会写入该目录的 GN 参数，不要用它覆盖其他应用已经调好的构建目录。

如需与 crPlayer 共用同一份引擎，应在同一个输出目录、同一套源码和参数下配置 `enable_libcr_shared=true`，并将 `//crPlayer`、`//crQuickShare` 一起加入 `root_extra_deps`，再使用 autoninja 构建各自目标。当前 C++ ABI 不支持混用不同版本或不同参数生成的引擎。

共享模式下，窗口、mDNS、传输状态机及文件处理保留在业务 EXE；Nearby Connections / Sharing protobuf、UKEY2、SecureMessage 及其基础依赖由引擎提供。协议相关目标和 protobuf lite 使用限定到目标的符号导出配置，没有将整个引擎改为默认导出。引擎不依赖 crQuickShare 的业务目标、语言资源或源码。

部署时将匹配的 `libcr.so` 与 EXE 放在同一目录；EXE 使用 `$ORIGIN` 查找引擎。仍需携带 ICU、资源及实际运行时依赖。项目尚未提供共享模式打包目标。共享模式沿用引擎的 CFI 限制，静态模式保持原配置。
GN 目标是 `//crQuickShare:crQuickShare`。部署需保留运行时依赖中的 ICU、资源 PAK 和其他平台文件，不可只复制可执行文件。

## 使用

```sh
out/crquickshare/crQuickShare --lang=zh-CN
out/crquickshare/crQuickShare --device-name=Office --receive-dir=/home/user/Downloads/Shared
```

`--receive-dir` 需要绝对路径；未提供或不是绝对路径时使用 `~/Downloads/crQuickShare`。
`--device-name` 接受 UTF-8 名称；默认 `crQuickShare`，协议中的设备名截取至 80 字节。
`--lang` 支持 en-US，以及 ja、ko、zh-CN、zh-HK、zh-TW、ar、fr、fr-CA、ru、es、es-419、pt-BR、pt-PT；默认读取系统语言。

发送：在 Chromebook 打开 Quick Share 接收界面并设为所有人可见；在 crQuickShare 选择文件，点击发现的设备，核对双方验证码并确认。接收方还需要接受文件。

接收：点击“接收 10 分钟”；在 Chromebook 中选择文件并通过 Quick Share 发送；在 crQuickShare 核对验证码、来源名称和完整文件列表，确认后接收。

设备名称来自未验证的网络广播，只用于展示。验证码和用户确认是无账号模式下的核验步骤，不会因为同名设备自动接受。

## 代码分层

- `crquickshare_main.cc`：独立 Views / Viz / Ozone 生命周期及语言资源加载。
- `crquickshare_window.cc`：主窗口、文件选择、设备列表、确认和进度。
- `ble_wakeup.cc`：Fast Initiation 广播、蓝牙状态观察及异步注销。
- `lan_discovery.h` / `lan_discovery.cc`：发现接口与端点信息编码。
- `mdns_codec.h/.cc`：跨平台 DNS 编解码、压缩名称展开、记录比较和探测排序。
- `lan_discovery_mdns.cc`：共享发现状态机、缓存、公告、探测及应答抑制。
- `mdns_network.h`：原生数据报边界；`lan_discovery_linux.cc` / `lan_discovery_desktop.cc` 仅负责网卡枚举、组播 socket、接口信息和收发。
- `quick_share_service.cc`：单会话后台线程、TCP framing、上游 UKEY2、匿名身份交换、收发状态机和文件提交。
- `transport_channel.h/.cc`：有序可靠字节流接口及非阻塞 TCP 实现，封装部分读写、就绪等待、EOF、系统错误与关闭；TCP 建连工厂保留有界等待和取消检查。
- `quick_share_service.h`：UI 与服务之间的快照接口。
- `crquickshare_strings.grd` / `translations/`：英文源字符串和 13 种语言资源。

上游参考：

- `third_party/nearby/src/connections/implementation/wifi_lan_service_info.cc`
- `third_party/nearby/src/sharing/advertisement.cc`
- `third_party/nearby/src/connections/implementation/proto/offline_wire_formats.proto`
- `third_party/nearby/src/sharing/proto/wire_format.proto`
- `chrome/browser/nearby_sharing/paired_key_verification_runner.cc`
- `chrome/browser/nearby_sharing/nearby_sharing_service_impl.cc`
- `chrome/browser/nearby_sharing/fast_initiation/fast_initiation_advertiser.cc`
- `third_party/ukey2/`

## 验证

### 网络恢复与错误诊断

`Network discovery reset` 记录接口变化、网络通知、休眠恢复与重建结果。验证时应覆盖
启动时断网、切换 Wi-Fi、拔插网线、休眠恢复，以及失败后再次分享，确认无需重启应用
即可恢复设备发现和接收广播。重建会短暂清空旧设备列表，防止选择失效地址。

协议快照使用明确的 Failure 类别；crFiles 分别显示连接建立失败、连接中断、存储不可用、
源文件读取失败、不支持请求及协议交互失败。拒绝与超时继续使用各自提示。会话失败后
保留收尾前的阶段及原因，发现网络不可用不会覆盖已有传输错误。Quick Share 弹窗与结果
提示提供“Copy diagnostics”，仅在用户点击时复制会话编号、方向、阶段和固定诊断原因，
不包含文件内容、路径、验证码或密钥。尚未收到文件清单的接收失败也显示结果提示。

### 传输通道抽象

协议会话通过 `TransportChannel` 读写，不直接操作连接 socket。会话保留帧缓冲、
加密序号、写队列、心跳、超时及取消逻辑；TCP 实现负责非阻塞系统调用与错误映射。
挂断时仍先读取缓冲数据，再报告 EOF。通道由会话线程独占，收尾后先关闭通道再恢复
接收广播；监听 socket 仍由 LAN 发现线程管理。出站连接保持总共 10 秒、单地址最多
3 秒的预算，等待时每 100 毫秒以内检查取消。接口目前只有 WIFI_LAN 实现，不进行
蓝牙连接或介质切换；未来升级必须额外处理通道交接、在途数据及加密状态。

实机回归需覆盖双向传输、连接取消、地址回退、传输中断线、多文件及连续两次发送。

### 消息分发与文件提交

Sharing 消息按类型进入有界队列，等待指定类型时保留提前到达的其他消息。相同消息的有限重复可以忽略，内容改变的重复清单或配对结果会被拒绝。接收会话中的对端 RESPONSE 不代表本地同意；只有本地手动确认才允许接收文件。文件与元数据分别跟踪 payload ID、写出偏移和生命周期，已完成元数据的迟到清理通知不会取消文件传输。

接收分块检查大小、偏移、结束标志以及存在时的连续 index；同一 payload 不允许中途改变是否携带 index。完全一致的空结束块可以有限重复，不重复写盘或增加进度。文件先写入临时目录并刷盘，整批文件通过不覆盖目标的目录重命名提交后，才发送最后一个文件的 ACK。提交前失败清理临时文件；提交后连接断开保留已保存文件。单文件 ACK 不表示整批目录已提交，旧端关闭连接且未返回 ACK 时也不声称交付已确认。

所有传入 payload 在路由或排队前统一检查结构：DATA 只能携带数据块，CONTROL 只能携带控制消息，ACK 不携带两者。文件 DATA 的大小、提供的文件名必须与已接受清单一致，文件名比较使用对端原名而非本地重名处理后的名称。目录必须与介绍清单一致；敏感标志和修改时间在首次提供后不得改变。省略可选字段不会清除已记录的值。目录经过分段校验，通过相对目录描述符建目录；敏感标志和修改时间不改变本地权限。普通文件块、元数据块和重复结束块共用分块校验入口，提前结束的块在写盘前拒绝；排队不会重复推进偏移或索引。ACK 仍允许省略类型及携带 total_size=-1。

实机复查应覆盖 Pixel 与 LineageOS 双向发送、连续发送两次、零字节及多文件、确认期间取消、传输期间取消、断网后重试；检查文件内容、临时目录清理和接收服务重新可发现。新状态机行为仍需要这些实机结果验证。

### 匿名会话头像材料诊断

当前只诊断是否收到证书材料，不解密、不下载或显示头像，也不新增账户登录或信任授权。终端会输出以下 `crQuickShare:` 日志：

- `Anonymous pairing diagnostics`：本地仍发送 `UNABLE`，对端结果数值为 UNKNOWN=0、SUCCESS=1、FAIL=2、UNABLE=3。
- `Certificate diagnostics`：每条允许处理的 `CERTIFICATE_INFO` 消息记录消息体是否存在、证书数量，以及具有各必要字段的证书数量。字节字段只有非空才计数；有效期字段只检查是否存在，不判断时间有效性。`with_all_fields` 不表示证书有效、可信或可解密。
- `Certificate diagnostics summary`：会话销毁时输出汇总，包括未收到消息时的 `certificate_frames=0`；`outgoing=1` 表示本机发送，`0` 表示接收。`pairing_result_received=0` 表示尚未收到配对结果，不能据此判断完整匿名会话的证书行为。

上游已将 `CERTIFICATE_INFO` 标为废弃消息，新版对端可能不发送。计数为零只说明当前会话没有观察到该消息，不代表对端没有账户头像，也不证明头像必须登录才能取得。计数非零仍需研究证书与发现广播材料是否匹配、是否可解密。当前诊断不保存或输出证书字节、密钥、标识哈希、账户资料、验证码或头像 URL；仍沿用原有消息大小和扩展消息数量限制。

实机验证时分别用 Pixel 和 LineageOS 与 crQuickShare 完成两个方向的普通文件分享，保留上述诊断行并标明设备和发送方向。不要为了触发证书交换伪造认证成功；现有验证码和手动确认流程保持不变。

资源检查不编译应用，也不是单元测试：

```sh
python3 crQuickShare/tools/validate_resources.py
```

已检查英文及 13 种语言的消息数量、GRIT 指纹和占位符一致性；已用独立临时输出目录完成 GN generation / `--check=//crQuickShare:*`。
已在独立检查目录验证 shared/static 两套 GN 生成和 crQuickShare 头文件依赖；shared 模式同时检查引擎与 crPlayer 的头文件依赖。生成的 shared 链接规则中，EXE 包含 5 个业务对象和 libcr.so；Bluetooth 底层、协议 protobuf 与加密库由引擎提供。Bluetooth 导出宏同时支持 LIBCR_SHARED_BUILD。

未新增单元测试；未运行 C++ 编译、链接、窗口截图验证或实际网络传输。GN 检查不能替代真实链接和运行验证。

后续实机验收应记录 ChromeOS 版本和两个方向的发现／发送／接收结果，并覆盖：双方同时关闭、确认超时、提前／重复／未知 ID ACK、发送背压期间取消、QU 查询、多地址失效回退、零字节文件、多文件、大文件、同名文件、拒绝、双方取消、错误验证码、空间不足、断网及重新发现。核对接收文件与原文件的 SHA-256；确认没有在接受前写入文件，已有文件不被覆盖。

逐文件进度取自实际 payload：发送端在完整记录写入通道后累计字节，接收端在写盘后累计字节、刷盘后累计完成文件。界面展示当前文件及其介绍顺序、文件字节数、总字节数和完成数量，空文件也计数。速度与剩余时间根据传输阶段最近约三秒的采样估算，不包含配对或人工确认等待；等待交付确认时不继续显示估算。文件 payload 完成不等于远端已确认交付，接收成功仍以整批提交为准。

目录传输使用原生 Quick Share `FileMetadata.parent_folder` 和 `PayloadHeader.parent_folder`，不使用 ZIP。crFiles 允许从右键 Quick Share 发送目录及混合选择，独立窗口新增选择文件夹按钮。发送递归保留根目录，接收进度和确认清单展示相对路径。同名选中根项目追加编号。仅发送普通文件，不跟随符号链接；发现符号链接、特殊文件或读取失败时整批失败。

限制：每批最多 100 个实际文件、总计 1 TiB，父目录最多 32 层、1024 UTF-8 字节，遍历最多 10000 个条目。空目录没有独立协议消息，因此跳过；只有空目录时返回不支持，不伪造文件。接收端拒绝绝对路径、空路径段、点路径段、反斜杠、盘符、文件与目录冲突，所有建目录和建文件操作相对 staging 描述符并禁用符号链接跟随；文件和子目录刷盘后才提交整批。Android 对目录字段的实际处理仍需验证，不能以传输成功推断对方保留了层级。

平台接口已与 Linux 后端分离：

- `LanDiscovery` / `CreateLanDiscovery`：发现、公告、缓存刷新及本地网段判定。地址使用数字 IP 字符串，接口不暴露 `sockaddr` 或字节序。
- `ConnectionListener` / `CreateConnectionListener`：监听端口及有界等待接收连接，返回 `TransportChannel`，不暴露 socket 描述符。协议服务仍负责接收开关、并发会话与来源策略。
- `FileStorage` / `CreateFileStorage`：递归收集并保留已打开的发送文件；创建 `ReceiveBatch` 管理接收文件写入、刷盘、整批不覆盖提交和失败清理。接收批次关闭句柄后才清理暂存目录；提交成功但父目录持久性未确认时单独报告，不撤销成功结果。文件读写采用跨平台 `base::File`，原生目录操作只在后端。

Linux 后端分别位于 `lan_discovery_linux.cc`、`transport_channel_linux.cc`、`file_storage_linux.cc`。GN 按平台选择后端，Windows/macOS 已加入 `platform_socket.cc`、`lan_discovery_desktop.cc`、`transport_channel_desktop.cc` 和各自的文件保存后端，并接入 crFiles 的发送窗口、后台接收及设置页面。独立 crQuickShare 窗口与 BLE 仍只在 Linux 启用。协议服务不直接调用原生网络和目录 API。

Windows 使用 Winsock、GetAdaptersAddresses 和句柄重命名提交；当前仅支持本地磁盘路径，拒绝 UNC、重解析点、保留名称及大小写冲突。文件刷盘后提交，但不声称父目录已持久化。macOS 使用 BSD sockets、getifaddrs、openat 与 renameatx_np(RENAME_EXCL)，拒绝文件系统大小写／Unicode 别名造成的目录合并。macOS Bonjour 服务清单已加入 Quick Share 服务类型；沿用 crFiles 的局域网权限与文件夹授权。

这些后端尚未经过 Windows/macOS 编译或实机互传验证。需验证系统局域网权限、防火墙、多网卡、睡眠恢复、双向传输、拒绝／取消、目录冲突与失败清理。


## 共享 mDNS 发现与协议行为

Linux、Windows 和 macOS 使用同一份编解码、缓存和发现状态机。公共代码不包含 socket／网卡原生类型；网络边界中的 IPv4 地址统一使用主机序整数。Linux 保留 netlink 通知，Windows/macOS 保留网卡轮询，三者共享睡眠恢复与接口变化后的缓存重建。

- 启用接收后，每个接口先随机等待 0–250 ms，再以至少 250 ms 间隔发送三次 ANY／QU 探测，Authority 携带完整 SRV、TXT、A 候选记录。第三次后再等待至少 250 ms 才公告；发送失败不会计为成功探测。公告成功后至少再发送一次，并维持周期公告。
- 同时探测按 class、type、展开后的 RDATA 作无符号字节排序；失败方等待一秒再试。已发布记录冲突先撤回并重新探测；探测期间确认重名后更换 DNS 主机名，或重新生成合法的四字符端点 ID。实例名始终保留 Nearby 的 base64 格式，服务线程同步握手使用的 ID。连续冲突带退避，不修改用户设置的显示名称。
- 应答只针对匹配的问题，附带相关 SRV/TXT/A；查询的 Answer 区仅用作 known-answer suppression，不进入缓存。TTL 至少达到本机记录的一半才抑制；TC 查询等待 400–500 ms 接收同一来源、端口及接口的后续已知答案，等待有上限。不同查询者分别保存待答记录；已发送的组播答案可同时满足其他等待的 QM 查询。传统单播查询保留 transaction ID 和问题区、TTL 至多 10 秒且不设置 cache-flush。
- 发出的查询携带仍有至少一半 TTL 的已知答案；超过单包边界时使用 TC 和无问题区的后续包。刷新和查询退避仍有去重与容量限制。
- 缓存按接口、规范化名称和记录类型保存 RRset。PTR 最多 128 条，其他 RRset 最多 16 条，总计最多 512 组；SRV/TXT 不再覆盖单个槽位。整包校验通过才更新，SRV/PTR 比较先展开压缩名称。goodbye 只撤销相同 RDATA；cache-flush 处理整包集合，保留包内兄弟记录和最近一秒收到的记录，其他旧记录延迟一秒删除。SRV/TXT 当前值优先选未撤销且最近收到的记录，同包多个值按规范化 RDATA 稳定选择。
- 数据包、名称跳转、记录数、缓存、待答队列和单次收包都有上限。名称编解码保留大小写和任意标签字节，DNS 比较使用规范化名称；不会因为同包其他服务的 UTF-8 名称就丢弃有效的 Quick Share 记录。

本次仅进行了 Linux 静态／共享配置的 GN 生成与依赖检查，没有编译、单测或实机验证。实机应重点检查首次发现的探测延迟、重复发送后重新公告、多网卡回流、冲突后 ID 与握手一致、带压缩名称的记录更新，以及旧 SRV/TXT 的 goodbye 不撤销新值。IPv6、完整通用 mDNS 功能和 TCP 出站接口绑定不在本次范围内。
=======
