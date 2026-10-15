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
- 双向多文件传输。每次最多 1024 个普通文件，总大小上限 1 TiB；不发送目录和符号链接。
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

## crFiles Linux 蓝牙接收（第一阶段）

crFiles 开启 Quick Share 接收后，`BluetoothReceiver` 在 UI 序列注册 NearbySharing
RFCOMM 服务和 FEF3 本地 GATT 服务；二者就绪后才广播接收入口。不依赖发送弹窗是否
打开，不要求手机和电脑位于同一局域网。现有接收开关仍要求服务初始化成功；未接入
独立 crQuickShare 窗口。需要本机蓝牙适配器及 BlueZ 支持外围广播和本地 GATT 服务。

广播使用 20 字节 Base64URL V2 索引（一个槽位）；服务数据、UUID 和 flags 合计
31 字节，不依赖扩展广播。手机从 FEF3 的 `00000000-0000-3000-8000-000000000000`
只读特征获取完整普通 Nearby 广告，其中含与 LAN 一致的公开名称、端点 ID、Sharing
salt/key hash 和本机经典蓝牙地址。服务 UUID 为
`a82efa21-ae5c-3dde-9bbc-f16da7b16c5a`，不要求系统蓝牙配对；通信仍进行 UKEY2
加密、匿名配对及本机接收确认，不支持自动接受。

接受的 RFCOMM socket 复用 `BootstrapProbe` 的有界、事件驱动传输桥，通过
`ReceiveBluetooth` 与 LAN 和出站请求互斥。完成或取消后恢复接收广播；忙碌时撤回广播
并拒绝新连接，已有传输不因广播撤回而断开。关闭功能、退出、蓝牙断电会清理相应
资源；异步创建成功但请求已经取消的 socket／广播会立即释放。

蓝牙直传上限为 **72 KiB（73728 字节）**。crFiles 接收端在用户确认后，优先请求
WIFI_LAN 升级（路径等待最多 5 秒）；已有对端主动提供的可用路径则直接尝试。
LAN 路径使用协议中的四字节 IPv4 地址和 TCP 端口，仅连接本机活动网卡同子网的
单播地址，并绑定对应源地址和接口。不依赖 mDNS 已经发现手机，不修改网络配置。
TCP 建连共用 3 秒预算，LAN 建连和 introduction/排空阶段共用 10 秒预算。
LAN 始终保留已有 UKEY2 加密上下文，不接受关闭 LAN 会话加密。

LAN 路径被拒绝、没有路径或在蓝牙排空屏障之前连接失败时，如有热点连接器，
再请求 WIFI_HOTSPOT（路径等待最多 15 秒）。使用
`--quick-share-upgrade-interface=wlp3s0` 选择加入热点的网卡，热点连接仍共用 40 秒预算。
已开始排空蓝牙或已经替换通道后的失败终止会话，不能安全回退。
热点仍默认自动选择频段/信道，并在结束后恢复网络和删除临时配置。

两种路径复用 TCP client introduction、蓝牙排空屏障及心跳验证；验证成功后才发送
分享层 ACCEPT。小批次未收到升级路径时仍走蓝牙，开始直传后拒绝迟到路径。
没有热点连接器时仍可尝试 LAN；所有可用升级路径失败时明确终止，不用蓝牙传大文件。
文件继续使用原有路径、长度、分块、空间检查及临时批次提交机制。

此接收实现支持连接 Android 提供的 LAN TCP 端点或加入其热点，不创建电脑热点，
不实现 Wi-Fi Direct，也不在电脑端发起 LAN 监听路径提议。LAN 首版不支持 IPv6、
跨子网路由。手机是否提供 LAN 路径取决于其实现和网络环境。出站升级策略保持热点。

诊断顺序：`Bluetooth receiver listener ready` → `Bluetooth receiver GATT ready` →
`Bluetooth receiver advertising` → `Bluetooth receiver advertisement read` →
`Bluetooth incoming session admitted` → `bluetooth_accepted` → 握手、确认、文件传输。
初始化失败按 15 秒间隔重试，并记录失败阶段，不输出文件正文或密钥。

切换诊断：`Wi-Fi upgrade TCP ready` 标记新 TCP 建连完成；`Wi-Fi upgrade introduction IO`
记录 introduction 写入、首次 TCP 读取和读取完成/错误（仅字节数，不输出内容）。
`Wi-Fi upgrade verification first receive` 是切换后的首次验证读取，与前面的 introduction
回包分开统计。验证摘要含 `wait_calls`、`readable_events`、`read_would_block`，用于
区分从未出现可读通知与可读后读取返回 would-block；这些应用层统计不能代替抓包。
蓝牙关闭回调记录 `close_elapsed_ms`、关闭时 `read_pending` 及关闭后的读取回调。
桥接日志 `session` 在入站连接中关联接收会话，出站/独立探测为 0（未关联）。
`Sharing response queued` 标记 ACCEPT 等响应入队，不代表对端已收到。
以上日志不改变心跳验证、ACCEPT 顺序或超时，不输出热点凭据及文件正文。


已有实机日志验证蓝牙发现、加密握手、文本和热点文件接收；LAN 升级本次未编译，
未新增单测，尚待实机验证。验收时让手机和电脑处于同一子网，发送超过 72 KiB 的
文件，检查 `medium=WIFI_LAN`、`lan_channel_ready`、`encryption_retained=1` 和
`delivery_confirmed=1`，并比较文件内容。再用不同网络验证热点回退，检查取消、拒绝、
连续接收，以及旧蓝牙通道排空后的失败不会再次尝试升级。

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

### crFiles 接收文本和链接

