import os
import shutil
import subprocess
import sys

VSDEVCMD = r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
MSBUILD = r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
PROJ = r"C:\Users\wuyua\Project\Magpie\src\Magpie\Magpie.vcxproj"
SOLUTIONDIR = "C:\\Users\\wuyua\\Project\\Magpie\\"
LOGFILE = r"C:\Users\wuyua\Project\Magpie\.workbuddy\build.log"
OUTDIR = r"C:\Users\wuyua\Project\Magpie\bin\x64\Release"
EFFECTS_SRC = r"C:\Users\wuyua\Project\Magpie\src\Effects"
EFFECTS_DST = os.path.join(OUTDIR, "effects")

# 1) Capture the full VS dev environment from VsDevCmd.bat
print("=== Initializing VS dev environment (VsDevCmd.bat) ===", flush=True)
cmd = f'cmd /c "call \"{VSDEVCMD}\" -arch=amd64 -host_arch=amd64 && set"'
res = subprocess.run(cmd, capture_output=True, text=True, shell=True)
env = os.environ.copy()
for line in res.stdout.splitlines():
    if "=" in line:
        k, v = line.split("=", 1)
        # skip the marker line "Environment initialized"
        if k == "Environment initialized":
            continue
        env[k] = v

# 2) Clear proxy vars (case-insensitive dupes cause MSBuild env dict collision)
for k in ("HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY",
          "https_proxy", "http_proxy", "all_proxy"):
    env.pop(k, None)

# 3) Make sure conan (venv) is on PATH in case MSBuild re-triggers anything
venv = r"C:\Users\wuyua\.workbuddy\binaries\python\envs\default\Scripts"
env["PATH"] = venv + ";" + env.get("PATH", "")

# 4) Confirm cl.exe is reachable
cl = subprocess.run("where cl.exe", capture_output=True, text=True, shell=True, env=env)
print("cl.exe:", cl.stdout.strip() or "(not found)", flush=True)

# 4.5) The 19041 XAML compiler (used to dodge the 26100 WMC9999 crash) emits duplicate
# x:Bind helper templates into the generated XamlTypeInfo.g.cpp -> C2995. The markup
# targets MarkupCompilePass1/Pass2 have NO Inputs/Outputs and run UNCONDITIONALLY every
# build, so the file is always freshly (re)generated. The dedupe is therefore done by an
# injected MSBuild target (dedupe.targets, AfterTargets=MarkupCompilePass1/Pass2, gated to
# the Magpie UI project). It runs immediately AFTER the markup regenerates the file and
# BEFORE ClCompile consumes it, stripping the bad hard-coded-string Header variants.

