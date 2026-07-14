# T2 BoT-SORT 算法一致性验证与修复 spec

## 背景与范围

本仓 `gst/ssv-track/botsort/` 已迁移 BoT-SORT 的 Kalman、GMC、IoU 关联和轨迹生命周期能力。为避免“接口可用但算法语义偏离”，本阶段以 Python 基准实现为唯一算法参考，验证已迁移能力并修复确认的偏差。

本工作属于 `T2` 感知算法与元数据主线。

固定基准如下：

| 项目 | 值 |
| --- | --- |
| 基准仓库 | `/mnt/work/BoT-SORT` |
| 基准分支 | `feat/python-to-cpp` |
| 基准提交 | `1f7d73314e9e14148fb4acf597997c7e5d0bb455` |
| 参考实现 | `tracker/bot_sort.py`、`tracker/kalman_filter.py`、`tracker/matching.py`、`tracker/gmc.py` |

仅验证已迁移的无 ReID 路径：检测阈值分流、8 维 Kalman、IoU 与 score fuse、三阶段关联、轨迹生命周期和 `sparseOptFlow` GMC。ReID、embedding、其他 GMC 方法、跨摄像头跟踪及源仓库未迁移功能不在本阶段范围内。

本阶段的唯一算法基准是上表所列 Python 源码。历史移植分支以及 `/mnt/work/BoT-SORT/cpp` 中的 C++ 实现均不参与比较、佐证或验收；它们至多可作为仓库历史信息，不能影响本阶段结论。

## 不变约束

1. `SsvDetection`、`SsvDetectionStore`、Redis detection JSON、bbox 归一化格式和下游消费者的数据契约保持不变。
2. `ssvtrack` 仍只向原检测写回 `track_id`、`track_state`、`occluded`；不得因算法内核修复改写下游可见 bbox、类别或置信度。
3. 默认配置以 Python 无 ReID 路径为准：`enable_score_fuse=true`、`enable_class_constraint=false`。
4. 所有修复先由确定性对照用例证明失败，再修改实现；不以真实视频回放替代确定性验收。

## 验证设计

### 双层证据

第一层为逐项静态审查，逐段对应 Python 与 C++ 的状态、代价、阈值和集合转换。

第二层为确定性逐帧对照：同一组像素坐标检测、同一配置和预定义仿射 warp 分别驱动 Python 基准与 C++ 内核；每帧比较活动、丢失、移除和未确认轨迹的 ID、状态、bbox、8 维均值及 `8×8` 协方差。浮点比较采用明确容差，离散状态和 ID 必须完全相同。

GMC 图像估计本身依赖 OpenCV 特征点，不作为跨语言逐 bit 比较对象。对 GMC 算法语义的验证直接注入固定仿射矩阵，隔离“运动估计”与“状态应用”两个问题。

### 必覆盖场景

| 场景 | 断言 |
| --- | --- |
| 高分连续检测 | 首帧激活、后续第一阶段复用 ID、Kalman 状态演化一致 |
| 低分续接 | 仅第二阶段匹配，且不得新建 ID |
| 短暂丢失后重现 | lost 到 re-activate 的 ID、状态和协方差一致 |
| 未确认轨迹 | 次帧确认或移除的状态转换一致 |
| 超过 `track_buffer` | lost 轨迹在相同帧移除 |
| 阈值边界 | `score == track_low_thresh`、`score == track_high_thresh`、`score == new_track_thresh` 的分流一致 |
| 平移 GMC | 中心、速度、协方差及后续关联一致 |
| 旋转/缩放 GMC | 8 维状态和协方差完整变换，与 Python `multi_gmc` 一致 |
| 并列代价 | 记录 Python `lapjv` 和 C++ Hungarian 的匹配选择，判定是否需要统一 tie-break |

## 已确认差异与处理决策

### D1：GMC 未完整变换 Kalman 状态（已修复）

Python `STrack.multi_gmc()` 以 `R8x8 = kron(I4, R)` 变换完整状态和协方差：

```text
mean' = R8x8 · mean
mean'[0:2] += translation
covariance' = R8x8 · covariance · R8x8ᵀ
```