crFiles 显式启用 Quick Share 的 TEXT／URL／ADDRESS／PHONE_NUMBER 附件接收；未指定类型或
类型为 UNKNOWN 的文本附件也按纯文本接收，仍校验大小、负载 ID、分块及正文编码。
地址与电话号码只复制正文，不调用地图或拨号程序。确认界面显示附件数量、大小及
“接受后复制到剪贴板”提示；仍需本机确认接收。正文按 introduction 中的 payload ID
从 BYTES 载荷分流，不作为 Sharing 控制 protobuf 解析，也不使用 text_title 代替正文。
大小、偏移、分块索引及结束标志沿用公共验证入口，接收完成后检查 UTF-8 并拒绝 NUL。

每批最多 16 个非空文本类附件，正文合计最多 1 MiB；多附件按 introduction 顺序
用换行合并为一次剪贴板写入。不创建接收目录，不自动打开 URL，不写入 HTML 或执行内容。
文件和文本混合批次，以及 Wi-Fi 凭据、应用和流附件仍明确返回不支持。
取消、拒绝、超时、非法或未完整收到的批次不会写入剪贴板。

核心完成队列最多保存 8 批已确认、完整接收的正文，由 crFiles UI 线程消费一次并写入
系统普通剪贴板；刷新状态或重新打开窗口不会再次覆盖剪贴板。完成提示为
“Copied to clipboard”。最终协议 ACK 表示正文已完整交给应用队列，并不保证操作系统
剪贴板已经持久保存。独立 crQuickShare 默认不启用文本接收，避免没有剪贴板消费者时
错误报告接收成功。尚未编译或进行 Android 实机验证；应验证中文、长文本、URL、拒绝、
取消，以及后续正常文件收发。

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

Linux 热点连接输出 `Hotspot join stage`，每个阶段有 `event=started` 和
`event=finished`，结束记录包含 `success`、本阶段 `stage_elapsed_ms`、从连接器入口
累计的 `elapsed_ms` 和 `deadline_expired`。开始记录的 `remaining_ms` 表示原有总截止
时间的剩余预算；计时使用单调时钟，所有 NM 方法调用与激活等待共用总截止时间。
阶段依次为 `validate_credentials`、`check_wifi_device`、`read_previous_connection`、
`create_profile`、`activate_connection`、`check_local_address`、
`connect_tcp`。其中 `create_profile` 调用 NM D-Bus `AddAndActivateConnection2`；
`activate_connection` 等待 ActiveConnection 进入 ACTIVATED 状态，
包含 Wi-Fi 关联、认证和 IP 配置，不能单凭此项区分扫描与 DHCP；
`check_local_address` 是激活完成后的本机同子网地址检查，不是 DHCP 等待时间。
`connect_tcp` 单独统计绑定该接口和地址的 TCP 建连耗时。提前返回时当前阶段记录
`success=0`（也可能是取消），最终 `Hotspot join timing` 汇总连接器结果与总耗时，
不包含后续协议切换、文件传输和网络恢复。日志不输出 SSID、密码或 D-Bus 错误正文。

热点凭据的 `frequency` 透传到 Linux 后端，仅用于诊断。热点连接默认自动选择
频段和信道：不向 NM 配置写入 `band` 或 `channel`，而非写入字面值 `auto`。
`Hotspot frequency hint` 保留协议原值，记录 `applied=0; band=auto; channel=0`。
不再先固定信道等待 12 秒后递归回退；激活、地址检查和 TCP 建连共用原始
40 秒截止时间，并保留取消检查及失败后的网络清理。该策略共用于发送和接收升级。
实际耗时及成功率仍需多轮实机对比。
`Hotspot NM state` 记录设备状态、原因和活动连接状态；`Hotspot NM scan` 记录
LastScan 变化及相对耗时（初始样本不是本次扫描完成事件）。诊断最多每 500 ms 采样，
单次诊断预算 100 ms；短暂状态可能漏采。NM 的 `wifi_config` 包含扫描、关联和认证，
这些子阶段仍需结合 supplicant 系统日志分析。不额外请求扫描。

热点后端直接使用系统 D-Bus，不启动 nmcli、不创建密码文件。连接配置使用
`persist=volatile`、`bind-activation=dbus-client`，由每次升级独立的私有 D-Bus 连接
持有生命周期；密码只通过 D-Bus 传给 NM。传输线程以最多 50 ms 的 D-Bus 等待片段
处理回复，并轮询激活状态，期间继续处理蓝牙心跳和取消。首次连接本机系统总线
使用 libdbus 的同步连接过程。

清理时先检查接口当前的 ActiveConnection；若已经切换到其他连接，则保留它，
否则尝试激活之前的配置，并等待激活完成。随后删除本次临时配置；NM 已自动删除
的配置也视为清理完成。取消时尚未收到的激活回复会在清理阶段短暂等待；无法确认
归属时关闭私有总线，让 NM 释放绑定的热点连接，并保留网络清理未完成提示，
不会盲目切换网络。异常退出时客户端绑定也会释放热点，但不保证恢复原 Wi-Fi。

`Network discovery reset` 记录接口变化、网络通知、休眠恢复与重建结果。验证时应覆盖
启动时断网、切换 Wi-Fi、拔插网线、休眠恢复，以及失败后再次分享，确认无需重启应用
即可恢复设备发现和接收广播。接口索引、IPv4 地址和掩码不变且 socket 正常时，网络通知
不再清空缓存或重建 socket，只触发限频的查询与公告刷新；累计次数见
`unchanged_network_notifications`。接口变化仅失效受影响接口的缓存，并保留其他接口的
缓存 ID、记录剩余 TTL 和公告状态。休眠恢复或 socket 不可用仍整体失效并重新探测。
`affected_interfaces` 与 `invalidated_all` 说明失效范围。接口信息完全相同的网络切换
目前无法凭这些字段识别，保留记录仍遵守原 TTL 和正常续查，不延长旧记录寿命。

