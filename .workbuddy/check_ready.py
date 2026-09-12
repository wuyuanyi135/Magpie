import os, glob, subprocess

print("== SDK 26100 ==")
sdk_include = r"C:/Program Files (x86)/Windows Kits/10/Include"
found = False
if os.path.isdir(sdk_include):
    for d in os.listdir(sdk_include):
        if "26100" in d:
            print("  OK", d)
            found = True
if not found:
    print("  NO 26100 SDK. available:", [d for d in os.listdir(sdk_include)] if os.path.isdir(sdk_include) else "none")

print("== cl.exe (MSVC) ==")
cl_paths = []
# search VS 2022 install for cl.exe in Hostx64/x64
vs = r"C:/Program Files/Microsoft Visual Studio/2022"
if os.path.isdir(vs):
    for root, dirs, files in os.walk(vs):
        # prune deep unrelated
        dirs[:] = [d for d in dirs if d not in ('Common7','Connected Services')]
        for f in files:
            if f.lower() == "cl.exe":
                rel = root
                cl_paths.append(os.path.join(root, f))
print("  count:", len(cl_paths))
for p in cl_paths[:10]:
    print("  ", p)

print("== CMake ==")
for cand in [
    r"C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
    r"C:/Program Files/CMake/bin/cmake.exe",
]:
    print("  ", "OK" if os.path.exists(cand) else "NO", cand)

print("== Conan ==")
conan = r"C:/Users/wuyua/.workbuddy/binaries/python/envs/default/Scripts/conan.exe"
print("  ", "OK" if os.path.exists(conan) else "NO", conan)

print("== MSBuild ==")
msb = r"C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
print("  ", "OK" if os.path.exists(msb) else "NO", msb)

print("== admin ==")
try:
    open(r"C:/Windows/system32_test_write.tmp","w").close()
    os.remove(r"C:/Windows/system32_test_write.tmp")
    print("  admin: YES")
except Exception as e:
    print("  admin: NO", e)