# 5) Run MSBuild on Magpie.vcxproj
def build_pass():
    args = [
        MSBUILD, PROJ,
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:PlatformToolset=v143",
        "/p:SolutionDir=" + SOLUTIONDIR,
        # 绕过基础版 10.0.26100.0 SDK 的 XamlCompiler(CompileXaml 任务) 已知 bug
        # (WMC9999: Value cannot be null, Parameter name: name)。该 bug 在 VS2022 17.14.10
        # 随打了补丁的 10.0.26100.x SDK 修复；本机无管理员/无法更新 VS，故把 XAML 编译器
        # 重定向到已安装的非缺陷 19041 SDK 的 XamlCompiler 目标(其 Build.Tasks.dll 早于该回归)。
        # 两个 SDK 的 Microsoft.UI.Xaml.Markup.winmd 字节一致，类型解析等价。
        "/p:WindowsKitsXamlCompilerTargetsPath=C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.19041.0\\XamlCompiler\\Microsoft.Windows.UI.Xaml.Common.targets",
        # 19041 编译器会为部分 x:Bind 辅助函数(如 Get/SetReferenceTypeMember_Header)生成两份
        # 同名但实现不同的定义(string 硬编码版 + 通用版)，导致 C2995。通过注入的 MSBuild
        # 目标(dedupe.targets, AfterTargets=MarkupCompilePass1/Pass2)在标记编译生成文件后、
        # ClCompile 消费前去除冗余的硬编码 string 版，保留通用版。不修改任何工程文件。
        "/p:CustomAfterMicrosoftCommonTargets=C:\\Users\\wuyua\\Project\\Magpie\\.workbuddy\\dedupe.targets",
        # 分进程编译：Magpie UI 工程的 WinUI PCH 极大(约 1.3GB)，若让单个 cl.exe 一次性
        # 编译全部 ~45 个源文件，编译器前端/PCH 堆会持续累积，约到第 35 个文件时撞上内部堆
        # 上限而报 C3859/C1076。解决方法是把源文件拆到多个 cl.exe 进程(MultiToolTask)，每个
        # 进程拿到全新堆，PCH 则是只读内存映射文件、跨进程共享(不翻倍占用)。MultiProcMaxCount=3
        # 即每个进程约 15 个文件，远低于 ~35 的累积阈值；PCH 共享使总内存极小。仍保留 /m:1 关闭
        # MSBuild 多工程节点并行(Magpie.Core 与 Magpie 顺序构建)。
        "/p:UseMultiToolTask=true",
        "/p:MultiProcMaxCount=3",
        "/v:minimal", "/m:1", "/nologo",
        "/fl", f"/flp:logfile={LOGFILE};verbosity=minimal",
    ]
    print("=== Running MSBuild ===", flush=True)
    print(" ".join(args), flush=True)
    return subprocess.run(args, env=env).returncode

rc = build_pass()

# 6) 部署效果文件(effects)。
#    关键点：Magpie.vcxproj 只 ProjectReference 了 Magpie.Core.vcxproj，并没有引用
#    src\Effects\Effects.vcxproj（后者是 Utility 工程，唯一职责就是把 hlsl/hlsli/dds
#    复制到 $(OutDir)\effects）。所以单独构建 Magpie.vcxproj 时效果文件不会被部署。
#    而 EffectsService::Initialize -> ListEffects 会从相对路径 "effects" 扫描 *.hlsl
#    (CommonSharedConstants::EFFECTS_DIR = L"effects"，相对进程工作目录)。目录不存在时
#    FindFirstFileEx 失败、效果表为空，于是 ScalingService::_StartScaleImpl 里
#    EffectsService::Get().GetEffect(effect.name) 返回 nullptr，最终报
#    ScalingError::InvalidScalingMode -> 界面弹出“缩放模式无效。”
#    因此这里在构建后显式同步 src\Effects 到输出目录，缺一不可。
def deploy_effects():
    if not os.path.isdir(EFFECTS_SRC):
        print(f"[effects] 源目录不存在，跳过: {EFFECTS_SRC}", flush=True)
        return False

    def _ignore(_dir, names):
        return [n for n in names
                if os.path.splitext(n)[1].lower() in (".vcxproj", ".filters", ".user")
                or n.lower() in ("obj", "bin", ".vs")]

    # 用覆盖式复制（dirs_exist_ok）而不是先 rmtree 再 copytree：
    # 删除上百个效果文件会触发环境的批量删除保护（阈值 50），导致构建被中断。
    try:
        shutil.copytree(EFFECTS_SRC, EFFECTS_DST, ignore=_ignore, dirs_exist_ok=True)
    except OSError as e:
        print(f"[effects] 部署失败: {e}", flush=True)
        return False

    n_hlsl = sum(len([f for f in fs if f.lower().endswith(".hlsl")])
                 for _, _, fs in os.walk(EFFECTS_DST))
    print(f"[effects] 已部署 {n_hlsl} 个 .hlsl -> {EFFECTS_DST}", flush=True)
    return True


deploy_effects()

print(f"MSBUILD_EXIT={rc}", flush=True)
sys.exit(rc)