入站 TCP 连接先进入发现线程拥有的预握手队列，不立即占用传输会话、撤回接收广播或
修改传输结果。最多暂存 8 个连接，每个从接入起限时 15 秒，首帧沿用 1 MiB 上限。
每轮对每个连接最多进行一次非阻塞读取，只读取长度头和首帧，不抢读后续 UKEY2 数据。
首帧必须是有效 V1 CONNECTION_REQUEST，并包含合法端点 ID 和名称，随后才竞争会话
名额，将原始首帧交给会话层重新验证并继续握手；这不是身份认证，也不替代接收确认。
空连接、非法长度、残缺帧、超时及容量拒绝使用 `Incoming connection not started` 的
`prehandshake_*` 原因记录，不弹出文件传输失败。通过时记录
`Incoming prehandshake accepted`。停止接收、开始其他传输和退出时清理暂存连接。
实机验收应覆盖空连接后正常接收、分段首帧、同时连接、首帧超时、接收开关及多网卡变化。

协议快照使用明确的 Failure 类别；crFiles 分别显示连接建立失败、连接中断、存储不可用、
源文件读取失败、不支持请求及协议交互失败。拒绝与超时继续使用各自提示。会话失败后
保留收尾前的阶段及原因，发现网络不可用不会覆盖已有传输错误。诊断保留在终端日志中，
crFiles 不再显示 Copy diagnostics 按钮。预握手失败不会生成传输结果；进入正式会话后
的协议错误仍按原有错误流程处理。

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

发送线程先进入 `source_preflight`，完成整批递归收集、打开文件、检查路径与数量／总大小限制，
再检查文件信息并构建实际 introduction。发送清单保留 4096 字节的封装余量，必须能放入
当前 1 MiB 单记录上限。全部通过后才进入 `tcp_connect`；失败或取消不连接对端。
发送继续使用预检查保留的句柄，不重新按路径打开；每个文件发送前后复核大小与修改时间，
完成后关闭其句柄。预检查不预读全部正文，也不创建文件快照，不能保证后续磁盘读取不失败，
或检测保留大小与修改时间的原地内容变化。此流程由 Linux、Windows、macOS 后端共用。
文件上限统一为 1024（包括目录内文件）。预检查仍保留全部源文件句柄，实际可发送数量也受
系统可用句柄、发送清单大小与对端限制影响；本机打开文件失败时在连接前报告错误。

限制：每批最多 1024 个实际文件、总计 1 TiB，父目录最多 32 层、1024 UTF-8 字节，遍历最多 10000 个条目。空目录没有独立协议消息，因此跳过；只有空目录时返回不支持，不伪造文件。接收端拒绝绝对路径、空路径段、点路径段、反斜杠、盘符、文件与目录冲突，所有建目录和建文件操作相对 staging 描述符并禁用符号链接跟随；文件和子目录刷盘后才提交整批。Android 对目录字段的实际处理仍需验证，不能以传输成功推断对方保留了层级。

平台接口已与 Linux 后端分离：

- `LanDiscovery` / `CreateLanDiscovery`：发现、公告、缓存刷新及本地网段判定。地址使用数字 IP 字符串，接口不暴露 `sockaddr` 或字节序。
- `ConnectionListener` / `CreateConnectionListener`：监听端口及有界等待接收连接，返回 `TransportChannel`，不暴露 socket 描述符。协议服务仍负责接收开关、并发会话与来源策略。
- `FileStorage` / `CreateFileStorage`：递归收集并保留已打开的发送文件；创建 `ReceiveBatch` 管理接收文件写入、刷盘、整批不覆盖提交和失败清理。接收批次关闭句柄后才清理暂存目录；提交成功但父目录持久性未确认时单独报告，不撤销成功结果。文件读写采用跨平台 `base::File`，原生目录操作只在后端。

Linux 后端分别位于 `lan_discovery_linux.cc`、`transport_channel_linux.cc`、`file_storage_linux.cc`。GN 按平台选择后端，Windows/macOS 已加入 `platform_socket.cc`、`lan_discovery_desktop.cc`、`transport_channel_desktop.cc` 和各自的文件保存后端，并接入 crFiles 的发送窗口、后台接收及设置页面。独立 crQuickShare 窗口仍只在 Linux 启用；crFiles 的 BLE 已接入 Linux 和 Windows，Windows 验证范围见文末。协议服务不直接调用原生网络和目录 API。

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

本次仅进行了 Linux 静态／共享配置的 GN 生成与依赖检查，没有编译、单测或实机验证。实机应重点检查首次发现的探测延迟、重复发送后重新公告、多网卡回流、冲突后 ID 与握手一致、带压缩名称的记录更新，以及旧 SRV/TXT 的 goodbye 不撤销新值。IPv6、完整通用 mDNS 功能不在本次范围内。
=======


### 多网卡出站连接

LAN 发现保留每个目标 IPv4 对应的本机源 IPv4 和接口编号，同一地址出现在不同接口时
保留独立路径，每个设备最多 32 条。预检查后按同一端点信息与端口刷新路径；每次连接前
复核接口、源地址及目标所属子网。Linux／Windows 使用 IP_UNICAST_IF，macOS 使用
IP_BOUND_IF，并绑定本机源 IPv4。绑定失败或路径失效后尝试下一条发现路径，不自动放宽
为系统路由；只有没有发现路径信息的兼容调用才沿用系统路由。

连接总预算仍为 10 秒，每条路径最多 3 秒，按剩余路径分配时间，等待期间可取消。
TCP attempt 日志包含 target_ipv4、local_ipv4、interface、routing 及具体失败原因。
此修改不能解决手机未发布 LAN 服务或热点不支持并发的问题。尚未编译或进行跨平台实机验证。