当前 C++ 仅 warp bbox，再将转换后的 `cx/cy/w/h` 写回 `mean[0..3]`。速度 `mean[4..7]` 与协方差没有补偿；对含旋转或缩放的 warp，C++ 的 bbox 外包矩形语义也不同于 Python 的状态向量变换。

修复方案：新增独立的 GMC 状态变换函数，直接接收 `KalmanState` 和 `GmcWarp`，严格按上述矩阵计算。对 tracked、lost 与 unconfirmed 轨迹在关联前应用；bbox 统一由变换后的 Kalman 状态导出。该修改只影响内核预测状态，插件写回契约不变。

### D2：高低分阈值边界不同（已修复）

Python 使用严格比较：低分过滤为 `score > track_low_thresh`，高分为 `score > track_high_thresh`。当前 C++ 使用 `score < low` 过滤和 `score >= high` 分类，导致等于阈值时进入不同集合。

修复方案：C++ 改为：

```text
score <= track_low_thresh：丢弃
score > track_high_thresh：高分
其余：低分
```

`new_track_thresh` 保持 Python 现有语义：仅 `score < new_track_thresh` 时不激活，等于阈值可以新建轨迹。

### D3：内核输出 bbox 与 Python 轨迹 bbox 不同（已修复）

Python `BoTSORT.update()` 返回的轨迹 bbox 来自 Kalman 更新后的状态；当前 C++ `UpdateResult` 返回原始检测 bbox。两者的 ID 和内部下一帧预测可以相同，但算法 DTO 输出不严格一致。

修复方案：匹配后的 C++ `UpdateResult` 使用更新后 Kalman 状态导出的 bbox，并保留检测的 `input_index`、分数、类别和跟踪字段。`gstssvtrack.cpp` 继续只写回跟踪字段，因此外部检测 bbox 契约不变。

### D5：sparseOptFlow 过早依赖当前帧特征点数量（已修复）

Python `GMC.applySparseOptFlow()` 用上一帧的 `prevKeyPoints` 调用 `calcOpticalFlowPyrLK`，是否估计仿射矩阵只取决于光流成功后的对应点数量是否大于 4。当前 C++ 在调用光流前额外要求当前帧 `goodFeaturesToTrack` 的结果也不少于 5；因此当前帧新检测特征点较少、但上一帧点仍可成功跟踪时，C++ 错误退化为单位矩阵。

修复方案：仅以 `prev_points_` 是否可用于光流作为前置条件；光流完成后再按成功对应点数量决定是否调用 `estimateAffinePartial2D`。当前帧特征点仅用于保存为下一帧的 `prev_points_`，不得影响当前帧 warp 估计。

### D4：匹配求解器 tie-break 可能不同（先验证后决策）

Python 使用 `lap.lapjv`，C++ 使用自实现 Hungarian。二者在存在唯一最优解时应给出相同匹配；完全相同代价的多解可能选择不同组合并交换 ID。

先实现并列代价对照夹具。若 Python 的固定版本与 C++ 产生不同 ID，采用以下优先级决策：

1. 若业务仅要求最优代价与有效匹配一致，将 tie 视为不唯一解，在对照报告中单列，ID 不做逐项相等断言。
2. 若必须逐 ID 与 Python 一致，移植或引入可复现的 LAPJV 求解器，并以固定依赖版本锁定行为。

未验证前不得通过索引微扰代价来伪造一致；那会改变 Python 基准的语义。

## 审查结论

| 组件 | 结论 | 依据 |
| --- | --- | --- |
| Kalman 初始化、预测、更新 | 一致 | 状态维度、转移矩阵、噪声权重和更新方程与 Python 相同；允许 `float32/float64` 容差 |
| IoU 与 score fuse | 一致 | 均使用 `1 - IoU` 与 `(1 - cost) * score` |
| 三阶段关联与生命周期 | 一致 | 高分第一阶段、低分第二阶段、未确认第三阶段及 lost TTL 与 Python 流程对应 |
| GMC 估计时序 | 一致 | C++ 先估计 warp、Python 先预测；两操作在 sparse-opt-flow 下独立，且都在关联前应用 warp |
| GMC 状态应用 | 已修复并通过 8 维状态回归 | D1 |
| sparseOptFlow 前置条件 | 已修复并通过回归 | D5 |
| 阈值边界 | 已修复并通过边界回归 | D2 |
| 内核输出 bbox | 已修复并通过 Kalman bbox 回归 | D3 |
| 求解器并列最优行为 | 源码审查完成；运行时对照受 Python `lap` 缺失阻断 | D4 |

