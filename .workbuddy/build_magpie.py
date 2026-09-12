import os, subprocess, sys, datetime

VS = r"C:/Program Files/Microsoft Visual Studio/2022/Community"
vsdevcmd = os.path.join(VS, r"Common7\Tools\VsDevCmd.bat")
msbuild = os.path.join(VS, r"MSBuild\Current\Bin\MSBuild.exe")
conan = r"C:/Users/wuyua/.workbuddy/binaries/python/envs/default/Scripts/conan.exe"
vcxproj = r"C:/Users/wuyua/Project/Magpie/src/Magpie/Magpie.vcxproj"
log = r"C:/Users/wuyua/Project/Magpie/.workbuddy/build.log"

assert os.path.exists(vsdevcmd), vsdevcmd
assert os.path.exists(msbuild), msbuild
assert os.path.exists(conan), conan
assert os.path.exists(vcxproj), vcxproj

# cmd /c: initialize VS env then run msbuild, output to log
inner = (
    f'call "{vsdevcmd}" -arch=amd64 -host_arch=amd64 && '
    f'set "PATH={os.path.dirname(conan)};%PATH%" && '
    f'"{msbuild}" "{vcxproj}" '
    f'/p:Configuration=Release /p:Platform=x64 '
    f'/v:minimal /m /nologo '
    f'> "{log}" 2>&1'
)
cmd = ["cmd.exe", "/c", inner]

print("== build start", datetime.datetime.now(), "==")
print("vsdevcmd:", vsdevcmd)
print("msbuild:", msbuild)
print("conan:", conan)
print("cmd:", " ".join(cmd))

env = dict(os.environ)
r = subprocess.run(cmd, env=env)
print("== build exit code:", r.returncode, "==")
print("log bytes:", os.path.getsize(log) if os.path.exists(log) else -1)
# print tail
if os.path.exists(log):
    with open(log, "r", errors="replace") as f:
        data = f.read()
    tail = data[-4000:]
    print("== LOG TAIL ==")
    print(tail)