### BLE 与无公开名称 LAN 记录的关联

发现快照保留结构有效、名称非公开但具备有效 IPv4／端口的 LAN 候选，界面不直接显示它们。
Linux BLE 合并要求端点 ID、16 字节盐与元数据密钥哈希，以及除名称可见性位外的头部全部
一致，才使用仍在有效期内的 BLE／GATT 公开名称。BlueZ 名称、仅端点 ID 或仅设备名称
相同均不能使匿名 LAN 候选成为可发送设备；仍隐藏 Bluetooth only 头像。
点击发送时重新合并有效发现记录，核心再次验证公开名称材料与当前 LAN 记录一致，连接
地址及路径只能取自当前 LAN 快照。关联只提供显示与连接线索，不是身份认证或自动信任。

每 30 秒的关联诊断增加 LAN association record 与 BLE LAN association，显示 cache_id、
IP／端口、端点 ID 及匹配布尔值，不输出名称、盐、密钥材料或原始广播。mDNS summary 的
lan_candidates 包括匿名候选，visible_peers 仅统计 LAN 自身已有公开名称的记录，不含
UI 层通过 BLE 补全的设备。尚未编译或实机验证。

### Linux 蓝牙连接引导与热点升级

基础探测仍通过以下命令启用；端点 ID 必须取自目标当前广播，不能把示例当作永久设备 ID：

```sh
out/debian/crFiles --quick-share-bootstrap-endpoint=394E374F
```

打开 crFiles 的 Quick Share 设备列表，手机保持接收页面。只使用未过期、含公开名称且
明确提供经典蓝牙 MAC 的广播／GATT 记录，不使用随机 BLE 地址猜测连接入口。
已有用户日志验证经典蓝牙服务连接、UKEY2 和匿名配对成功。握手不等于身份已验证。
基础模式只提供蓝牙介质，不修改电脑网络，每次启动一次，45 秒总期限。

实际 Wi-Fi 热点升级需要额外明确选择 NetworkManager 管理的无线接口：

```sh
out/debian/crFiles --quick-share-bootstrap-endpoint=394E374F --quick-share-upgrade-interface=wlp3s0
```

这会允许临时断开该接口原来的 Wi-Fi 并加入手机提供的热点。手机不需要先加入电脑的
局域网，但必须提供 WIFI_HOTSPOT 升级路径。依赖运行中的 NetworkManager、系统 D-Bus
及当前用户创建／激活连接的权限；不需要 nmcli、辅助脚本或修改 NetworkManager，
不会自动 sudo 或安装依赖。接口无效时热点后端不可用；NM 调用失败会报告升级失败，
不能悄悄当作升级成功。带 bootstrap-endpoint 的命令仍仅做连接验证，
不向手机发送附件。电脑创建热点、Wi-Fi Direct、Windows/macOS 热点升级后端尚未实现；Windows 的 WIFI_LAN 升级见文末。

crFiles 的正式发送界面也支持有公开名称及经典蓝牙连接地址的 BLE-only 设备。
这些设备可以通过蓝牙发送小文件，不要求本机具备热点后端；大文件会在连接前检查热点后端。
正式发送不需要 bootstrap-endpoint 参数；可以只指定 --quick-share-upgrade-interface=wlp3s0。
未指定时仅在 /sys/class/net 中存在唯一无线接口时自动选择（允许当前未连接 Wi-Fi）；
多无线接口环境需要显式选择。点击 Send 后先完整检查源文件，再启动蓝牙连接，通过
蓝牙发送文件介绍并完成验证码和接收确认，让手机无需等待热点连接即可显示接收提示。
源文件总大小不超过 72 KiB（包含零字节文件）时，正式发送保留加密经典蓝牙通道，
双方确认后直接发送文件；阈值按整批文件总大小计算，不按单个文件计算。
此策略在握手前选定，仅声明蓝牙介质，不请求热点升级；对手机主动提供的升级路径
回复 UPGRADE_FAILURE 并保留蓝牙。不会在文件发送途中自动升级或重发。
大于 72 KiB 时双方确认后再进行热点升级和通道往返验证；升级期间显示连接中，
验证通过才发送文件内容。小文件蓝牙直传的对端兼容性仍需实机验证。原 LAN 发送路径
保留。每个发送会话固定本地 endpoint，不因加网期间的发现重置而改变。

`Transfer medium selected` 记录会话选择的介质、总字节数及蓝牙阈值；
`Sending files` 记录实际发送介质。蓝牙直传保留底层发送回调之前的待写记录，
文件进度、最终分块完成和后续交付判断不再把桥接队列写入当作 socket 发送完成。
确认、取消、心跳和交付确认沿用现有逻辑；socket 发送完成本身不代表手机已保存文件。
诊断用 bootstrap-endpoint 探测仍验证热点，不受小文件策略影响。

蓝牙桥接通过事件推进，不再用 10 ms 定时器检查队列或复制会话快照。
工作线程写入数据、读空接收缓冲区、请求关闭，以及 UI 线程收到发送完成回调时，
都会安排一次可合并的 UI 处理任务；工作线程仍通过条件变量等待数据和可写空间。
GATT 释放后用一次性通知启动等待中的经典蓝牙连接；会话完成网络清理并释放 busy
后通知桥接结束。取消会使已排队的 UI 弱回调失效。超时保护仍保留，经典蓝牙设备
发现回退的 100 ms 查询也仍保留，它不参与数据收发。