## 完整审核矩阵

“一致”仅表示已按固定 Python 基准完成静态核对；还需由“验证设计”中的确定性夹具提供执行证据。未迁移功能已经在“背景与范围”中排除，不出现在本表。

| 范围 | 当前状态 | 后续动作 |
| --- | --- | --- |
| `BoTSortTracker::update()`：高低分分流、三阶段关联、激活、丢失、重激活、超时移除 | 已审，D1-D5 除外一致 | 用逐帧夹具覆盖唯一最优、遮挡、未确认与超时场景 |
| `KalmanFilter.initiate/predict/update` | 已审一致 | 比较 8 维均值和 `8×8` 协方差；允许声明的浮点容差 |
| `BoTSortKalman::multi_predict()` | 已删除；主路径保留 Python 等价的 lost 速度清零 | 删除完成并通过构建回归 |
| IoU、score fuse、阈值过滤 | 已审；阈值边界存在 D2 | 补低阈值、高阈值和新轨迹阈值等值用例 |
| `linear_assignment` | 源码审查：唯一最优、阈值过滤和虚拟未匹配扩展与 Python `lapjv(extend_cost=True, cost_limit=thresh)` 语义对应；并列最优 ID 选择无法实测 | 安装固定版本 Python `lap` 后，以唯一最优、阈值等值、矩形矩阵和并列最优对照；按 D4 决定是否统一求解器 |
| `remove_duplicates` | 一致：相同 IoU `< 0.15` 条件；均按轨迹存续帧数保留较长者，时长相等时删除 tracked 集合中的轨迹 | 增加多对 overlap、轨迹时长相等和不同的回归用例 |
| `sparseOptFlow` warp 估计 | D5 已修复；OpenCV 固定图像序列对照仍未执行 | 运行 Python/C++ 固定 BGR 帧对照，记录 warp 容差 |
| GMC 对状态的应用 | 不一致，D1 | 修复后用平移、旋转、缩放矩阵直接注入，比较完整状态与协方差 |
| `BoTSortTrack` | 已删除；实际运行时仅保留 `TrackRecord` | 删除完成并通过构建回归 |
| 坐标归一化/像素适配 | 本仓 GStreamer 适配，不属于 Python 算法内核 | 保留 round-trip 与插件写回测试，确保不改变数据契约 |
| `SsvDetection.confidence → Detection.score` | 一致：YOLOv5 已计算 `objectness × class_score`；Nx6/YOLOv8 传入已完成的单一置信度；插件原样赋值 | 增加 parser 到 tracker 的分数语义回归，覆盖 YOLOv5 与 Nx6 输入 |

在上述“已迁移但未接入”项得到删除或独立验证结论前，本阶段不得使用“全部算法原理已审核”的表述。

## 验收标准

1. 新增的确定性对照测试覆盖“必覆盖场景”全部项目；基准结果固化在本仓，不在测试执行时读取外部 `/mnt/work/BoT-SORT`。
2. D1、D2、D3 的修复前用例必须失败，修复后 Python/C++ 的逐帧离散状态和 ID 全等，浮点状态与协方差在声明容差内一致。
3. D4 在可执行环境安装固定版本 Python `lap` 后取得实际对照结论；在此之前，报告必须明确其仅完成源码审查。
4. “完整审核矩阵”中的每个已迁移项均获得“一致”“已修复”或“已删除”的结论；D4 的 LAPJV 运行时对照作为唯一剩余门禁。
5. `./ssv build` 和可执行的 BoT-SORT 内核/插件测试通过。若全量测试被 TensorRT 运行时环境阻断，报告必须将其与 BoT-SORT 结果明确隔离。
6. `SsvDetection` 的下游可见数据契约回归测试通过。

## 非本阶段范围

1. 修复 TensorRT 包中 `libnvinfer.so.11` soname 链接缺失及其导致的 `ssvinfer` 测试加载失败；该问题与本算法修复独立。
2. 根据真实 RTSP 流评估 GMC 生产参数或调整阈值默认值。
3. 增加 ReID、类别约束、多摄像头关联或其他 GMC 方法。
