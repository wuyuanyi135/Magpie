# 「后台保持渲染」功能实现计划

## 范围说明
- 本次只实现之前讨论的「后台保持渲染」功能；以下两项已列入待办清单，**不在本次范围**：
  1. Alt+Tab 切回后光标死锁修复（CursorManager.cpp:647-690 早退分支）；
  2. 「其他置顶窗口覆盖缩放画面」的取消置顶选项。
- 两个未确认的设计取舍按推荐默认执行：画面可见期间**保持置顶**不变；收起时源窗口**移到屏幕外**（保持未最小化）。

## 功能行为
开启模式后（设置开关 / 快捷键切换 + toast / 工具栏按钮）：
- 工具栏最小化按钮变为「最小化到后台」：隐藏缩放画面 + 把源窗口移到屏幕外（不最小化）→ 游戏继续渲染、声音继续；后台期间 Magpie 跳过自身捕获/特效/Present 省 GPU。
- 点击任务栏图标或 Alt+Tab 切回该程序（源窗口被激活）→ 自动移回并恢复缩放画面。
- 若收起状态下源窗口被真·最小化（如 Win+D、点击前台程序任务栏图标），走现有「等待-重启」路径；所有停止路径都会先把源窗口移回原位。
- 画面可见期间置顶行为不变；3D 游戏模式在后台期间跳过前台重叠检测。

## 实现步骤
1. **选项**：`AppSettings` 增 `_isBackgroundRendering`（默认 false，JSON `"backgroundRendering"`）；`ScalingFlags::BackgroundRendering` 位 + accessor；`ScalingService::_StartScaleImpl` 传入。
2. **核心（ScalingWindow）**：新增 `_isSrcParked`、`_srcRectBeforePark`、`SetBackgroundRendering(bool)`、`ParkToBackground()`、`RestoreFromBackground()`；`Render()` 循环在后台态只做轻量状态检测（源窗口销毁→停止；**获得焦点→恢复**；被最小化→现有暂停路径），跳过 3D 检测、`_UpdateRendererRect`、光标更新与渲染；收起时恢复系统光标（走 CursorManager 公共接口，不碰未提交 WIP）；所有停止路径先移回源窗口。
3. **快捷键**：`ShortcutAction::BackgroundRendering`（默认无按键）；AppSettings 默认值与 JSON 读写；`ShortcutHelper` 两个函数加 case（顺手补缺失的 TakeScreenshot 分支）；`ScalingService::_ShortcutService_ShortcutPressed` 加分支：更新 AppSettings + 对所有活跃 runtime TryEnqueue 切换 + toast。
4. **工具栏（OverlayDrawer）**：左侧 toggle 组加「后台保持渲染」按钮（高亮反映状态，TryEnqueue 切换 + ShowToast）；右侧最小化按钮模式开时改为「最小化到后台」且不再要求源窗口有 WS_MINIMIZEBOX。图标实现时从 Segoe 字体验证选定（新码点需同步 ICON_RANGES + FONTS_CACHE_VERSION）。
5. **Toast 回退**：ToastPage 锚点矩形在屏幕外/最小化时回退主屏工作区底部居中。
6. **设置界面 + 本地化**：HomePage 设置卡片与快捷键卡片 + HomeViewModel 属性；resw 新增约 8 个 key（en-US、zh-Hans 必加）。

## 验证
- msbuild 编译 x64；启动冒烟（设置持久化、快捷键注册日志、工具栏按钮）。
- 手动测试清单随交付附上：收起后声音/进度持续、任务栏与 Alt+Tab 恢复、Win+D 行为、各停止路径后源窗口位置正确、全屏/窗口模式、多窗口。
- 已知限制：游戏失焦自行暂停/静音无法避免；Magpie 崩溃极小概率留下源窗口在屏幕外（任务栏右键「移动/最大化」可找回）。