BLE discovery readiness 日志分别记录首次发现候选和首次具备公开名称、经典蓝牙地址
的候选距扫描启动的耗时，帮助区分匿名广播等待与连接耗时；不代表界面已经完成绘制。
发送期间暂停 BLE 扫描时保留已有广播和 GATT 记录，完成后可以立即恢复尚未过期的设备。
暂停不刷新记录时间，仍按原来的 120 秒寿命过期；关闭发现界面或蓝牙不可用时清空缓存。
GATT 调度优先处理已知但缺少公开名称或经典蓝牙地址的端点，其次处理未知头部，
最后刷新已具备发送信息但读取不完整的候选。只有同一头部已完整读取、读取缓存尚未
过期、且所有已知端点都能通过协议 discovery key、endpoint ID 和兼容头部关联到
完整发送信息时，才跳过重复补全。不凭设备名称、BLE 地址或头部 bloom/hash 猜测关联；
首次出现、头部变化、部分读取仍需补全，完整读取在缓存到期前 15 秒恢复刷新；
持续广播不延长已读槽位的有效期。
连接／读取连续失败使用 30、60、120 秒的有界退避；成功重置失败次数，头部变化按
原有至少 5 秒尝试间隔重新评估，暂停或取消不累计失败。已发起但尚未返回的底层连接
仍串行等待，避免堆积无法取消的操作。
诊断包括 `BLE GATT resolution skipped`、`BLE GATT candidate selected`、
`BLE GATT retry deferred`，以及发送前实际等待的 `gatt_wait_ms`；summary 中
`gatt_ready_skips` 统计跳过状态进入次数，不累计每次调度 tick。

设备移除和地址变更仍会淘汰对应记录。readiness 日志的 record_age_ms 和
retained_before_scan 区分恢复的缓存记录与本轮新发现，cache retention 日志标记暂停状态。

若 BlueZ 尚无目标经典蓝牙设备对象且不提供 Adapter1.ConnectDevice，连接桥会回退到
最多八秒的经典蓝牙发现，仅按协议携带的目标地址匹配，再连接 Quick Share 服务。
该回退不主动配对；目标必须能够被经典蓝牙发现。诊断日志包含 classic discovery
fallback 和 classic discovery matched，连接建立失败按蓝牙连接错误报告。

启用后 ConnectionRequest 和路径请求声明 WIFI_HOTSPOT 客户端角色；收到匹配的
UPGRADE_PATH_AVAILABLE 后：

1. 校验 SSID、WPA 密码、IPv4 网关及端口。创建不保存到磁盘、不自动连接的 NM 配置；
   密码直接通过系统 D-Bus 提供，不写临时文件或日志。禁用该配置的默认
   路由和自动 DNS。当前支持 8–63 字节口令，控制字符、二进制 SSID 及 IPv6 网关不支持。
2. 加入网络后复核网关与新接口 IPv4 同子网，绑定该接口及源地址连接热点 TCP 端口。
   加网期间继续处理原蓝牙通道的心跳和控制消息。网络连接／介绍／旧通道排空共限 40 秒。
3. 在新 TCP 通道发送明文 CLIENT_INTRODUCTION，按对端声明等待可选的介绍 ACK。
   ACK 允许上游使用的只有事件类型、没有空消息体的编码。介绍帧中的
   supports_disabling_encryption 与原加密通道收到的升级路径保持一致。
4. 在旧加密蓝牙通道交换 LAST_WRITE_TO_PRIOR_CHANNEL 和 SAFE_TO_CLOSE_PRIOR_CHANNEL，
   等待本机蓝牙 socket 实际写完关闭屏障，再交换可选的明文 DISCONNECTION（最多 2 秒），
   不推进加密序列号。关闭内存队列后由 UI 线程实际断开蓝牙 socket，不等待整个探测结束。
   只有切换到新通道后才应用协商模式：该标志为 true 时收发明文 Nearby 帧（不再有
   UKEY2 消息层保护），为 false 或缺失时沿用原 UKEY2 上下文及序列号。旧通道的加密
   不受影响，绝不通过解密失败自动降级。正式 BLE 发送使用相同的协商策略。
5. 切换到 WIFI_HOTSPOT 后按协商模式发送心跳，最多等 15 秒验证匹配的应答。
   结果分开记录 handshake_success、upgrade_completed、channel_round_trip 和
   encrypted_round_trip。明文模式成功时 channel_round_trip=1、encrypted_round_trip=0、
   negotiated_plaintext=1，不代表加密验证成功。诊断随后结束并恢复网络；正式发送则继续
   文件传输，成功、拒绝、取消或失败后关闭通道、恢复网络，再释放 busy 状态。

若在通道关闭屏障前失败，发 UPGRADE_FAILURE，仍能在蓝牙通道清理；屏障开始后失败则
终止会话，不能假装恢复应用数据发送而破坏加密顺序。结束时删除临时 NM 配置，并且仅在
接口仍由本次操作控制或已断开时尝试恢复原配置，不覆盖用户中途选择的新网络。恢复和
删除各有结果日志；清理失败会记录临时配置 UUID，便于人工处理。强制杀进程／关机可能
打断恢复，因此验证时应等待网络恢复日志后退出。整个升级探测总期限为 120 秒，取消后
网络恢复在工作线程继续进行，不阻塞 UI。

经典蓝牙探测等待当前 GATT 请求及尚未返回的连接回调结束，探测及网络清理期间暂停
新的 GATT 请求。目标消失或停止发现时解除等待。GATT 长度不足时每槽完整重读一次，
不拼接不同快照；失败保留旧槽位的原到期时间。日志包含槽位、声明／实际长度和特征缺失
原因，遍历完成但有失败记为 partial。22 字节值的底层原因尚未确定。

