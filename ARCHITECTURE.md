# Sona 音频架构规范

状态：阶段一（运输与恢复）与阶段二（音频引擎外移）均已在真实宿主上验收，`make check-driver-boundary` 通过，驱动不再调用 client HAL（见文末记录）。阶段三（生产验证）未开始。

## 不可跨越的进程边界

> Anything that calls Core Audio client HAL lives outside the AudioServerPlugIn host process.

Apple [QA1811](https://developer.apple.com/library/archive/qa/qa1811/_index.html) 明确规定 AudioServerPlugIn 不得调用 CoreAudio.framework 的 client HAL API，否则产生 undefined behavior。把调用放在配置队列、避开实时回调或通过回归测试，都不能解除此限制。

此规则包括设备枚举、属性读取和写入、监听器注册和移除、设备启动和停止、IOProc 生命周期、硬件音量及默认设备操作；也包括通过辅助库或动态解析间接调用。插件正常实现 AudioServerPlugIn 接口、使用 Core Audio 数据类型以及调用宿主提供的 AudioServerPlugInHostInterface，不属于 client HAL 调用。

## 进程职责

```text
应用音频 → Sona Virtual Device / AudioServerPlugIn
                      │ 每客户端带时间戳的共享内存 ring
                      ▼
              Sona Audio Service
              客户端归并、增益、路由、混音、DSP
                      │ 每物理输出独立 ring / SRC / 时钟校正
                      ▼
              Core Audio client HAL → 真实输出设备

Sona 菜单栏 App ← 控制通道 → Sona Audio Service
AudioServerPlugIn ← Mach 控制通道 → Sona Audio Service
```

插件负责虚拟设备对象模型、客户端注册、虚拟时间线、接收 PCM 和有界写入。实时路径禁止分配/释放内存、文件访问、阻塞锁、同步 RPC、连接重建、设备发现和高层对象生命周期操作。断线或 ring 已满时丢弃数据并记录有界计数，立即返回。

服务负责物理设备所有权、配置、路由矩阵、应用及输出增益、静音、信号检测、混音、DSP、SRC、漂移校正和故障恢复。App 负责用户交互；已有 App 侧 HAL 操作满足进程边界，但应明确与服务之间的默认设备和配置所有权，避免双方同时恢复系统输出。

## 客户端与时间线

- 在 `ProcessOutput` 捕获每客户端未混音数据；不能用 `WriteMix` 的混合结果替代。遵守宿主输出操作契约，不将同一音频重复送入服务。
- ClientID 区分客户端，同一进程可有多个客户端。PID 可复用，BundleID 可为空；两者都不是 ring 的唯一身份。
- 运输身份使用连接世代、客户端实例序号和 stream ID；保留 ClientID、PID、可选 BundleID 作为元数据。客户端删除后重新出现必须获得新的实例身份。
- 服务把多个客户端归并为逻辑应用；辅助进程归属需明确策略，不能假定 BundleID 永远代表用户所见应用。
- 每块 PCM 携带有效帧数、格式世代、输出 sample time、有效时间戳标志和 discontinuity 信息。使用 `AudioServerPlugInIOCycleInfo` 的输出时间线，不能用消息到达顺序代替播放时间。
- 混音按共同时间线对齐。缺块填零、迟到块丢弃、重复块拒绝；时间线重置必须重置相关历史和 SRC 状态。明确周期完成或播放截止规则，避免将本周期尚未提交的客户端误判为静音。

## IPC 与内存所有权

控制通道负责协商、客户端元数据、映射交接、配置和状态；PCM 仅经过共享内存。插件 Info.plist 必须通过 `AudioServerPlugIn_MachServices` 声明实际使用的服务名。普通 XPC 消息不能作为逐音频块的运输方式。

协议至少定义：`protocolVersion`、连接 `generation`、`ringID`、客户端实例、格式及格式世代、容量、读写序号、帧数、sample time、host time 及有效标志。布局使用明确宽度、对齐、长度和 offset，不传递进程私有指针或 C++ 对象图。验证所有长度、容量、格式和协议版本；不兼容时保持可恢复的断线状态。

每个客户端 ring 只有一个生产者和一个消费者。服务的混音执行路径消费客户端 rings，向各 sink ring 写入；各设备 IOProc 消费各自的 sink ring。必须验证实际回调并发约束；若存在多个生产线程，不能继续假定 SPSC。

共享原子操作必须在支持的平台上实际 lock-free，并定义 acquire/release 发布顺序、计数器回绕及缓存行布局。映射与初始化、预触页、容量规划均在非实时路径完成。溢出时生产者不得擅自推进消费者读指针；消费者根据时间戳丢弃过期积压，防止恢复时播放陈旧音频。

`generation` 用于拒绝旧会话数据，不能代替安全回收。映射切换采用发布新映射、实时路径确认不再引用旧映射、非实时路径解除旧映射的顺序。客户端退出、服务退出和格式变更均遵守该顺序；实时线程不等待确认。限制客户端数量、映射大小和退休映射总量，超过预算时丢弃或拒绝新流，不能无限分配。

## 时钟、音质和延迟

保留现有 HQ sinc SRC 和各物理输出独立时钟校正，先迁移再改变算法。不同 sink 的同名采样率不代表时钟相同；只有在已验证时钟锁定和格式兼容的条件下才允许逐帧直通。

虚拟设备必须在服务离线时仍能提供合法、单调的时间线。若通过共享时钟锚点跟随物理主设备，读取必须有界；锚点过期、主设备变化、睡眠唤醒时进入明确的重同步状态，并按宿主契约更新时间戳 seed。实时路径不能为获取锚点调用 HAL 或等待服务。

当前 `SonaTarget` 已有异步 ring，目标填充常量为 2048 帧，直通余量为 512 帧；它们不等同于所有模式的实际延迟。迁移验收记录运输缓冲、混音调度、sink 填充、安全偏移、SRC lookahead 和硬件延迟。通过回环测量报告实际端到端延迟及分布，不能用 128 / 48000 的单块时长承诺总延迟。虚拟设备报告的延迟属性也必须与选定时钟和路由策略一致。

## 生命周期与部署

服务由 launchd 管理，其存活不得依赖菜单栏窗口或 App 进程。选择服务注册域时必须验证 coreaudiod 的 sandbox 与 bootstrap 可达性；不能假定用户会话中的 LaunchAgent 自动对系统宿主可见。首个集成里程碑必须验证真实宿主握手。

服务注册方案同时定义 IPC 对端身份校验、配置修改授权、快速用户切换、注销及配置归属。重启采用退避，避免持续崩溃循环。服务重启创建新连接世代，重新登记客户端、重建 rings、恢复配置和 sink 状态，清除旧时间线数据，输出淡入。

服务离线时插件正常返回，不执行 HAL 回退、不阻塞音频线程。若产品需要在长期失效后恢复系统默认设备，必须由仍存活的外部监督者执行；只有设备仍是 Sona 且未被用户改变时才恢复，不能与用户操作竞争。没有外部监督者时应明确接受暂时静音，不能声称会自动恢复到扬声器。

## 迁移阶段与验收

1. **运输与恢复**：实现协议、共享映射、每客户端时间线和安全回收。先验证真实 coreaudiod 到 launchd 服务的连接，然后测试客户端并发启停、超容量、服务断线、旧世代拒绝、格式切换和重连。实时路径不得等待服务。
2. **音频引擎外移**：迁移 `SonaTarget`、硬件控制、设备监听、增益、路由、混音、信号检测、SRC 和时钟；将 App 的配置和状态接入服务。保留逐应用多输出、音量键代理、硬件/数字音量回退、设置恢复及菜单栏行为。二进制 HAL 边界检查必须通过。
3. **生产验证**：安装/升级/卸载与 launchd 生命周期验证；注入服务崩溃、coreaudiod 重启、设备热插拔、采样率/缓冲区变化、睡眠唤醒、用户切换。验证长时间漂移、音质、CPU、丢帧、恢复时间和端到端延迟，并记录硬件、系统版本及测量方法。多设备漂移稳定不等于设备间采样级同步。

## 构建门禁与当前差距

阶段一已落地的部分：

- `Shared/SonaTransport.h`：协议版本、共享区域头、每客户端 SPSC ring（块描述符 + 采样环）、生产者/消费者内联函数，布局有 static_assert。
- `Driver/SonaTransportClient.*`：驱动侧 XPC 控制通道、区域映射与校验、退避重连、客户端实例序号、映射退休（等待实时线程释放后再 unmap）。`ProcessOutput` 把每客户端未混音 PCM 连同 `mOutputTime` 写入 ring；服务离线时丢弃并立即返回。
- `Service/main.cpp`：launchd Mach 服务 `com.sona.audio-service`，每次握手创建新 generation 的区域，登记客户端，定时消费 ring 并统计。阶段一只计数不发声。
- 验证：`make test-transport`（布局、回绕、丢弃/不连续标志、跨线程 SPSC 压力）；`make test-loopback` 需先运行服务，验证握手、写入、服务被 kill 后的换代重连（已在用户域完成三代重连验证）。
- `Driver/Info.plist` 已声明 `AudioServerPlugIn_MachServices`。

阶段一验收记录（2026-09-19，macOS 27.0 26A428，Apple Silicon）：

- 真实宿主握手：`sudo make install-service` 后 `sudo make install-driver`，coreaudiod 插件宿主连上 system 域 LaunchDaemon，`sonactl service` 报 `connected: true`。sandbox 未拦截 Mach 名。
- 真实客户端：22 个客户端登记；播放中的 `com.netease.163music` blocks 持续增长，`droppedBlocks`、`discontinuities`、`staleBlocks` 均为 0。
- 服务崩溃注入：`launchctl kill KILL` 后 launchd 拉起新进程，generation 换代，驱动自动重连并以新 instance 重新登记客户端，音乐不中断。
- coreaudiod 重启：`killall coreaudiod` 后新宿主重新握手，服务建立新 generation，客户端重新登记，旧 session 无残留块。
- 格式切换：48k → 44.1k → 48k，服务端 `formatGeneration` 1 → 2 → 3，采样率同步，无丢块。

阶段二已落地的部分（2026-09-19）：

- `Driver/SonaDriver.cpp` 重写为纯传输适配器：`ProcessOutput` 把每客户端 PCM 写入 ring 后清零宿主缓冲；不再声明 `WriteMix`。自定义属性保留为门面：Config/OutputLevel/主音量写入转发给服务，Clients/Status/Config 读取返回服务推送的快照。`GetZeroTimeStamp` 通过共享区域头页里的时钟 cell（seqlock）跟随服务主输出的锚点，并把锁定状态写回 `driverLocked` 供服务做 1:1 直通。驱动改用 `os_log_create("com.sona.driver")`。
- `Service/SonaEngine.cpp`：从原型搬入配置解析、路由、目标设备生命周期、硬件音量、输出级别、信号检测；新增混音线程（1 ms 周期，time-constraint 策略）按 sampleTime 对齐各客户端块，迟到块丢弃、时间线跳变重置、格式世代变化重置，写入各 sink ring。状态持久化到 `/Library/Application Support/Sona/service-state.plist`。
- `Service/SonaTarget.cpp`、`SonaResampler.h`、`SonaRingBuffer.h`、`SonaHardwareControls.h` 原样迁入服务，只把锚点发布改为写共享 cell。
- 协议升级到 v2：新增 Config/OutputLevel/Master 转发与 StatusPush/ClientsPush/ConfigPush 推送；plist 以二进制 CFPropertyList 数据传输。
- 测试：`make test`（引擎 mock HAL：配置、硬件控制、输出级别、持久化、混音器、活动检测、生命周期）、`make test-audio`、`make test-transport`、`make test-clock`（驱动时钟锁）、`make check-driver-boundary` 全部通过。用户域回环：服务打开内建扬声器 IOProc，混音线程消费 773 块，队列深度 1，无 underrun、无丢块。
- 边界检查通过：驱动 Mach-O 不再导入任何 `AudioObject*`/`AudioDevice*`/`AudioHardware*` 符号。

阶段二验收记录（2026-09-19，macOS 27.0 26A428，Apple Silicon）：

- 安装新服务与新驱动后，网易云音乐从扬声器正常出声；服务报告扬声器 sink `passthrough: true`、`clockLocked: true`、underruns 0、无丢块，驱动时钟成功锁定物理设备。
- 逐应用音量：通过驱动门面推送 0.2 / 0.05 / 1.0，服务接受并回推 config；用户听到的变化未单独确认，但引擎测试已覆盖增益路径。
- 音量键：用户确认正常，系统音量条随硬件音量变化。
- 服务崩溃注入：静音 1 到 2 秒后自动恢复出声，符合规范"接受暂时静音"。重启后 config 从 `service-state.plist` 恢复，新 generation 重新锁定时钟。
- 多输出：把应用路由到扬声器 + BlackHole，两个 sink 同时 active，BlackHole 走 SRC 路径（非直通）；通过门面设置 BlackHole 数字音量 0.5 与静音，状态正确回推；撤销路由后 BlackHole 关闭。

阶段三进行中（2026-09-19）：

- `install-driver` 现依赖 `check-driver-boundary`；`make test-all` 覆盖全部单元测试与门禁。
- App 解析 `serviceConnected`，服务离线时菜单顶部显示提示。
- Status 新增 `sinkFillFrames`（每 sink）、`mixLagFrames`、`mixLateBlocks`、`mixResets`，用于延迟与调度观测。
- 稳定性：网易云音乐播放 90 s 采样，扬声器 sink underruns 0，passthrough/clockLocked 全程 true，吞吐恒定 ≈ 94 块/s，ring 队列深度 0–1，无丢块。服务 CPU 0.9%（单核百分比），RSS 28 MB，5 线程。

真机验证（用户操作，2026-09-19）：

- 热插拔：插拔耳机期间音乐不中断，事后扬声器 sink 仍 passthrough/clockLocked，underruns 0，mixResets 1（对应一次时间线重置），无丢块。
- 睡眠唤醒：唤醒后自动恢复出声，服务未重启（pid 不变），驱动重新锁定时钟。
- App 提示：正常时无提示；服务被 kill 的窗口内菜单显示"Sona Audio Service 未运行"，重启后消失。
- 观测：sink 填充稳定在 1536 帧（32 ms @ 48 kHz）；这是当前直通模式下服务侧的主要缓冲，加上 coreaudiod 到驱动的 IO 周期即为新增延迟上限。

缺陷与修复（2026-09-20）：两个应用同时播放（网易云音乐 + 浏览器抖音）时一路静音、另一路卡顿，`mixResets` 每秒数百次。原因是混音器按"周期"对齐，假定同一虚拟设备上所有客户端的块共享同一 sampleTime；实际上不同客户端使用不同 IO 缓冲大小（128 与 512 帧），块的起点和领先量都不同，被互相判成时间线跳变。重写为按宿主时间推进提交边沿：以最新的 (sampleTime, hostTime) 对锚定时间线，边沿 = 现在 + margin 对应的 sample time，margin 跟踪最近两秒内最小领先量减 2 ms；块按绝对 sample 位置累加，落后边沿的块只贡献未提交部分并计为 late。`Driver/Tests/TransportLoopback2.cpp` 用 128/512 帧双客户端回环验证：late 0、resets 1、丢块 0。规范第 40 行"周期完成或播放截止规则"以此实现。

默认输出跟随（2026-09-20）：用户反馈插入耳机或连接蓝牙后默认目标仍是扬声器。策略放在 App（规范第 31 行：默认设备归属由 App 明确），`AppModel.refreshDevices` 在设备列表变化时检测新出现的内建/USB/蓝牙/雷雳输出并把默认目标切过去，记住切换前的目标；该设备离开时回退，用户手动选择会取消回退。菜单设置里有"新设备接入时自动切换"开关，默认开。HDMI、AirPlay、虚拟/聚合设备不触发。

评审修复（2026-09-20，四项）：

- **ring 只由混音线程消费。** 原来 `SonaEngineClientAdd` 在客户端对混音线程可见之后从控制队列调用 `SonaTransportRingFlush`，与混音线程的 Peek/Consume 并发改读指针，离线复现可把 `blockRead` 推过 `blockWrite`，之后生产者永远判满。现在控制队列不再触碰任何 ring；混音线程按块的 instance/formatGeneration 与已登记值的差值分类（`SonaClassifyBlock`）：旧实例或旧格式的块由它自己消费丢弃，比已登记值更新的块（clientAdd 消息晚于首块到达）留在队头等待登记。`SonaMixReset` 也不再从控制队列调用，改为置位 `gMixResetRequested` 由混音线程执行。
- **对端身份校验。** 驱动 Mach 服务 `com.sona.audio-service` 对每个接入连接调用 `xpc_connection_set_peer_code_signing_requirement`，要求 `anchor apple and (identifier "com.apple.audio.coreaudiod" or identifier "com.apple.audio.Core-Audio-Driver-Service" or identifier "com.apple.audio.Core-Audio-Driver-Service.helper")`（已用 `codesign --verify -R` 对 macOS 27 上的三个宿主二进制核对）。libxpc 按每条消息背后的代码签名校验，不满足的连接在 `HandleMessage` 之前就被切断，其 hello 以 `XPC_ERROR_CONNECTION_INTERRUPTED` 结束，服务日志记 "rejected pid … not the plug-in host"。只读状态查询移到第二个 Mach 服务 `com.sona.audio-service.status`（任何调用者可用；`sonactl service` 已改用它）。测试服务通过环境变量 `SONA_PEER_REQUIREMENT` 换成 `identifier "com.sona.test.loopback"`，回环工具以该标识符 ad-hoc 签名。
- **会话替换语义。** 合法宿主的新 hello 替换当前会话：旧区域从引擎撤下，旧连接若不是同一条则 `xpc_connection_cancel`，其持有者收到 `XPC_ERROR_CONNECTION_INVALID`，按既有逻辑丢弃映射并退避重连；不会再有驱动往无人消费的旧 ring 里写。两个同时存活的宿主会互相替换（最新 hello 胜出），生产环境只有一个插件宿主，`make test-e2e` 第 3 步刻意制造这种交替来验证被替换方重连。
- **回收必须有读者确认。** `SonaEngineDetachRegion` 撤下指针后记录每个读者的通行计数（混音线程 `gMixerPass`，每个 sink IOProc 的 `clockPass`，奇数表示正在区域内），返回 `SonaEngineRetireTicket`；`SonaEngineRegionRetired` 只在每个读者要么当时不在区域内、要么计数已前进时才为真。读者一侧先做 seq_cst 计数再加载指针，控制侧先 seq_cst 清指针再读计数，构成 Dekker 顺序。`main.cpp` 只在票据已退休时 `munmap`；否则会话进入 `gRetired` 每 50 ms 轮询，最多保留 8 个（约 9 MB 一个），超出则拒绝新 hello 而不是解除仍可能被访问的映射。`sonactl service` 的 `retiredRegions` 显示积压数。
- **尾音排空。** 混音器记录累加区有效末端 `gMixAccumEnd`；输入 ring 为空时仍把 `[gMixedUpTo, min(edge, gMixAccumEnd))` 提交到 sink，之前的提前 return 会把最后一块中超过当次边沿的部分永远留在累加区。

对应测试：`make test` 新增 session 段（登记不动读指针、未登记实例的块等待、退休票据在读者离开前不为真）和尾音排空用例；`make test-e2e` 把新构建的服务引导进用户 launchd 域（无需 sudo），验证签名放行、同一二进制换签名后被拒、会话替换后重连、状态监听器可用且无滞留区域，结束后自动 bootout。`make test-loopback` 现在需要一个用 `SONA_PEER_REQUIREMENT` 放行测试签名的服务，系统守护进程会拒绝它。

未完成：快速用户切换、端到端延迟回环实测、多小时漂移（因混音缺陷中断，需在修复后重新采样）。服务日志已改用子系统 `com.sona.audio-service`，下次安装后可用 `log show` 查看。

`make check-driver-boundary` 构建驱动并扫描最终 Mach-O 的所有架构，拒绝 `AudioObject*`、`AudioDevice*`、`AudioHardware*` 和旧式 `AudioStream*` client API 导入。无白名单，检查工具失败也返回失败。`make production-driver` 依赖此检查。

`make test-all` 运行全部单元测试和边界检查。`install-driver` 已依赖 `check-driver-boundary`。导入检查只是必要条件：动态解析、依赖库间接 HAL 调用、实时约束及生命周期正确性还需要审查和运行验证。

阶段一和阶段二已有代码、本地测试与上述部分真实宿主验收记录；这些历史记录不代表所有生产场景通过。阶段三生产验证仍未完成，具体待验项目和本次测试结果见 [兼容性记录](docs/COMPATIBILITY.md)。
