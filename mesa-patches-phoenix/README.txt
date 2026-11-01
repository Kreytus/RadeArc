Phoenix (Radeon 780M, gfx1103) patches on top of Cobalt64's RADV Darwin port.

Base: Mesa f5cb8ee032adabef599ae892ec4e42d796b84da9 + reference/Cobalt64/mesa-patches/0001..0009.
Apply these after them (git am, 0001..0007).  0001..0003 are what tools/mesa_phoenix_canned.py,
tools/mesa_phoenix_identity.py and tools/mesa_phoenix_service.py produce (0003: open the PhxBringup service).
0004 answers AMDGPU_INFO_TIMESTAMP from SMUIO GOLDEN_TSC (needs PhxBringup build 31): Metal GPUStartTime/GPUEndTime.
0005 opens PhxBringup through NMXAccel when sandboxed (needs the PhxAccel proxy): Metal in WebKit's GPU process.
0006 unrolls nested loops without a full optimization round per level: a new CoreAnimation compositor shader
     variant compiles in ~1 s instead of ~2 s (same code), so WindowServer stalls less on first use.
0007 adds the Phoenix 2 (Radeon 740M/760M, GC 11.0.4) canned table and picks the table from the kext's GC version
     (11.0.1 phoenix, 11.0.4 phoenix2) instead of the PCI id; RADV_DARWIN_MOCK_GC lets the mock pretend to be either.

Build on the Mac (Tahoe 26.6.2, CLT 16.2, Python 3.14 venv with meson/ninja/mako/pyyaml/packaging,
glslangValidator built from source into ~/gpu/prefix):
  meson setup build --buildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
    -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
    -Dgles2=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
    -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dspirv-tools=disabled
  ninja -C build

Check (no GPU, in-process mock kext):
  VK_ICD_FILENAMES=build/src/amd/vulkan/radeon_devenv_icd.x86_64.json RADV_DARWIN_MOCK=1 tools/vkprobe/vkprobe
  -> "AMD Radeon 780M Graphics (RADV PHOENIX) 1002:1900", device created, compute pipeline compiled (ACO gfx1103).
  RADV_DARWIN_MOCK_GC=0xb0004 -> "... (RADV PHOENIX2) 1002:15c8". CI does both (.github/workflows/build-mesa.yml).