GATT 诊断按 `request` 和 BLE `address` 关联：`connect requested` 记录请求时的
`gatt_connected`、`services_complete`；`connect callback` 记录回调耗时、错误码、
连接对象状态和 `stale`（请求已结束或暂停后的迟到回调）。迟到回调仍保持原来的
丢弃行为。`services discovered` 单独记录服务发现通知及距请求开始的时间。
`BLE GATT stage` 只在阶段变化时输出，阶段为 `connect_callback`（等待连接回调）、
`wait_services`（等待服务就绪）、`find_service`、`read_characteristic`、`process_read`。
结束日志包含当前阶段、阶段耗时、总耗时、设备存在性、连接状态及服务就绪状态，
便于区分连接回调未返回、服务解析未完成和特征读取未返回。每次特征读取记录槽位、
回调耗时、字节数及错误码；解析成功后记录 endpoint ID，将 GATT 请求关联到端点。
地址和 endpoint ID 都不是经过认证的永久设备身份；日志不输出设备名称或原始数据。
这些诊断不改变发现、超时、重试和缓存策略。

GATT 恢复：周期检查发现已有连接断开时，以 `outcome=disconnected` 结束请求，
保留已成功读取的有效槽位，不再等待完整的 15 秒请求期限。连接回调尚未返回时
仍禁止启动另一个连接，避免堆积未完成的底层操作；`BLE GATT scheduler blocked`
每五秒最多记录一次阻塞请求、地址、等待时间及候选数。
Chromium Linux BlueZ 的 GATT 建连使用新增的 `ConnectWithTimeout`，将 D-Bus
回复等待限定为 20 秒。应用请求先在 15 秒结束时，随后收到的回调标记 `stale=1`，
但仍释放对应 pending 状态，允许后续候选调度。普通 Connect 和配对保留原超时。
这不是对 bluetoothd 操作的强制取消，也不主动断开其他客户端可能使用的已配对连接；
服务解析仍可能失败。此公共后端调整影响 Linux BlueZ GATT 调用者，不限于 crQuickShare。

无可用路径或没有响应仍不能证明手机不支持热点。仓库 Nearby 能构造 UPGRADE_PATH_REQUEST，
但该版本接收分支不完整；Android 实现可能不同。实现也接受手机主动提供的热点路径，
不依赖对路径请求一定有回复。网络凭据只用于本次连接，诊断不打印 SSID、密码或原始帧。

用户提供的实机日志已验证热点升级、协商明文模式的匹配心跳 ACK，以及网络恢复和配置
清理成功。正式头像发送接入仅做源码和差异检查，未编译，仍需验证实际文件传输、拒绝和
取消；连接探测成功不等于这些文件传输场景均已验证。

crFiles 的可发送 BLE 设备使用 endpoint ID 与协议提供的经典蓝牙地址作为界面路由键，
与 SameBluetoothRoute 的合并条件一致，不再把会轮换的 endpoint_info 用于该键。
点击发送时仍从当前有效发现结果取得最新凭据；不复用过期记录、不按名称回退匹配，
也不将路由键作为认证身份。发送前拒绝输出 `Send rejected before session`，
包含 adapter、bluetooth_sender 或 bluetooth_service 层的 reason，不输出文件名或凭据。

热点升级后的通道验证使用独立重试调度：立即发送首个 ping，随后按前一个 ping
完整写出后的 500/1000/2000 ms 间隔最多重试三次（共四个 ping）。输出阻塞时
不会累计排队重试。验证总窗口仍为 15 秒，任何已完整写出的验证序号收到匹配 ACK
即可通过；对端主动心跳只触发正常回复，不替代匹配 ACK 验证。
验证期间暂停普通定时心跳发送，结束后恢复协商间隔，取消和失败退出验证循环。
`Wi-Fi upgrade verification retry queued` 记录重试序号、次数和间隔，验证汇总增加
`retries` 与 `elapsed_ms`，便于比较首次 ACK 缺失时的等待时间。

## Windows Bluetooth and LAN upgrade

The Windows crFiles adapter now includes BLE discovery/GATT slot reads, Fast
Initiation service-data advertising, classic Bluetooth sending and receiving,
and Bluetooth-to-WIFI_LAN migration. Existing LAN transfers remain available.
This implementation has not been compiled or tested on Windows hardware in this
change; no unit tests were added.

The receiver uses a native WinRT GattServiceProvider for FEF3 and a static,
read-only Nearby discovery slot. Windows handles read offsets. Advertising uses
GattServiceProviderAdvertisingParameters.ServiceData (Windows 10 version 1903
or newer) and requires a controller supporting the peripheral role. Only the
Started state is accepted; StartedWithoutAllAdvertisementData is a setup failure.
The GATT provider is stopped on disable/power loss, and advertising is withdrawn
while the shared service is busy. Setup failures are logged and retried after
15 seconds. No hotspot credentials or file contents are logged.

RFCOMM uses the protocol-provided classic address and NearbySharing service UUID
through Winsock SDP resolution, without requiring a WinRT device cache entry.
Connections are nonblocking with an 8-second deadline and cancellation checks.
The receiver explicitly permits uncached incoming peers only for this listener;
Nearby UKEY2 and user confirmation still authenticate/authorize the transfer.
The regular Chromium listener retains its existing cached-peer requirement.

Small batches (at most 72 KiB) stay on Bluetooth. Larger outgoing and incoming
batches request a LAN path, validate a directly attached IPv4 peer, bind its
interface/source address, and reuse introduction, old-channel barriers, retained
UKEY2 encryption and heartbeat verification before streaming. This phase does
not join/create a Windows hotspot or implement Wi-Fi Direct. If Android does
not offer a reachable LAN endpoint, large Bluetooth-originated transfers fail
explicitly. The existing LAN-first receiver path and Linux hotspot fallback are
unchanged.

Manual Windows acceptance: verify BLE discovery and small-file sending; verify
Android discovers crFiles and sends a small file/text; then test both directions
with a larger file on the same subnet. Check medium=WIFI_LAN,
lan_channel_ready, encrypted_round_trip=1 and delivery_confirmed=1 and compare
file hashes. Also exercise cancellation during connect/upgrade, rejection,
Bluetooth off/on, app exit, and consecutive transfers. A different subnet must
fail the LAN upgrade without changing the Windows Wi-Fi connection.

## Linux Wi-Fi Direct GO and client (experimental)

crFiles supports local WIFI_DIRECT GO offers and credential-based Direct clients
using the downstream NetworkManager API. The client requires
`network-manager-go` package `1.58.1-1+crquickshare2` or later, its wpa_supplicant
backend, and a radio supporting concurrent STA + P2P Client/GO. NM owns the
independent interface, DHCP, addresses and teardown. No helper process is used.

On Linux, Settings → Quick Share → Advanced provides an Enable Wi-Fi Direct
checkbox and a Wi-Fi Direct adapter selector. The empty/default selection is
Automatic: select the first available NM-managed Wi-Fi adapter with a free P2P
device, in interface-name order. Explicit selections never fall back to another
adapter. Missing adapters remain visible as unavailable. Refresh updates the
adapter list; activation always rechecks the state on its worker.

To preserve existing connections, the current policy excludes AP, mesh and IBSS
modes, even on hardware that might support additional concurrent combinations.
AP + P2P is not inferred from STA + P2P support. This is a conservative mode
precheck, not a full nl80211 interface-combination solver. Existing LAN transfers
are unaffected. Direct remains opt-in and requires the downstream NM package.

The settings are persisted. `--quick-share-upgrade-interface` overrides the saved
adapter (an empty value selects Automatic); `--quick-share-direct-go` enables both
roles for this run. Overridden settings are indicated and disabled in the UI,
without replacing the saved preferences. The optional preference switch remains
a command-line diagnostic option.

For an outgoing Bluetooth-bootstrap comparison without the initial LAN upgrade
request, disable Enable Wi-Fi Direct in Settings and run:

```sh
out/debian/crFiles --quick-share-upgrade-interface=wlp3s0 --quick-share-prefer-hotspot
```

`--quick-share-prefer-hotspot` requests WIFI_HOTSPOT first and waits up to 15
seconds for the phone's path. It does not create a local hotspot or force the
phone to provide one. Direct opt-in (from settings or command line) takes
precedence and disables this preference. The selected interface is also used by
the hotspot client, which may replace its current Wi-Fi connection while joining
the phone's hotspot. Automatic hotspot selection requires a sole wireless
interface. Existing LAN connections and small Bluetooth-only batches are
unaffected: test with a BLE-only peer and a file larger than 73,728 bytes.
Check `prefer_hotspot=1; hotspot_client=1; direct_client=0` in the transfer
diagnostics and `Upgrade request diagnostics; ... media=3` for the first
upgrade request. `stage=hotspot_join` means a peer path was received; a timeout
with `path_received=0` occurred before local Wi-Fi association was attempted.

Upgrade diagnostics log the initial `Connection request capabilities`,
`Upgrade role diagnostics` (presence and value separately), `Upgrade request
diagnostics`, and `Upgrade request socket completed` (observed local completion,
not a peer receipt). Media values are Bluetooth=2, Hotspot=3, LAN=5, Direct=8.
`Upgrade negotiation received` records peer event and path field presence before
validation; `Upgrade path decision` distinguishes accepted, duplicate, disabled
backend, session-state, unsupported-medium, and invalid-field cases. Incoming
path requests schedule a local Direct GO only when the requested media and peer
client role match an available backend; otherwise they receive UPGRADE_FAILURE
before migration is committed. Late requests after a drain marker are ignored.
These diagnostics
exclude SSIDs, passwords, endpoint identities, keys, and raw serialized frames.

The request layout matches Nearby's `offline_frames.cc::ForBwuPathRequest`:
UPGRADE_PATH_REQUEST → upgrade_path_info → upgrade_path_request → mediums and
medium_meta_data.medium_role. The bundled C++ BWU manager emits this request
under dynamic-role-switch conditions, but its incoming-event switch does not
handle UPGRADE_PATH_REQUEST. This is not evidence of the Android implementation's
behavior; a silent phone still requires phone-side diagnostics to determine why
it did not provide a hotspot path.

Upgrade coordination follows the Nearby separation between offering a network
and joining a peer's network. By default an outgoing bootstrap session first
waits up to five seconds for a peer path (`upgrade_wait_peer_path`). An already
queued valid offer is used immediately. Explicit Hotspot/Direct preferences skip
this initial wait. When no offer arrives, UPGRADE_PATH_REQUEST remains a bounded
compatibility fallback, not an assumption that every peer implements it.

An incoming bootstrap session can offer a local Direct GO when the peer declares
Direct client support (or the existing explicit Direct preference is enabled).
A peer role-switch request can also select this host role for an outgoing
session. A queued peer offer takes precedence over a queued host-role request;
requests arriving during migration are rejected rather than starting a second
network. Local GO creation is attempted at most once per negotiation. No local
Hotspot host or LAN upgrade listener is implemented or claimed by this change.

Before either side sends LAST_WRITE_TO_PRIOR_CHANNEL, a failed join sends
UPGRADE_FAILURE, waits for local Bluetooth write completion, and explicitly
restores/releases its network before retrying. Heartbeats and peer control
messages continue during recovery. New offers may arrive during cleanup or the
5/10-second retry waits. There are at most three attempts under a 120-second
negotiation budget; individual joins retain their shorter timeouts. Cleanup
failure, cancellation, broken Bluetooth, or either drain marker prevents retry.
There is no large-file fallback to streaming on Bluetooth. Diagnostic stages are
`upgrade_network_recovery` and `upgrade_retry_wait`. This recovery cannot make a
phone hotspot visible when the phone or radio does not broadcast/discover it.

Alternatively, enable both roles with an explicit interface:

```sh
out/debian/crFiles --quick-share-upgrade-interface=wlp3s0 \
  --quick-share-direct-go --quick-share-prefer-direct-go
```

For outgoing files, crFiles follows a valid phone-provided LAN or Direct path.
It advertises the Direct client role and does not create a competing local GO.
Without the preference switch it requests LAN first, then a phone Direct path;
with the switch it requests Direct first when no usable phone path is pending.
The switch still prefers a local GO for incoming files when no phone path is
pending. A pending usable phone path takes precedence in either direction.
Small outgoing batches remain on Bluetooth.

The client waits for phone acceptance before activating a volatile, user-restricted
`wifi-p2p.client` profile with SSID, PSK and optional frequency. A private D-Bus
connection binds the activation lifetime to the session. IPv4 DHCP must succeed;
phone-provided default routes, extra routes and DNS are not adopted. The TCP
connection is bound to the new interface and its directly attached IPv4 address.
The station's active connection is checked during setup. An occupied P2P device
is rejected without taking over an unrelated group. IPv6-only Direct paths are
not supported by this client backend.

After TCP connects, the client sends CLIENT_INTRODUCTION and waits for its ACK
when advertised and drains Bluetooth through both barriers. The Direct client
echoes the encryption mode from the authenticated phone upgrade path: when
supports_disabling_encryption is true, the replacement channel uses plaintext
Nearby records over the WPA-protected Direct link; otherwise it retains UKEY2.
It verifies the replacement channel with a matching heartbeat ACK. Repeated
identical path offers during joining do not restart or reject the active join.
File bytes are sent only after channel verification. Failure/cancellation closes
the data channel and releases the NM activation through normal session cleanup.
GO opt-in continues to disable the station-replacing hotspot backend.

This client integration has been source-reviewed only: Chromium was not built
and no phone transfer was run for this change. NM interface creation/cancellation
validation is recorded in `../network-manager-go/contrib/crquickshare/VALIDATION.md`.
Manual validation should cover outgoing Direct and LAN, phone rejection, cancel
during NM association/DHCP, small Bluetooth batches, and incoming GO regression.
Expected outgoing Direct logs: `direct_client_join`, `Direct client NM state`,
`Direct client ready`, `Direct client TCP connect`, `direct_channel_ready`,
`channel_round_trip=1`, `delivery_confirmed=1`, then client network cleanup.
For a phone path requesting plaintext, expect `negotiated_plaintext=1` and
`encrypted_round_trip=0`; this is not a verification failure.
Verify file hashes and confirm that the original station and default route remain
unchanged and that the temporary P2P interface/profile disappears after cleanup.

The old `--quick-share-direct-go-helper` switch is replaced by
`--quick-share-direct-go`. The backend does not invoke the legacy Python helper,
pkexec, nmcli, or a second supplicant. Run the application as the desktop user;
NM applies its normal connection/Polkit authorization. No new passwordless policy
is installed. Setup and peer association share the existing 40-second deadline.

The GO backend locates NM's `p2p-dev-<station>` device of type 30 and rejects an
already active P2P connection. It creates a `wifi-p2p` profile with
`autonomous=true`, `frequency=0`, `ipv4.method=shared`, `ipv6.method=link-local`,
and never-default addressing. NM chooses the station's current frequency when
associated. The profile ACL is restricted to the application user, and
AddAndActivateConnection2 uses `persist=volatile` and
`bind-activation=dbus-client` on a private, long-lived D-Bus connection. The NM
unique bus name is pinned so a daemon restart cannot redirect cleanup to reused
object paths. D-Bus waits continue servicing the Bluetooth channel.

After activation, GetGroupCredentials supplies SSID/passphrase/frequency and
the group interface. IpInterface and Ip4Config.AddressData supply the actual
address and subnet, checked against the local interface. The TCP listener binds
only that group address on an ephemeral port; a usable IPv6 link-local listener
is optionally advertised on the same port. No fixed subnet is assumed. NM owns
DHCP and interface teardown. Its shared IPv4 mode also manages forwarding/NAT
and firewall rules; it is not the legacy helper's DHCP-only network.

The existing upgrade protocol publishes credentials over the encrypted Bluetooth
channel, checks CLIENT_INTRODUCTION endpoint identity, retains UKEY2 encryption,
performs both old-channel barriers, and verifies an encrypted heartbeat before
accepting file data. Credentials and D-Bus error bodies are not logged.

On completion/cancellation the backend closes listeners and deactivates only
its own active connection. Cleanup success requires removal of the active
connection, the captured group interface, and the volatile profile. Closing the
private D-Bus connection also releases the NM activation lease, including when
an activation reply was lost. A cleanup timeout logs failure rather than claiming
teardown completed. The station is never explicitly deactivated or restored.
Its active connection is checked during setup and while waiting for the peer;
radio/driver side effects still require hardware validation.

Manual acceptance: send a >72 KiB file from Android and check `direct_go_create`,
`Direct GO ready; backend=NetworkManager`, `direct_go_accept`,
`direct_channel_ready`, `medium=WIFI_DIRECT`, `encrypted_round_trip=1`,
`delivery_confirmed=1`, and `Direct GO cleanup ... success=1`. Compare file hashes
and check the station address/default route. Repeat for cancellation, denied
permissions, peer rejection, application exit and a second transfer. Confirm no
GO interface, shared DHCP process or temporary profile remains. No Chromium
compilation or unit tests were run for this integration.
