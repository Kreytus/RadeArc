//
//  PhxBringup.cpp — Phoenix (Radeon 780M, 1002:1900) and Phoenix 2 (Radeon 740M, 1002:15C8) bring-up kext, step E1.
//  The chip is told from its IP discovery table (identifyChip, rdna3_chip_profile), not from the PCI id.
//
//  Publishes the service the RADV Darwin backend opens (class PhxBringup, user client type 'N48N')
//  and answers ABI v1 selectors 0..8 with the same rules as Mesa's in-process mock kext
//  (src/amd/common/darwin/ac_darwin_mock.c): buffers are kernel/user shared system memory,
//  GPU VA mappings are bookkeeping only, and Submit retires at once without touching the GPU.
//  E1 proves the IOKit transport end to end; E3/E4 put the GPU behind the same calls.
//
//  Boot-arg -phxbringupoff disables it; -phxoff disables it and the rest of the driver (PhxAccel, phxboot). ReadRegs (MMIO read) is root-only; nothing else touches hardware.
//

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>

#include "PhxNativeABI.h"
#include "rdna3_chip_profile.h"
#include "phx_dcn_i2c.h"

#define PHX_KEXT_BUILD 42u

// debug.phxbos_flush: how a client's BOs are made coherent around each submission and each completed wait.
// 2 = clflush every line (the bring-up way), 1 = clflushopt (the same lines, pipelined), 0 = none (the GPU maps system
// memory snooped, kPteSysFlags). With clflush, WindowServer spent ~73% of its CPU here and composited ~13 fps; with
// none, 41 fps and no artifacts (2026-10-20), hence the default.
static int gBosFlush = 0;
static volatile SInt64 gBosFlushBytes;
SYSCTL_INT(_debug, OID_AUTO, phxbos_flush, CTLFLAG_RW | CTLFLAG_LOCKED, &gBosFlush, 0,
           "BO cache maintenance per submit/wait: 0 none, 1 clflushopt, 2 clflush");
SYSCTL_QUAD(_debug, OID_AUTO, phxbos_flush_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gBosFlushBytes,
            "bytes of BOs flushed from the CPU caches");
// debug.phxbo_*: why BO creation or mapping was refused, and the most handles/mappings one client has held, so a
// table that fills up shows here instead of only as "Failed to allocate MTLTexture" in WindowServer.
static volatile UInt32 gBoHandlesFull, gBoMapsFull, gBoNoMem, gBoPeakHandles, gBoPeakMaps;
SYSCTL_UINT(_debug, OID_AUTO, phxbo_handles_full, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gBoHandlesFull, 0,
            "BO creations refused: all handles in use");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_maps_full, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gBoMapsFull, 0,
            "GPU VA mappings refused: mapping table full");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_nomem, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gBoNoMem, 0,
            "BO creations refused: pool budget or memory");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_peak_handles, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gBoPeakHandles, 0,
            "most BO handles one client has held");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_peak_maps, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gBoPeakMaps, 0,
            "most GPU VA mappings one client has held");
static unsigned int gHostCapMb = 8192;
static volatile UInt32 gHostNoMem, gHostWireFail, gVmMapFail, gHostMbNow, gHostMbPeak;
static volatile SInt64 gHostBytes;
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_cap_mb, CTLFLAG_RW | CTLFLAG_LOCKED, &gHostCapMb, 0,
            "host-import budget per client, MiB");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_nomem, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostNoMem, 0,
            "host imports refused: over the per-client budget");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_wire_fail, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostWireFail, 0,
            "host imports refused: pages could not be wired");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_vmmap_fail, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gVmMapFail, 0,
            "GPU VA mappings that failed in the page tables");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_mb, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostMbNow, 0,
            "host-imported MiB now, all clients");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_mb_peak, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostMbPeak, 0,
            "most host-imported MiB at once, all clients");
// debug.phxbo_host_ro (default 0): the read-only fallback of build 40. With it WindowServer's GPU hung within a minute
// (watchdog: nvmtl.execution spinning in the kext's fence wait, 2026-11-02): those pages are not only read by the GPU.
// Off, a read-only IOSurface import fails as in build 39 and Metal copies it instead, which worked.
static unsigned int gHostRo = 0;
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_ro, CTLFLAG_RW | CTLFLAG_LOCKED, &gHostRo, 0,
            "1 = wire read-only host imports for device reads (experimental, hung the GPU)");
static volatile UInt32 gHostReadOnly, gHostWireErr, gHostWireErrRo;
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_readonly, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostReadOnly, 0,
            "host imports wired read-only (the caller's pages are read-only)");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_wire_err, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostWireErr, 0,
            "IOReturn of the last read-write wiring that failed");
SYSCTL_UINT(_debug, OID_AUTO, phxbo_host_wire_err_ro, CTLFLAG_RD | CTLFLAG_LOCKED, (unsigned int *)&gHostWireErrRo, 0,
            "IOReturn of the last read-only wiring that failed");
static void notePeak(volatile UInt32 *peak, UInt32 v);
static void hostBytes(SInt64 delta) {
    const SInt64 now = OSAddAtomic64(delta, &gHostBytes) + delta;
    gHostMbNow = (UInt32)(now > 0 ? now >> 20 : 0);
    notePeak(&gHostMbPeak, gHostMbNow);
}
static void notePeak(volatile UInt32 *peak, UInt32 v) {
    UInt32 old = *peak;
    while (v > old && !OSCompareAndSwap(old, v, peak))
        old = *peak;
}
static volatile UInt32 gSysctlUp;

// psp_gfx_rb_frame (64 B) — the ring entry the PSP reads. Only 5 fields are set; the rest stay zero
// (Linux amdgpu_psp.c:3485: a non-zero cmd_buf_size makes the PSP silently drop the frame).
struct PhxGfxRbFrame {
    uint32_t cmd_buf_addr_lo, cmd_buf_addr_hi, cmd_buf_size;
    uint32_t fence_addr_lo, fence_addr_hi, fence_value;
    uint32_t sid_lo, sid_hi;
    uint8_t vmid, frame_type, reserved1[2];
    uint32_t reserved2[7];
} __attribute__((packed));
static_assert(sizeof(PhxGfxRbFrame) == 64, "PhxGfxRbFrame must be 64 B");

// GB_ADDR_CONFIG RADV is told before the GPU runs (once it runs, the register is read: Linux gfx_v11_0_gpu_early_init
// reads it too). Phoenix: Mesa amdgpu_devices.c canned "phoenix" table (upstream's constant). Phoenix 2: measured on the
// Ryzen 5 8500G (RADV GB_ADDR_CONFIG, hardware/dumps/b650m_8500g/radv-info.txt).
static uint32_t gbAddrConfigBeforeBringup(rdna3::PhxChip c) { return c == rdna3::PhxChip::Phoenix1 ? 0x242u : 0x142u; }

// Pools reported to RADV. E1 backs every BO with system memory, so these are budgets, not hardware:
// BAR0 is 256 MiB on this machine, the UMA carve-out 6 GiB.
static constexpr uint64_t kVisTotal = 256ull << 20;
static constexpr uint64_t kHiTotal = 1536ull << 20;
static constexpr uint64_t kGttCap = 512ull << 20;
static constexpr uint64_t kGttMaxBo = 256ull << 20;
static constexpr uint64_t kHostMaxBo = 1024ull << 20;   // one host import
// host imports per client: debug.phxbo_host_cap_mb (default 8 GiB). WindowServer imports its IOSurfaces this way; the
// fixed 4 GiB of builds <= 38 was the likely wall behind its "Failed to allocate MTLTexture" floods (2026-11-01).
static constexpr uint64_t kVaMask = (1ull << 48) - 1;
// GPU VA mappings per client. WindowServer on the 740M ran out of the earlier 4096 (with the 4095 BO handles): every
// MTLTexture allocation failed, 64x64 included, the desktop flickered and WindowServer crashed (2026-11-01).
static constexpr int kMaxMaps = 16384;

static bool vaCanonical(uint64_t va) {
    uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}

class PhxBringup : public IOService {
    OSDeclareDefaultStructors(PhxBringup)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *whatDevice) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;

    bool claimSession();
    void releaseSession();
    IOReturn readRegs(uint32_t first, uint32_t count, uint32_t *out);
    IOReturn vramRead(uint32_t byteOff, uint32_t count, uint32_t *out);
    IOReturn writeReg(uint32_t reg, uint32_t value, uint32_t *readback);
    // The monitor's EDID over DDC1 (phx_dcn_i2c.h). Thread context only (polls with IODelay, every wait bounded).
    IOReturn readEdid(uint8_t out[256], uint32_t *len, uint32_t *result);
    IOReturn vramWrite(uint32_t byteOff, uint32_t value);
    IOReturn doorbellWrite(uint64_t byteOff, uint64_t value);
    IOReturn sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1);
    IOReturn smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter);
    IOReturn pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]);
    IOReturn pspSubmit(uint32_t cmdId, uint64_t out[4]);
    IOReturn fwStage(uint32_t dstOffset, const void *bytes, uint32_t len);
    IOReturn loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]);
    IOReturn pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]);
    IOReturn pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]);
    bool debugEnabled() const { return debug; }
    // The chip, from the IP discovery table (rdna3_chip_profile): nullptr = unknown, nothing but reads is allowed.
    const rdna3::ChipProfile *chipProfile() const { return chip; }
    // Commands that write the GPU (PSP, SMU, registers, VRAM, doorbells, queues, the claim) are refused unless the chip
    // is known. Phoenix 2 bring-up (phxfw firmware plan, TOC/TMR as Linux, mesboot layout from the carve-out) has not run
    // on hardware yet, so it is opt-in: the boot-arg -phxp2 enables it, without it Phoenix 2 stays read-only.
    bool hwWritesAllowed() const {
        return chip && (chip->chip == rdna3::PhxChip::Phoenix1 || (chip->chip == rdna3::PhxChip::Phoenix2 && p2OptIn));
    }
    uint32_t gbAddrConfig() const;
    uint32_t gcVersion() const {
        return chip ? (uint32_t(chip->gc.major) << 16) | (uint32_t(chip->gc.minor) << 8) | chip->gc.revision : 0;
    }
    // E8: GPU execution. The userspace bring-up maps MES queues (compute = 0, gfx = 1) and hands them over (adoptQueue);
    // user BOs are then mapped into the VMID 8 page table and SUBMIT writes INDIRECT_BUFFERs into the queue's ring.
    IOReturn adoptQueue(const uint64_t in[6]);
    IOReturn claimAccel();
    bool gpuReady() const { return gq[0].ready || gq[1].ready; }
    // one GPUVM context (VMID 8..15) per client process: RADV instances allocate VAs independently
    uint32_t vmidAlloc();
    void vmidFree(uint32_t vmid, uint64_t lastSeq);
    IOReturn vmMap(uint32_t vmid, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size, bool writable = true);
    IOReturn vmUnmap(uint32_t vmid, uint64_t va, uint64_t size);
    IOReturn gpuSubmit(uint32_t vmid, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seq);
    uint64_t gpuRetired() const;

private:
    uint32_t rreg(uint32_t reg) const;
    void wreg(uint32_t reg, uint32_t value) const;
    uint32_t smuPoll() const;
    bool pspWaitTos(uint32_t timeoutMs) const;
    IOReturn pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]);
    void hdpFlush() const;
    void readFbLayout();
    void identifyChip();
    void clearRing();

    IOPCIDevice *pci{nullptr};
    IOMemoryMap *mmio{nullptr};
    const rdna3::ChipProfile *chip{nullptr};
    bool pmJoined{false};   // PMinit/joinPMtree done (Phoenix 2): the PCI device is held on
    rdna3::ChipRegs cr{};   // valid when chip is set
    IOLock *smuLock{nullptr};
    // UMA carve-out ("VRAM"): GPU MC address range and the system physical address behind it.
    uint64_t fbMcBase{0}, fbSize{0}, fbPhys{0};
    // BAR0 framebuffer aperture: the CPU window into the first fbAperLen bytes of VRAM. It is an MMIO
    // region above system RAM (phys 0x8_2040_0000 here, 256 MiB), so CPU writes through it reach the
    // GPU's local memory and never the macOS heap. This is how the PSP ring is initialised.
    IOMemoryMap *fbAper{nullptr};
    IOMemoryMap *doorbellMap{nullptr}; // BAR2: doorbell aperture (2 MiB)
    IOBufferMemoryDescriptor *sysBuf{nullptr}; // debug: physically contiguous system memory for GPU VM experiments
    uint64_t fbAperPhys{0}, fbAperLen{0};
    // MMIO dword to write 0 to for an HDP flush (0 if the NBIO remap register was implausible).
    uint32_t hdpFlushDword{0};
    // PSP KM ring (debug selector 101): MC address once created, else 0. The GPCOM command + fence buffers
    // (selector 103) live just above the ring in VRAM; offsets are VRAM byte offsets, Mc the matching MC address.
    uint64_t ringMc{0}, ringOff{0};
    uint64_t cmdOff{0}, cmdMc{0}, fenceOff{0}, fenceMc{0};
    uint32_t fenceCounter{0};
    // Firmware bring-up regions (set when the ring is created): TMR the PSP copies firmware into, and fw_buf
    // where the CPU stages ucode for LOAD_IP_FW. Offsets are VRAM byte offsets, Mc the matching MC addresses.
    uint64_t tmrOff{0}, tmrMc{0}, tmrSize{0};
    uint64_t fwBufOff{0}, fwBufMc{0}, fwBufSize{0};
    bool tmrSetUp{false};
    // E8 state: the adopted queues (ring/wb/fence are VRAM byte offsets reached through BAR0) and the VMID 8
    // page-table pool (physically contiguous system memory; page 0 is the root). Sequence numbers are global across
    // the queues; each queue's fence holds the last sequence it finished, and fseq/fend keep its outstanding
    // submissions in order (sequence, ring position after it).
    struct GpuQueue {
        bool ready, gfx;
        uint32_t ringOff, ringDw, wbOff, fenceOff;
        uint64_t dbByte, wptr, doneEnd;
        uint64_t fseq[64], fend[64];
        volatile uint32_t head, tail;
    };
    GpuQueue gq[2]{};
    uint64_t gSeq{0};
    uint64_t queueFence(const GpuQueue &q) const;
    IOLock *gpuLock{nullptr};
    // One GPUVM context (page tables) per client, and the eight hardware VMIDs lent to contexts at submission time,
    // as amdgpu does: a fixed VMID per client capped Metal at eight processes, and with every app allowed Metal the
    // ninth got no device (2026-10-20). A context's handle is its index + 1.
    // A context's page tables, in physically contiguous 64 KiB chunks allocated as it maps (most contexts need one or
    // two): a single 4 MiB contiguous pool per context failed once memory was fragmented. Table i is page i % 16 of
    // chunk i / 16; table 0 is the root.
    static constexpr uint32_t kPtChunkBytes = 64u << 10, kPtChunkPages = kPtChunkBytes / 4096, kPtChunks = 64;
    struct VmCtx {
        IOBufferMemoryDescriptor *chunk[kPtChunks];
        uint64_t chunkPhys[kPtChunks];
        uint64_t phys;                    // the root table
        uint32_t pages, used;
        bool busy;
        uint32_t hw;                      // the hardware VMID pointed at these tables, 0 = none
    };
    static constexpr uint32_t kMaxCtx = 96;
    VmCtx vm[kMaxCtx]{};
    struct HwVmid {
        uint32_t ctx;                     // handle of the context it points at, 0 = free
        uint64_t lastSeq, lastUse;        // its newest submission; when it was last bound or used
    };
    HwVmid hwv[8]{};
    uint64_t hwTick{0};
    uint32_t vmBind(uint32_t handle);
    VmCtx *vmCtx(uint32_t handle) { return handle && handle <= kMaxCtx && vm[handle - 1].busy ? &vm[handle - 1] : nullptr; }
    uint64_t *ptTable(const VmCtx &c, uint64_t phys) const;
    uint64_t ptTablePhys(VmCtx &c, uint32_t i);
    uint64_t *vmPte(VmCtx &c, uint64_t va, bool alloc);
    void vmFlushTlb(uint32_t vmid) const;
    void ringPut(GpuQueue &q, uint32_t idx, uint32_t value);
    bool debug{false};
    bool p2OptIn{false};   // boot-arg -phxp2
    bool smuUsed{false};
    volatile SInt32 sessionOpen{0};
};

OSDefineMetaClassAndStructors(PhxBringup, IOService)

class IOAccelPhxNativeClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelPhxNativeClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
    struct Bo {
        IOMemoryDescriptor *md;   // an IOBufferMemoryDescriptor, or the caller's own pages (host, prepared)
        uint64_t size;
        uint32_t domain;
        uint32_t bits;
        uint8_t *kva;             // the kernel's view of the BO; NULL for host imports, which the kernel never reads
        bool host;
        bool readOnly;            // a host import wired for device reads only (the caller maps those pages read-only)
    };
    struct Map {
        bool used;
        uint32_t handle;
        uint64_t va, size, off;
        uint32_t flags;
    };

    IOReturn doHello(IOExternalMethodArguments *a);
    IOReturn doQueryInfo(IOExternalMethodArguments *a);
    IOReturn doReadRegs(IOExternalMethodArguments *a);
    IOReturn doBoCreate(IOExternalMethodArguments *a);
    IOReturn doBoFree(IOExternalMethodArguments *a);
    IOReturn doBoImportHost(IOExternalMethodArguments *a);
    void boRelease(Bo &b);
    IOReturn doGemVa(IOExternalMethodArguments *a);
    IOReturn doCtx(IOExternalMethodArguments *a);
    IOReturn doSubmit(IOExternalMethodArguments *a);
    IOReturn doWaitSeq(IOExternalMethodArguments *a);
    IOReturn doDbgSmu(IOExternalMethodArguments *a);
    IOReturn doDbgPspRing(IOExternalMethodArguments *a);
    IOReturn doDbgVramRead(IOExternalMethodArguments *a);
    IOReturn doDbgPspSubmit(IOExternalMethodArguments *a);
    IOReturn doDbgFwStage(IOExternalMethodArguments *a);
    IOReturn doDbgLoadIpFw(IOExternalMethodArguments *a);
    IOReturn doDbgPspCtl(IOExternalMethodArguments *a);
    IOReturn doDbgPspRaw(IOExternalMethodArguments *a);
    IOReturn doDbgWriteReg(IOExternalMethodArguments *a);
    IOReturn doDbgVramWrite(IOExternalMethodArguments *a);
    IOReturn doDbgDoorbell(IOExternalMethodArguments *a);
    IOReturn doDbgSysMem(IOExternalMethodArguments *a);
    IOReturn doDbgAdoptQueue(IOExternalMethodArguments *a);
    IOReturn doDbgClaim(IOExternalMethodArguments *a);
    IOReturn doDbgEdid(IOExternalMethodArguments *a);
    void flushBos();
    Bo *findBo(uint32_t h);
    bool ibCovered(uint64_t vaCanon, uint64_t bytes);
    void freeAll();

    PhxBringup *owner{nullptr};
    IOLock *lock{nullptr};
    bool privileged{false};
    bool hello{false};
    bool sessionClaimed{false};
    uint32_t vmid{0};        // this client's GPUVM context, taken on its first GPU mapping or submission
    uint64_t lastSeq{0};     // its newest submission: the context is released only once the GPU is past it
    bool ensureVmid() { return vmid || (vmid = owner->vmidAlloc()) != 0; }
    Bo *bos{nullptr};      // PHXN_MAX_BOS entries, index = handle
    Map *maps{nullptr};    // kMaxMaps entries
    bool ctx[PHXN_MAX_CTX + 1]{};
    uint64_t visUsed{0}, hiUsed{0}, gttUsed{0}, hostUsed{0};
    uint32_t handlesUsed{0}, mapsUsed{0}, handleHint{1};
    // a free BO handle (0 = none), searched from where the last one was found, so handing out thousands stays linear
    uint32_t takeHandle() {
        for (uint32_t n = 1; n < PHXN_MAX_BOS; n++) {
            const uint32_t h = 1 + (handleHint - 1 + n - 1) % (PHXN_MAX_BOS - 1);
            if (!bos[h].md) { handleHint = h + 1 >= PHXN_MAX_BOS ? 1 : h + 1; return h; }
        }
        return 0;
    }
    void handleTaken() { notePeak(&gBoPeakHandles, ++handlesUsed); }
    task_t task{nullptr};
    uint64_t emitted{0};
    uint32_t pendFenceHandle{0}, pendFenceOffset{0};
    uint64_t pendFenceSeq{0};
};

OSDefineMetaClassAndStructors(IOAccelPhxNativeClient, IOUserClient)

// ---- service ---------------------------------------------------------------------------------------------

bool PhxBringup::start(IOService *provider) {
    int dummy = 0;
    // -phxoff turns everything off (PhxBringup, PhxAMDFB, NMXAccel, the phxboot daemon); -phxbringupoff only this kext
    if (PE_parse_boot_argn("-phxbringupoff", &dummy, sizeof(dummy)) || PE_parse_boot_argn("-phxoff", &dummy, sizeof(dummy)))
        return false;
    if (!IOService::start(provider))
        return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;
    pci->retain();
    {   // the GPU must be a bus master before it can DMA to/from system memory (page tables, user BOs)
        uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
        setProperty("phx-pci-cmd-before", cmd, 16);
        pci->setBusMasterEnable(true);
        pci->setMemoryEnable(true);
        setProperty("phx-pci-cmd-after", pci->configRead16(kIOPCIConfigCommand), 16);
    }
    mmio = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    fbAper = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fbAper) {
        fbAperLen = fbAper->getLength();
        fbAperPhys = fbAper->getPhysicalAddress();
    }
    doorbellMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    smuLock = IOLockAlloc();
    gpuLock = IOLockAlloc();
    uint32_t dbg = 0;
    debug = PE_parse_boot_argn("phxdebug", &dbg, sizeof(dbg)) && dbg == 1;
    p2OptIn = PE_parse_boot_argn("-phxp2", &dummy, sizeof(dummy));
    if (mmio)
        identifyChip();
    // Power management: hold the GPU's PCI device on while this driver runs it. Without a power child, IOPCIFamily may
    // move the device to a low-power state; in an M2 (PhxAMDFB) boot the framebuffer that normally holds it is not
    // there, and the 8500G panicked 89 s after power-on with a CPU stuck in IOPCIBridge::saveDeviceState's config read
    // (setDevicePowerState) while phxboot was bringing the GPU up (Kernel-2026-11-01-122248.panic). Phoenix 2 only, as
    // that is where it was seen; the Phoenix 1 path stays as it was.
    if (chip && chip->chip == rdna3::PhxChip::Phoenix2) {
        static IOPMPowerState kPower[2] = {
            { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
        };
        PMinit();
        provider->joinPMtree(this);
        pmJoined = registerPowerDriver(this, kPower, 2) == kIOReturnSuccess;
        if (pmJoined)
            changePowerStateTo(1);
        setProperty("phx-pm-hold", pmJoined);
    }
    if (chip) {
        readFbLayout();
        // HDP flush window: the NBIO remap register holds the BAR5 byte offset where writing 0 flushes the
        // host data path. Accept it only if it is a plausible dword offset inside our MMIO aperture.
        uint32_t remap = rreg(cr.nbio_remap_hdp_mem_flush_cntl);
        if (remap && remap != 0xFFFFFFFFu && (remap & 3) == 0 && remap + 4 <= mmio->getLength())
            hdpFlushDword = remap >> 2;
        setProperty("phx-hdp-flush-dword", hdpFlushDword, 32);
    }
    setProperty("phx-build", PHX_KEXT_BUILD, 32);
    setProperty("phx-stage", "E3");
    setProperty("phx-debug", debug);
    setProperty("phx-fb-aper-phys", fbAperPhys, 64);
    setProperty("phx-fb-aper-len", fbAperLen, 64);
    IOLog("PhxBringup: service up (mmio %s, bar0 %s %llu MiB, debug %d, chip %s%s)\n", mmio ? "mapped" : "unmapped",
          fbAper ? "mapped" : "unmapped", (unsigned long long)(fbAperLen >> 20), debug, chip ? chip->name : "unknown",
          chip && !hwWritesAllowed() ? ", read-only (boot-arg -phxp2 enables Phoenix 2 bring-up)" : "");
    registerService();
    return true;
}

// The power states only express "the PCI device must be on"; nothing in the GPU is switched here.
IOReturn PhxBringup::setPowerState(unsigned long, IOService *) {
    return kIOPMAckImplied;
}

void PhxBringup::stop(IOService *provider) {
    if (pmJoined) {
        PMstop();
        pmJoined = false;
    }
    if (gSysctlUp) {
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush);
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_unregister_oid(&sysctl__debug_phxbo_handles_full);
        sysctl_unregister_oid(&sysctl__debug_phxbo_maps_full);
        sysctl_unregister_oid(&sysctl__debug_phxbo_nomem);
        sysctl_unregister_oid(&sysctl__debug_phxbo_peak_handles);
        sysctl_unregister_oid(&sysctl__debug_phxbo_peak_maps);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_cap_mb);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_nomem);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_wire_fail);
        sysctl_unregister_oid(&sysctl__debug_phxbo_vmmap_fail);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_mb);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_mb_peak);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_ro);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_wire_err_ro);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_wire_err);
        sysctl_unregister_oid(&sysctl__debug_phxbo_host_readonly);
        gSysctlUp = 0;
    }
    clearRing();
    if (fbAper) {
        fbAper->release();
        fbAper = nullptr;
    }
    if (sysBuf) {
        sysBuf->release();
        sysBuf = nullptr;
    }
    gq[0].ready = gq[1].ready = false;
    for (VmCtx &c : vm) {
        for (IOBufferMemoryDescriptor *b : c.chunk)
            if (b)
                b->release();
        c = VmCtx{};
    }
    for (HwVmid &h : hwv)
        h = HwVmid{};
    if (gpuLock) {
        IOLockFree(gpuLock);
        gpuLock = nullptr;
    }
    if (doorbellMap) {
        doorbellMap->release();
        doorbellMap = nullptr;
    }
    if (mmio) {
        mmio->release();
        mmio = nullptr;
    }
    if (pci) {
        pci->release();
        pci = nullptr;
    }
    if (smuLock) {
        IOLockFree(smuLock);
        smuLock = nullptr;
    }
    IOService::stop(provider);
}

uint32_t PhxBringup::rreg(uint32_t reg) const {
    return reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg];
}

void PhxBringup::wreg(uint32_t reg, uint32_t value) const {
    reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg] = value;
}

// "AMD Radeon <model>" from the CPU brand string's "Radeon <model>", else "AMD Radeon Graphics".
static void cpuGpuName(char *out, size_t n) {
    char brand[49] = {0};
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t a = 0x80000002u + i, b, c, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        memcpy(brand + 16 * i, &a, 4); memcpy(brand + 16 * i + 4, &b, 4);
        memcpy(brand + 16 * i + 8, &c, 4); memcpy(brand + 16 * i + 12, &d, 4);
    }
    strlcpy(out, "AMD Radeon Graphics", n);
    for (const char *p = brand; *p; p++) {
        if (strncmp(p, "Radeon ", 7) != 0)
            continue;
        size_t len = 0;
        while (p[7 + len] && p[7 + len] != ' ' && len < 8)
            len++;
        if (len && p[7 + len - 1] == 'M') {   // a model like "740M", not "Graphics"
            strlcpy(out, "AMD Radeon ", n);
            strlcat(out, p + 7, n < 11 + len + 1 ? n : 11 + len + 1);
        }
        break;
    }
}

// Reads the IP discovery table the way Linux does before it knows the chip's register layout and picks the chip
// profile from it (amdgpu_discovery.c: amdgpu_discovery_get_tmr_info, amdgpu_discovery_read_binary_from_mem ->
// amdgpu_device.c amdgpu_device_mm_access): wait up to 2 s for the IFWI-init flag (bit 31) in MP0 C2PMSG_33, take the
// table at RCC_CONFIG_MEMSIZE MiB - DISCOVERY_TMR_OFFSET (64 KiB), DISCOVERY_TMR_SIZE (10 KiB), or where
// DRIVER_SCRATCH_0..2 point, and read it through the MM_INDEX/MM_DATA window. MM_INDEX/MM_INDEX_HI are the only
// registers written. These fixed offsets are the ones Linux uses before discovery, for every chip.
void PhxBringup::identifyChip() {
    constexpr uint32_t mmMM_INDEX = 0x0, mmMM_DATA = 0x1, mmMM_INDEX_HI = 0x6;
    constexpr uint32_t mmDRIVER_SCRATCH_0 = 0x94, mmDRIVER_SCRATCH_1 = 0x95, mmDRIVER_SCRATCH_2 = 0x96;
    constexpr uint32_t mmRCC_CONFIG_MEMSIZE = 0xde3, mmMP0_SMN_C2PMSG_33 = 0x16061;
    constexpr uint64_t kTmrOffset = 64u << 10;
    constexpr uint32_t kTmrSize = 10u << 10, kMaxSize = 64u << 10, kBinarySignature = 0x28211407;
    if ((static_cast<uint64_t>(mmMP0_SMN_C2PMSG_33) + 1) * 4 > mmio->getLength()) {
        setProperty("phx-chip", "unknown (BAR5 too small)");
        return;
    }
    uint32_t ms = 0;
    while (!(rreg(mmMP0_SMN_C2PMSG_33) & 0x80000000u) && ms < 2000) {
        IOSleep(1);
        ms++;
    }
    auto vram32 = [this](uint64_t pos) {
        wreg(mmMM_INDEX, static_cast<uint32_t>(pos) | 0x80000000u);
        wreg(mmMM_INDEX_HI, static_cast<uint32_t>(pos >> 31));
        return rreg(mmMM_DATA);
    };
    const uint32_t memsizeMb = rreg(mmRCC_CONFIG_MEMSIZE);
    uint64_t off = 0;
    uint32_t size = kTmrSize, sig = 0;
    if (memsizeMb && memsizeMb != 0xFFFFFFFFu) {
        off = (static_cast<uint64_t>(memsizeMb) << 20) - kTmrOffset;
        sig = vram32(off);
    }
    if (sig != kBinarySignature && rreg(mmDRIVER_SCRATCH_2)) {
        size = rreg(mmDRIVER_SCRATCH_2);
        off = (static_cast<uint64_t>(rreg(mmDRIVER_SCRATCH_1)) << 32) | rreg(mmDRIVER_SCRATCH_0);
        sig = vram32(off);
    }
    setProperty("phx-discovery-offset", off, 64);
    if (sig != kBinarySignature) {
        setProperty("phx-chip", "unknown (no discovery table)");
        return;
    }
    size = (size > kMaxSize ? kMaxSize : size) & ~3u;
    uint32_t *buf = static_cast<uint32_t *>(IOMalloc(size));
    if (!buf) {
        setProperty("phx-chip", "unknown (no memory)");
        return;
    }
    for (uint32_t i = 0; i < size / 4; i++)
        buf[i] = vram32(off + static_cast<uint64_t>(i) * 4);
    rdna3::IpDiscoveryParser disc;
    const rdna3::ChipProfile *p = nullptr;
    if (disc.init(reinterpret_cast<const uint8_t *>(buf), size) && (p = rdna3::chip_profile(disc)) &&
        rdna3::chip_regs(disc, *p, cr))
        chip = p;
    if (OSData *bin = OSData::withBytes(buf, size)) {
        setProperty("phx-discovery-bin", bin);   // tools/parse_discovery.py reads this
        bin->release();
    }
    IOFree(buf, size);
    setProperty("phx-chip", chip ? chip->name : "unknown (IP versions not supported)");
    if (chip)
        setProperty("phx-gc-version", gcVersion(), 32);
}

// FB location (MC, 16 MiB units, FB_BASE bits 23:0) and the system physical address of the carve-out (FB_OFFSET << 24).
void PhxBringup::readFbLayout() {
    uint32_t base = rreg(cr.mmhub_fb_location_base) & 0x00FFFFFF;
    uint32_t top = rreg(cr.mmhub_fb_location_top) & 0x00FFFFFF;
    fbMcBase = static_cast<uint64_t>(base) << 24;
    fbSize = top >= base ? (static_cast<uint64_t>(top - base) + 1) << 24 : 0;
    fbPhys = static_cast<uint64_t>(rreg(cr.mmhub_fb_offset)) << 24;
    setProperty("phx-fb-mc-base", fbMcBase, 64);
    setProperty("phx-fb-size", fbSize, 64);
    setProperty("phx-fb-phys", fbPhys, 64);
    // About This Mac / System Information name the GPU from the PCI device's "model" and size its memory from
    // "VRAM,totalMB"; without them the 780M read "Display, 15 MB" (the boot framebuffer). Phoenix 2 parts are named
    // after the CPU brand string ("AMD Ryzen 5 8500G w/ Radeon 740M Graphics" on the 8500G).
    char kModel[40] = "AMD Radeon 780M";
    if (chip->chip == rdna3::PhxChip::Phoenix2)
        cpuGpuName(kModel, sizeof kModel);
    if (OSData *m = OSData::withBytes(kModel, strlen(kModel) + 1)) {
        pci->setProperty("model", m);
        m->release();
    }
    if (fbSize)
        pci->setProperty("VRAM,totalMB", fbSize >> 20, 32);
}

void PhxBringup::clearRing() {
    ringMc = ringOff = 0;
    cmdOff = cmdMc = fenceOff = fenceMc = 0;
    fenceCounter = 0;
    tmrOff = tmrMc = tmrSize = 0;
    fwBufOff = fwBufMc = fwBufSize = 0;
    tmrSetUp = false;
}

// Drain the host data path so the PSP sees our VRAM writes before we ring the doorbell (C2PMSG_67).
void PhxBringup::hdpFlush() const {
    if (hdpFlushDword)
        wreg(hdpFlushDword, 0);
    (void)rreg(0x0DE3);   // read-back barrier (RCC_CONFIG_MEMSIZE, amdgpu_discovery.c; any read does), as in the reference
}

// Read-only: count dwords from VRAM at byteOff, through the BAR0 aperture. Used by debug selector 102 to
// find the live console framebuffer and a free (all-zero) VRAM region for the PSP structures.
IOReturn PhxBringup::vramRead(uint32_t byteOff, uint32_t count, uint32_t *out) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || (static_cast<uint64_t>(byteOff) + static_cast<uint64_t>(count) * 4) > fbAperLen)
        return kIOReturnBadArgument;
    volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff);
    for (uint32_t i = 0; i < count; i++)
        out[i] = v[i];
    return kIOReturnSuccess;
}

// Write one MMIO dword (bring-up experimentation; root + phxdebug only). Returns the readback.
IOReturn PhxBringup::writeReg(uint32_t reg, uint32_t value, uint32_t *readback) {
    if (!mmio)
        return kIOReturnNotReady;
    if ((static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength())
        return kIOReturnBadArgument;
    wreg(reg, value);
    __asm__ __volatile__("mfence" ::: "memory");
    *readback = rreg(reg);
    return kIOReturnSuccess;
}

// The monitor's EDID with the display engine's hardware I2C engine on DDC1, as Linux dce_i2c_hw.c drives it
// (phx_dcn_i2c.h). MMIO through rreg/wreg; every poll bounded (the I2C transaction timeout Linux computes, at most
// ~0.4 s per 128-byte block at 100 kHz). The pad and the engine are only used when GOP left them in hardware I2C mode.
IOReturn PhxBringup::readEdid(uint8_t out[256], uint32_t *len, uint32_t *result) {
    *len = 0;
    *result = phxi2c::kEngineUnavailable;
    if (!mmio)
        return kIOReturnNotReady;
    if ((static_cast<uint64_t>(phxi2c::kHighestReg) + 1) * 4 > mmio->getLength())
        return kIOReturnBadArgument;
    struct Hw {
        const PhxBringup *b;
        uint32_t rd(uint32_t reg) { return b->rreg(reg); }
        void wr(uint32_t reg, uint32_t v) { b->wreg(reg, v); }
        void delayUs(uint32_t us) { IODelay(us); }
    } hw{this};
    *result = phxi2c::readEdid(hw, out, len);
    if (debug)
        IOLog("PhxBringup: EDID over DDC1: %s, %u bytes\n", phxi2c::resultName(*result), *len);
    return kIOReturnSuccess;
}

// Write one dword into VRAM at byteOff through the BAR0 aperture.
// op 1: allocate a physically contiguous buffer of a bytes (replaces any previous), out0 = physical address
// op 2: read dword at byte offset a -> out1;  op 3: write dword b at byte offset a;  op 4: free
IOReturn PhxBringup::sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1) {
    *out0 = *out1 = 0;
    if (op == 1) {
        if (sysBuf) {
            sysBuf->release();
            sysBuf = nullptr;
        }
        if (a == 0 || a > (64ull << 20))
            return kIOReturnBadArgument;
        sysBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, a, 0x00007FFFFFFFF000ull);
        if (!sysBuf)
            return kIOReturnNoMemory;
        bzero(sysBuf->getBytesNoCopy(), a);
        *out0 = sysBuf->getPhysicalAddress();
        *out1 = sysBuf->getLength();
        return kIOReturnSuccess;
    }
    if (!sysBuf)
        return kIOReturnNotReady;
    if (op == 4) {
        sysBuf->release();
        sysBuf = nullptr;
        return kIOReturnSuccess;
    }
    if ((a & 3) || a + 4 > sysBuf->getLength())
        return kIOReturnBadArgument;
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(sysBuf->getBytesNoCopy()) + a);
    if (op == 2) {
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory");
        *out1 = *p;
        *out0 = sysBuf->getPhysicalAddress();
        return kIOReturnSuccess;
    }
    if (op == 3) {
        *p = static_cast<uint32_t>(b);
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); // GPU walker may not snoop CPU caches
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

// ---- E8: GPU execution ------------------------------------------------------------------------------------
// Everything here reproduces what tools/phxctl/mesboot.c proved on the 780M: VMID 8 with a 4-level table in
// system memory (PDE = next|VALID|SYSTEM|SNOOPED, no BFS/TF since gmc_v11 has translate_further off), system-page
// PTEs with MTYPE 4, invalidation on engine 17, and an ACQUIRE/RELEASE_MEM GL2 writeback after each IB.
static constexpr uint32_t kFirstUserVmid = 8, kNumUserVmids = 8;   // VMIDs 8..15, lent to client contexts (vmBind)
static constexpr uint64_t kPteSysFlags = 0x77ull | (4ull << 48);  // VALID SYSTEM SNOOPED EXEC READ WRITE, MTYPE 4
static constexpr uint64_t kPtAddrMask = 0x0000FFFFFFFFF000ull;
// GC registers (absolute dword index, GC base 0x1260): context n = context 0 + n (CNTL) / + 2n (address pairs),
// invalidation engine 17
static constexpr uint32_t kRegCtx0Cntl = 0x28E8, kRegCtx0BaseLo = 0x2953, kRegCtx0StartLo = 0x2973, kRegCtx0EndLo = 0x2993;
static constexpr uint32_t kRegInvEng17Req = 0x291C, kRegInvEng17Ack = 0x292E;

static inline void flushLine(const volatile void *p) {
    __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
}

uint64_t *PhxBringup::ptTable(const VmCtx &c, uint64_t phys) const {
    for (uint32_t i = 0; i < kPtChunks && c.chunk[i]; i++)
        if (phys - c.chunkPhys[i] < kPtChunkBytes)
            return reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(c.chunk[i]->getBytesNoCopy()) + (phys - c.chunkPhys[i]));
    return nullptr;
}

// The physical address of table i, allocating (zeroed, flushed) the chunk it lives in; 0 when that fails.
uint64_t PhxBringup::ptTablePhys(VmCtx &c, uint32_t i) {
    const uint32_t k = i / kPtChunkPages;
    if (k >= kPtChunks)
        return 0;
    if (!c.chunk[k]) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kPtChunkBytes, kPtAddrMask);
        if (!b)
            return 0;
        uint8_t *p = static_cast<uint8_t *>(b->getBytesNoCopy());
        bzero(p, kPtChunkBytes);
        for (uint32_t o = 0; o < kPtChunkBytes; o += 64)
            flushLine(p + o);
        c.chunk[k] = b;
        c.chunkPhys[k] = b->getPhysicalAddress();
    }
    return c.chunkPhys[k] + static_cast<uint64_t>(i % kPtChunkPages) * 4096;
}

uint64_t *PhxBringup::vmPte(VmCtx &c, uint64_t va, bool alloc) {
    uint64_t tbl = c.phys;
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t *t = ptTable(c, tbl);
        if (!t)
            return nullptr;
        uint64_t *e = t + ((va >> shift) & 511);
        if (!(*e & 1)) {
            if (!alloc || c.used >= c.pages)
                return nullptr;
            const uint64_t child = ptTablePhys(c, c.used);
            if (!child)
                return nullptr;
            c.used++;
            *e = child | 7;
            flushLine(e);
        }
        tbl = *e & kPtAddrMask;
    }
    uint64_t *t = ptTable(c, tbl);
    return t ? t + ((va >> 12) & 511) : nullptr;
}

void PhxBringup::vmFlushTlb(uint32_t vmid) const {
    __asm__ __volatile__("mfence" ::: "memory");
    wreg(kRegInvEng17Req, 0x00F80000u | (1u << vmid) | 1u);
    for (int i = 0; i < 1000 && !(rreg(kRegInvEng17Ack) & (1u << vmid)); i++)
        IODelay(1);
}

// Takes a free context: empty tables (root only), no hardware VMID until its first submission. Returns its handle,
// 0 when all kMaxCtx are in use.
uint32_t PhxBringup::vmidAlloc() {
    if (!gpuReady())
        return 0;
    IOLockLock(gpuLock);
    for (uint32_t i = 0; i < kMaxCtx; i++) {
        VmCtx &c = vm[i];
        if (c.busy)
            continue;
        if (!c.chunk[0]) {
            c.phys = ptTablePhys(c, 0);
            if (!c.phys)
                break;
            c.pages = kPtChunks * kPtChunkPages;
        }
        c.used = 1;
        c.busy = true;
        c.hw = 0;
        IOLockUnlock(gpuLock);
        return i + 1;
    }
    IOLockUnlock(gpuLock);
    return 0;
}

// Points a hardware VMID at the context's tables for a submission (gpuLock held): the one it already has, a free one,
// or the least recently used one whose submissions the GPU has all finished (waits for one, bounded ~2 s). 0 on timeout.
uint32_t PhxBringup::vmBind(uint32_t handle) {
    VmCtx &c = vm[handle - 1];
    const uint64_t now = ++hwTick;
    if (c.hw) {
        hwv[c.hw - kFirstUserVmid].lastUse = now;
        return c.hw;
    }
    int pick = -1;
    for (int spin = 0; pick < 0; spin++) {
        const uint64_t done = gpuRetired();
        uint64_t oldest = ~0ull;
        for (uint32_t i = 0; i < kNumUserVmids; i++) {
            if (!hwv[i].ctx) {
                pick = static_cast<int>(i);
                break;
            }
            if (hwv[i].lastSeq <= done && hwv[i].lastUse < oldest) {
                oldest = hwv[i].lastUse;
                pick = static_cast<int>(i);
            }
        }
        if (pick >= 0)
            break;
        if (spin > 40000)
            return 0;
        IODelay(50);
    }
    HwVmid &h = hwv[pick];
    if (h.ctx)
        vm[h.ctx - 1].hw = 0;
    h.ctx = handle;
    h.lastSeq = 0;
    h.lastUse = now;
    const uint32_t vmid = kFirstUserVmid + static_cast<uint32_t>(pick);
    c.hw = vmid;
    wreg(kRegCtx0BaseLo + 2 * vmid, static_cast<uint32_t>(c.phys | 3));
    wreg(kRegCtx0BaseLo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
    wreg(kRegCtx0Cntl + vmid, 0x01fffe07);
    wreg(kRegCtx0StartLo + 2 * vmid, 0);
    wreg(kRegCtx0StartLo + 2 * vmid + 1, 0);
    wreg(kRegCtx0EndLo + 2 * vmid, 0xffffffff);
    wreg(kRegCtx0EndLo + 2 * vmid + 1, 0xf);
    vmFlushTlb(vmid);
    return vmid;
}

// Returns a context once the GPU is past the client's last submission (bounded wait): the tables used so far are
// cleared, so nothing the client mapped stays reachable, and its hardware VMID (if any) is freed.
void PhxBringup::vmidFree(uint32_t handle, uint64_t lastSeq) {
    if (!vmCtx(handle))
        return;
    for (int i = 0; i < 2000 && gpuRetired() < lastSeq; i++)
        IOSleep(1);
    IOLockLock(gpuLock);
    VmCtx &c = vm[handle - 1];
    for (uint32_t k = 0; k < kPtChunks && c.chunk[k] && k * kPtChunkPages < c.used; k++) {
        const uint32_t n = c.used - k * kPtChunkPages < kPtChunkPages ? c.used - k * kPtChunkPages : kPtChunkPages;
        uint8_t *p = static_cast<uint8_t *>(c.chunk[k]->getBytesNoCopy());
        bzero(p, static_cast<size_t>(n) * 4096);
        for (uint32_t o = 0; o < n * 4096u; o += 64)
            flushLine(p + o);
    }
    if (c.hw) {
        vmFlushTlb(c.hw);
        hwv[c.hw - kFirstUserVmid].ctx = 0;
        c.hw = 0;
    }
    c.used = 1;
    c.busy = false;
    IOLockUnlock(gpuLock);
}

IOReturn PhxBringup::adoptQueue(const uint64_t in[6]) {
    if (!fbAper || !doorbellMap || !mmio || !fbMcBase || !gpuLock)
        return kIOReturnNotReady;
    // in[5] = fence offset | queue index (0 compute, 1 gfx): the fence is 8-byte aligned, so bit 0 is free
    const uint64_t ringOff = in[0], ringDw = in[1], wbOff = in[2], dbByte = in[3], wptr = in[4], fenceOff = in[5] & ~1ull;
    const uint32_t qi = static_cast<uint32_t>(in[5] & 1);
    if (ringDw < 256 || ringDw > 65536 || (ringDw & (ringDw - 1)) || (ringOff & 0xFF) ||
        ringOff + ringDw * 4 > fbAperLen || (wbOff & 7) || wbOff + 32 > fbAperLen || (fenceOff & 7) ||
        fenceOff + 8 > fbAperLen || (dbByte & 7) || dbByte + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    GpuQueue &q = gq[qi];
    bzero(&q, sizeof(q));
    q.gfx = qi == 1;
    q.ringOff = static_cast<uint32_t>(ringOff);
    q.ringDw = static_cast<uint32_t>(ringDw);
    q.wbOff = static_cast<uint32_t>(wbOff);
    q.fenceOff = static_cast<uint32_t>(fenceOff);
    q.dbByte = dbByte;
    q.wptr = q.doneEnd = wptr;
    volatile uint32_t *fence = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
    fence[0] = static_cast<uint32_t>(gSeq);
    fence[1] = static_cast<uint32_t>(gSeq >> 32);
    hdpFlush();
    q.ready = true;
    IOLockUnlock(gpuLock);
    IOLog("PhxBringup: adopted %s queue ring=+0x%llx (%llu dw) wptr=%llu doorbell=+0x%llx\n",
          qi ? "gfx" : "compute", ringOff, ringDw, wptr, dbByte);
    return kIOReturnSuccess;
}

// PhxAccel (the NullMoth accelerator in its AMD configuration) starts only on a GPU marked as driven; matching is
// re-run so the accelerator personality, refused at boot, is offered the device again.
IOReturn PhxBringup::claimAccel() {
    if (!pci || !gpuReady())
        return kIOReturnNotReady;
    pci->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci->registerService();
    IOLog("PhxBringup: GPU claimed for PhxAccel, matching re-run\n");
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vmMap(uint32_t handle, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size, bool writable) {
    // a read-only host import gets no AMDGPU_PTE_WRITEABLE (bit 6, amdgpu_vm.h): the GPU may read it, never write it
    const uint64_t flags = writable ? kPteSysFlags : (kPteSysFlags & ~(1ull << 6));
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        IOByteCount segLen = 0;
        const addr64_t pa = md->getPhysicalSegment(off + p, &segLen, kIOMemoryMapperNone);
        uint64_t *pte = pa ? vmPte(c, va + p, true) : nullptr;
        if (!pte) {
            r = pa ? kIOReturnNoMemory : kIOReturnVMError;
            break;
        }
        *pte = (pa & kPtAddrMask) | flags;
        flushLine(pte);
    }
    if (c.hw)   // an unbound context is flushed when it is next bound
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

IOReturn PhxBringup::vmUnmap(uint32_t handle, uint64_t va, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        uint64_t *pte = vmPte(c, va + p, false);
        if (pte) {
            *pte = 0;
            flushLine(pte);
        }
    }
    if (c.hw)
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return kIOReturnSuccess;
}

void PhxBringup::ringPut(GpuQueue &q, uint32_t idx, uint32_t value) {
    const uint64_t slot = (q.wptr + idx) & (q.ringDw - 1);
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff + slot * 4) = value;
}

uint64_t PhxBringup::queueFence(const GpuQueue &q) const {
    const volatile uint32_t *f = reinterpret_cast<const volatile uint32_t *>(fbAper->getVirtualAddress() + q.fenceOff);
    return static_cast<uint64_t>(f[0]) | (static_cast<uint64_t>(f[1]) << 32);
}

// Everything up to the returned sequence has finished: the oldest submission still running on any queue, minus one.
// Lock-free reader: an entry is written before `tail` moves past it.
uint64_t PhxBringup::gpuRetired() const {
    uint64_t r = gSeq;
    for (const GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        for (uint32_t i = q.head; i != tail; i++) {
            if (q.fseq[i & 63] > done) {
                if (q.fseq[i & 63] - 1 < r)
                    r = q.fseq[i & 63] - 1;
                break;
            }
        }
    }
    return r;
}

IOReturn PhxBringup::gpuSubmit(uint32_t handle, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut) {
    // graphics IBs go to the gfx queue and compute IBs to the compute queue; with one of them missing, the other runs both
    GpuQueue *qp = gfx ? (gq[1].ready ? &gq[1] : &gq[0]) : (gq[0].ready ? &gq[0] : &gq[1]);
    if (!qp->ready || !vmCtx(handle))
        return kIOReturnNotReady;
    GpuQueue &q = *qp;
    const uint32_t need = n * 4 + 8;
    if (need + 16 > q.ringDw)
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    // Ring space and the 64-entry history: retire finished entries, wait for the oldest one if needed (bounded, ~2 s).
    for (int spin = 0;; spin++) {
        const uint64_t done = queueFence(q);
        while (q.head != q.tail && q.fseq[q.head & 63] <= done) {
            q.doneEnd = q.fend[q.head & 63];
            q.head++;
        }
        if (q.tail - q.head < 63 && q.wptr + need - q.doneEnd <= q.ringDw - 16)
            break;
        if (spin > 40000) {
            IOLockUnlock(gpuLock);
            return kIOReturnBusy;
        }
        IODelay(50);
    }
    const uint32_t vmid = vmBind(handle);
    if (!vmid) {
        IOLockUnlock(gpuLock);
        return kIOReturnBusy;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        ringPut(q, k++, 0xC0023F00);                               // INDIRECT_BUFFER
        // the CP wants the canonical (sign-extended) address: an upper-half IB (bit 47, where RADV puts them) with a
        // bare 16-bit high word silently wedges the queue
        const uint64_t cva = (ibVa[i] & (1ull << 47)) ? (ibVa[i] | 0xFFFF000000000000ull) : ibVa[i];
        ringPut(q, k++, static_cast<uint32_t>(cva) & ~3u);
        ringPut(q, k++, static_cast<uint32_t>(cva >> 32));
        // size | VMID, plus VALID on compute (gfx_v11_0_ring_emit_ib_compute; the gfx variant has no VALID bit)
        ringPut(q, k++, ibDw[i] | (q.gfx ? 0u : (1u << 23)) | (vmid << 24));
    }
    const uint64_t seq = ++gSeq;
    hwv[vmid - kFirstUserVmid].lastSeq = seq;
    const uint64_t fenceMc = fbMcBase + q.fenceOff;
    ringPut(q, k++, 0xC0064900);                                   // RELEASE_MEM, as gfx_v11_0_ring_emit_fence
    ringPut(q, k++, 0x06603514);                                   // GCR SEQ|GL2_WB|GLM_INV|GLM_WB, CP 3, CACHE_FLUSH_AND_INV_TS, idx 5
    ringPut(q, k++, 0x40000000);                                   // DATA_SEL 2 (64-bit), no interrupt
    ringPut(q, k++, static_cast<uint32_t>(fenceMc));
    ringPut(q, k++, static_cast<uint32_t>(fenceMc >> 32));
    ringPut(q, k++, static_cast<uint32_t>(seq));
    ringPut(q, k++, static_cast<uint32_t>(seq >> 32));
    ringPut(q, k++, 0);
    q.wptr += k;
    q.fseq[q.tail & 63] = seq;
    q.fend[q.tail & 63] = q.wptr;
    __asm__ __volatile__("" ::: "memory");
    q.tail = q.tail + 1;
    // BAR0 is write-combined: drain the WC buffers so the packets are in VRAM before the CP is told about them
    __asm__ __volatile__("sfence" ::: "memory");
    (void)*reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff);
    hdpFlush();
    volatile uint32_t *wb = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.wbOff + 16);
    wb[0] = static_cast<uint32_t>(q.wptr);
    wb[1] = static_cast<uint32_t>(q.wptr >> 32);
    __asm__ __volatile__("sfence" ::: "memory");
    (void)wb[0];
    hdpFlush();
    __asm__ __volatile__("mfence" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + q.dbByte) = q.wptr;
    IOLockUnlock(gpuLock);
    *seqOut = seq;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::doorbellWrite(uint64_t byteOff, uint64_t value) {
    if (!doorbellMap)
        return kIOReturnNotReady;
    if ((byteOff & 7) || byteOff + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + byteOff) = value;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vramWrite(uint32_t byteOff, uint32_t value) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || static_cast<uint64_t>(byteOff) + 4 > fbAperLen)
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff) = value;
    __asm__ __volatile__("sfence" ::: "memory");
    return kIOReturnSuccess;
}

// psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK): response flag set and status 0.
bool PhxBringup::pspWaitTos(uint32_t timeoutMs) const {
    for (uint32_t i = 0; i < timeoutMs * 10; i++) {
        if ((rreg(cr.psp_c2pmsg_64) & PHX_MBOX_TOS_MASK) == PHX_MBOX_TOS_READY)
            return true;
        IODelay(100);
    }
    return false;
}

// Debug selector 101: create / destroy the PSP 13.0.4 KM ring the way Linux psp_v13_0_ring_create / _stop and
// Cobalt64 psp_v14_0.cpp do. The ring lives in VRAM at carveOffset; the CPU writes it through the BAR0
// framebuffer aperture, which is MMIO above system RAM — so the PSP sees the bytes and the macOS heap is never
// touched. The MC address handed to the PSP is fbMcBase + carveOffset.
IOReturn PhxBringup::pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]) {
    if (!hwWritesAllowed())
        return kIOReturnUnsupported;
    if (!mmio || !fbAper || !fbMcBase)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    IOLockLock(smuLock);
    if (op == 1) {
        if (ringMc) {
            r = kIOReturnExclusiveAccess;   // a ring is already live; destroy it first
        } else if ((carveOffset & 0xFFF) || carveOffset + PHX_PSP_FENCE_REL + 0x1000 > PHX_VRAM_TMR_OFF ||
                   PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE > fbAperLen) {
            r = kIOReturnBadArgument;        // ring+cmd+fence must sit below the TMR; fw_buf must fit the aperture
        } else {
            volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + carveOffset);
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                nonzero |= ring[i];
            out[2] = nonzero == 0;           // report whether the VRAM region was already clear (informational)
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                ring[i] = 0;                 // zero the ring buffer (Linux memsets ring_mem; safe via BAR0)
            if (!pspWaitTos(2000)) {
                r = kIOReturnNotReady;       // SOS not ready to accept the ring
            } else {
                ringMc = fbMcBase + carveOffset;
                wreg(cr.psp_c2pmsg_69, static_cast<uint32_t>(ringMc));
                wreg(cr.psp_c2pmsg_70, static_cast<uint32_t>(ringMc >> 32));
                wreg(cr.psp_c2pmsg_71, PHX_PSP_RING_SIZE);
                wreg(cr.psp_c2pmsg_64, PHX_PSP_RING_TYPE_KM << 16);
                IOSleep(20);
                if (!pspWaitTos(2000)) {
                    r = kIOReturnTimeout;    // raw C2PMSG_64 (with PSP status in bits 15:0) returned in out[0]
                    ringMc = 0;              // not established: allow destroy + retry
                } else {
                    // Ring is live. Lay the GPCOM command + fence buffers just above it and zero them,
                    // then reset the doorbell so wptr agrees with our slot 0 (Linux psp_v13_0_ring_create).
                    ringOff = carveOffset;
                    cmdOff = carveOffset + PHX_PSP_CMD_REL;
                    fenceOff = carveOffset + PHX_PSP_FENCE_REL;
                    cmdMc = fbMcBase + cmdOff;
                    fenceMc = fbMcBase + fenceOff;
                    fenceCounter = 0;
                    tmrOff = PHX_VRAM_TMR_OFF;
                    tmrMc = fbMcBase + tmrOff;
                    tmrSize = 0;
                    tmrSetUp = false;
                    fwBufOff = PHX_VRAM_FWBUF_OFF;
                    fwBufMc = fbMcBase + fwBufOff;
                    fwBufSize = PHX_VRAM_FWBUF_SIZE;
                    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + cmdOff);
                    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
                    for (uint32_t i = 0; i < 0x1000 / 4; i++) {
                        cmd[i] = 0;
                        fen[i] = 0;
                    }
                    wreg(cr.psp_c2pmsg_67, 0);
                }
            }
        }
    } else if (op == 2) {
        wreg(cr.psp_c2pmsg_64, PHX_GFX_CTRL_CMD_ID_DESTROY_RINGS);
        IOSleep(20);
        if (!pspWaitTos(2000))
            r = kIOReturnTimeout;
        clearRing();
    } else if (op != 3) {
        r = kIOReturnBadArgument;
    }
    out[0] = rreg(cr.psp_c2pmsg_64);
    out[1] = ringMc;
    out[3] = rreg(cr.psp_c2pmsg_67);
    IOLockUnlock(smuLock);
    return r;
}

// Core GPCOM submit (caller holds smuLock and a ring exists): write a prepared command — cmd_id plus up to
// nWords union dwords at +28 — into the VRAM command buffer, ring the doorbell, wait for the PSP's fence and
// read the response. Ports Linux psp_cmd_submit_buf / Cobalt64 psp_ring_cmd_submit.
IOReturn PhxBringup::pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]) {
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress());
    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(base + cmdOff);
    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(base + fenceOff);

    // Command buffer: zero it, set cmd_id (buf_size/buf_version stay 0, as upstream does), then the union.
    for (uint32_t i = 0; i < PHX_PSP_CMD_BUF_SIZE / 4; i++)
        cmd[i] = 0;
    cmd[PHX_PSP_CMD_HDR_CMD_ID_OFFSET / 4] = cmdId;
    for (uint32_t i = 0; i < nWords && i < 14; i++)
        cmd[PHX_PSP_GFX_CMD_UNION_OFFSET / 4 + i] = unionWords[i];

    uint32_t fence = ++fenceCounter;
    fen[0] = 0;

    // Ring frame at the slot the current wptr points to.
    uint32_t wptr = rreg(cr.psp_c2pmsg_67);
    const uint32_t ringSizeDw = PHX_PSP_RING_SIZE / 4;         // 1024
    const uint32_t frameSizeDw = PHX_PSP_GFX_FRAME_SIZE / 4;   // 16
    uint32_t slot = (wptr % ringSizeDw == 0) ? 0 : wptr / frameSizeDw;
    if (slot >= PHX_PSP_RING_SIZE / PHX_PSP_GFX_FRAME_SIZE)
        return kIOReturnInternalError;
    PhxGfxRbFrame frame;
    bzero(&frame, sizeof(frame));
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(cmdMc);
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(cmdMc >> 32);
    frame.fence_addr_lo = static_cast<uint32_t>(fenceMc);
    frame.fence_addr_hi = static_cast<uint32_t>(fenceMc >> 32);
    frame.fence_value = fence;
    volatile uint32_t *slotp =
        reinterpret_cast<volatile uint32_t *>(base + ringOff + static_cast<uint64_t>(slot) * PHX_PSP_GFX_FRAME_SIZE);
    const uint32_t *src = reinterpret_cast<const uint32_t *>(&frame);
    for (uint32_t i = 0; i < PHX_PSP_GFX_FRAME_SIZE / 4; i++)
        slotp[i] = src[i];

    // Drain CPU write buffers, flush the host data path, then ring the doorbell.
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    wptr = (wptr + frameSizeDw) % ringSizeDw;
    wreg(cr.psp_c2pmsg_67, wptr);

    // Wait for the PSP to write the fence value into VRAM (BAR0 read bypasses the CPU cache).
    uint32_t observed = 0, us = 0;
    const uint32_t budgetUs = 5000000;
    while (us < budgetUs) {
        observed = fen[0];
        if (observed == fence)
            break;
        IOSleep(1);
        us += 1000;
    }
    uint32_t status = cmd[PHX_PSP_GFX_RESP_OFFSET / 4];
    // Always succeed at the transport level so IOKit delivers the outputs; the PSP result is in out[].
    // out[1] = elapsed us (≈budget on a fence timeout), so userspace distinguishes timeout from a status code.
    out[0] = (observed == fence) ? status : 0xFFFFFFFFu;  // 0xFFFFFFFF = fence never arrived
    out[1] = us;
    out[2] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 8) / 4];   // resp.fw_addr_lo / session_id
    out[3] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 12) / 4];  // resp.fw_addr_hi
    return kIOReturnSuccess;
}

// Selector 103: no-payload query command (FB_FW_RESERV_ADDR/EXT only) — validates the ring without touching GPU power.
IOReturn PhxBringup::pspSubmit(uint32_t cmdId, uint64_t out[4]) {
    if (!hwWritesAllowed())
        return kIOReturnUnsupported;
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if (cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_ADDR && cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_EXT_ADDR)
        return kIOReturnNotPermitted;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, nullptr, 0, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Selector 104: stage a chunk of firmware bytes into the VRAM fw_buf through BAR0 (no submit).
IOReturn PhxBringup::fwStage(uint32_t dstOffset, const void *bytes, uint32_t len) {
    if (!fbAper || !fwBufSize)
        return kIOReturnNotReady;
    if ((dstOffset & 3) || static_cast<uint64_t>(dstOffset) + len > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress() + fwBufOff + dstOffset);
    const uint8_t *s = static_cast<const uint8_t *>(bytes);
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    if (i < len) {
        uint32_t v = 0;
        memcpy(&v, s + i, len - i);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    IOLockUnlock(smuLock);
    return kIOReturnSuccess;
}

// Selector 105: submit GFX_CMD_ID_LOAD_IP_FW for the firmware already staged in fw_buf at fwBufOffset.
IOReturn PhxBringup::loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]) {
    if (!hwWritesAllowed())
        return kIOReturnUnsupported;
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if ((fwBufOffset & 3) || size == 0 || static_cast<uint64_t>(fwBufOffset) + size > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else {
        uint64_t mc = fwBufMc + fwBufOffset;
        uint32_t u[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, fwType};
        r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, out);
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 106: op 1 = SETUP_TMR (driver-managed TMR, Phoenix psp_13_0_4 is not in psp_skip_tmr), op 2 = AUTOLOAD_RLC.
IOReturn PhxBringup::pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]) {
    if (!hwWritesAllowed())
        return kIOReturnUnsupported;
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else if (op == 1) {
        uint32_t size = arg ? arg : 0x400000u;
        if (size > PHX_VRAM_TMR_CAP)
            size = PHX_VRAM_TMR_CAP;
        tmrSize = size;
        // psp_gfx_cmd_setup_tmr (Linux psp_prep_tmr_cmd_buf): buf_phy_addr = TMR MC address, buf_size,
        // bitfield(virt_phy_addr=1 => 0x2), system_phy_addr = the REAL system-physical address of the TMR
        // (amdgpu_gmc_vram_pa = mc - vram_start + vram_base_offset = fbPhys + tmrOff on this UMA part).
        uint64_t tmrPa = fbPhys + tmrOff;
        uint32_t u[6] = {static_cast<uint32_t>(tmrMc), static_cast<uint32_t>(tmrMc >> 32), size, 0x2u,
                         static_cast<uint32_t>(tmrPa), static_cast<uint32_t>(tmrPa >> 32)};
        r = pspRunCmd(PHX_PSP_GFX_CMD_SETUP_TMR, u, 6, out);
        if (r == kIOReturnSuccess)
            tmrSetUp = true;
    } else if (op == 2) {
        r = pspRunCmd(PHX_PSP_GFX_CMD_AUTOLOAD_RLC, nullptr, 0, out);
    } else if (op == 3) {
        // LOAD_TOC (amdgpu_psp.c psp_load_toc, psp_prep_load_toc_cmd_buf): the TOC staged at fw_buf offset 0 (selector
        // 104), arg = its size. out[2] = resp.tmr_size (psp_gfx_if.h psp_gfx_resp +16): the TMR this chip's firmware
        // needs (psp_tmr_init uses it instead of PSP_TMR_SIZE).
        if (!arg || arg > fwBufSize) {
            r = kIOReturnBadArgument;
        } else {
            uint32_t u[3] = {static_cast<uint32_t>(fwBufMc), static_cast<uint32_t>(fwBufMc >> 32), arg};
            r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_TOC, u, 3, out);
            if (r == kIOReturnSuccess && out[0] != 0xFFFFFFFFu) {
                volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + cmdOff);
                out[2] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 16) / 4];
            }
        }
    } else if (op == 4) {
        // SETUP_TMR the way Linux sets it up: size from LOAD_TOC rounded to PSP_TMR_ALIGNMENT (1 MiB, amdgpu_psp.h;
        // psp_tmr_init); psp_prep_tmr_cmd_buf: TMR MC address, size, virt_phy_addr = 1, and the TMR's system physical
        // address (amdgpu_gmc_vram_pa = fbPhys + offset on this UMA part). The TMR sits at 128 MiB of VRAM, where
        // bringup5.sh puts the 780M's, clear of the firmware framebuffer at 0 and of the ring, fw_buf, GART and queue
        // regions below 80 MiB. Op 1 and the 780M's raw command are unchanged.
        const uint64_t size = (static_cast<uint64_t>(arg) + 0xFFFFFull) & ~0xFFFFFull;
        if (!arg || size > PHX_VRAM_TMR_TOC_CAP || PHX_VRAM_TMR_TOC_OFF + size > fbSize) {
            r = kIOReturnBadArgument;
        } else {
            const uint64_t mc = fbMcBase + PHX_VRAM_TMR_TOC_OFF, pa = fbPhys + PHX_VRAM_TMR_TOC_OFF;
            uint32_t u[6] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), static_cast<uint32_t>(size),
                             0x2u, static_cast<uint32_t>(pa), static_cast<uint32_t>(pa >> 32)};
            r = pspRunCmd(PHX_PSP_GFX_CMD_SETUP_TMR, u, 6, out);
            if (r == kIOReturnSuccess && out[0] == 0) {
                tmrOff = PHX_VRAM_TMR_TOC_OFF;
                tmrMc = mc;
                tmrSize = size;
                tmrSetUp = true;
            }
            out[3] = size;   // what was asked for (after rounding)
        }
    } else {
        r = kIOReturnBadArgument;
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 107: submit an arbitrary GPCOM command with six caller-supplied union dwords. For bring-up
// experiments (SETUP_TMR / LOAD_TOC parameter sweeps) without rebuilding the kext.
IOReturn PhxBringup::pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]) {
    if (!hwWritesAllowed())
        return kIOReturnUnsupported;
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, words, 6, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Polls the SMU response register for up to 2 s (Linux: usec_timeout * 20); 0 means no answer.
uint32_t PhxBringup::smuPoll() const {
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t r = rreg(cr.smu_resp);
        if (r)
            return r;
        IODelay(10);
    }
    return 0;
}

// One SMU 13.0.4 mailbox transaction, the sequence of Linux smu_msg_v1_send_msg().
IOReturn PhxBringup::smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter) {
    if (!hwWritesAllowed())
        return kIOReturnUnsupported;
    if (!mmio || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    // The first message of the boot skips the pre-poll (Linux SMU_FW_INIT): nothing may have been sent yet.
    if (smuUsed && smuPoll() == 0) {
        IOLockUnlock(smuLock);
        return kIOReturnBusy;
    }
    smuUsed = true;
    wreg(cr.smu_resp, 0);
    wreg(cr.smu_arg, arg);
    wreg(cr.smu_msg, msg);
    uint32_t r = smuPoll();
    *resp = r;
    *argAfter = rreg(cr.smu_arg);
    IOLockUnlock(smuLock);
    return r ? kIOReturnSuccess : kIOReturnTimeout;
}

// Once the GPU runs, GB_ADDR_CONFIG is read from the hardware (Linux: gfx_v11_0.c gfx_v11_0_gpu_early_init); before,
// the profile's value. 0 on an unknown chip.
uint32_t PhxBringup::gbAddrConfig() const {
    if (!chip)
        return 0;
    if (gpuReady()) {
        uint32_t v = rreg(cr.gb_addr_config);
        if (v && v != 0xFFFFFFFFu)
            return v;
    }
    return gbAddrConfigBeforeBringup(chip->chip);
}

bool PhxBringup::claimSession() { return OSCompareAndSwap(0, 1, &sessionOpen); }

void PhxBringup::releaseSession() { OSCompareAndSwap(1, 0, &sessionOpen); }

IOReturn PhxBringup::readRegs(uint32_t first, uint32_t count, uint32_t *out) {
    if (!mmio)
        return kIOReturnNotReady;
    uint64_t len = mmio->getLength();
    if ((static_cast<uint64_t>(first) + count) * 4 > len)
        return kIOReturnBadArgument;
    volatile uint32_t *r = reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress());
    for (uint32_t i = 0; i < count; i++)
        out[i] = r[first + i];
    return kIOReturnSuccess;
}

IOReturn PhxBringup::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    // the sysctls go up with the first client, from user space, rather than in start(): the bootloader injects this
    // kext into the boot collection, and it starts long before the rest of BSD
    if (!gSysctlUp && OSCompareAndSwap(0, 1, &gSysctlUp)) {
        sysctl_register_oid(&sysctl__debug_phxbos_flush);
        sysctl_register_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_register_oid(&sysctl__debug_phxbo_handles_full);
        sysctl_register_oid(&sysctl__debug_phxbo_maps_full);
        sysctl_register_oid(&sysctl__debug_phxbo_nomem);
        sysctl_register_oid(&sysctl__debug_phxbo_peak_handles);
        sysctl_register_oid(&sysctl__debug_phxbo_peak_maps);
        sysctl_register_oid(&sysctl__debug_phxbo_host_cap_mb);
        sysctl_register_oid(&sysctl__debug_phxbo_host_nomem);
        sysctl_register_oid(&sysctl__debug_phxbo_host_wire_fail);
        sysctl_register_oid(&sysctl__debug_phxbo_vmmap_fail);
        sysctl_register_oid(&sysctl__debug_phxbo_host_mb);
        sysctl_register_oid(&sysctl__debug_phxbo_host_mb_peak);
        sysctl_register_oid(&sysctl__debug_phxbo_host_ro);
        sysctl_register_oid(&sysctl__debug_phxbo_host_wire_err_ro);
        sysctl_register_oid(&sysctl__debug_phxbo_host_wire_err);
        sysctl_register_oid(&sysctl__debug_phxbo_host_readonly);
    }
    if (type != PHXN_UC_TYPE)
        return kIOReturnBadArgument;
    IOAccelPhxNativeClient *client = OSTypeAlloc(IOAccelPhxNativeClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ---- user client ----------------------------------------------------------------------------------------

bool IOAccelPhxNativeClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type))
        return false;
    privileged = clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task = owningTask;
    lock = IOLockAlloc();
    bos = static_cast<Bo *>(IOMallocZero(sizeof(Bo) * PHXN_MAX_BOS));
    maps = static_cast<Map *>(IOMallocZero(sizeof(Map) * kMaxMaps));
    return lock && bos && maps;
}

bool IOAccelPhxNativeClient::start(IOService *provider) {
    owner = OSDynamicCast(PhxBringup, provider);
    if (!owner)
        return false;
    // E9: several processes at once, each in its own GPUVM context (vmidAlloc); the first one still marks the session
    if (owner->claimSession())
        sessionClaimed = true;
    return IOUserClient::start(provider);
}

void IOAccelPhxNativeClient::freeAll() {
    if (!bos)
        return;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++)
        if (bos[h].md)
            boRelease(bos[h]);
}

IOReturn IOAccelPhxNativeClient::clientClose() {
    if (lock)
        IOLockLock(lock);
    freeAll();
    if (lock)
        IOLockUnlock(lock);
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    if (sessionClaimed && owner) {
        owner->releaseSession();
        sessionClaimed = false;
    }
    terminate();
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::free() {
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    freeAll();
    if (sessionClaimed && owner)
        owner->releaseSession();
    if (bos)
        IOFree(bos, sizeof(Bo) * PHXN_MAX_BOS);
    if (maps)
        IOFree(maps, sizeof(Map) * kMaxMaps);
    if (lock)
        IOLockFree(lock);
    IOUserClient::free();
}

IOAccelPhxNativeClient::Bo *IOAccelPhxNativeClient::findBo(uint32_t h) {
    if (h == 0 || h >= PHXN_MAX_BOS || !bos[h].md)
        return nullptr;
    return &bos[h];
}

IOReturn IOAccelPhxNativeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOLockLock(lock);
    Bo *b = findBo(type);
    if (!b) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    b->md->retain();
    *memory = b->md;
    *options = 0;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

// Exact shape check, as in the contract: scalar counts and struct sizes must match.
static bool shape(const IOExternalMethodArguments *a, uint32_t sin, uint32_t sout, uint32_t stin, uint32_t stout) {
    return a->scalarInputCount == sin && a->scalarOutputCount == sout && a->structureInputSize == stin &&
           a->structureOutputSize == stout && !a->structureInputDescriptor && !a->structureOutputDescriptor;
}

// Debug selectors that write the GPU: PSP, SMU, registers, VRAM, doorbells, firmware staging, queue adoption, the claim.
static bool writesGpu(uint32_t sel) {
    switch (sel) {
    case PHXN_SEL_DBG_SMU: case PHXN_SEL_DBG_PSP_RING: case PHXN_SEL_DBG_PSP_SUBMIT: case PHXN_SEL_DBG_FW_STAGE:
    case PHXN_SEL_DBG_PSP_LOADIPFW: case PHXN_SEL_DBG_PSP_CTL: case PHXN_SEL_DBG_PSP_RAW: case PHXN_SEL_DBG_WRITE_REG:
    case PHXN_SEL_DBG_VRAM_WRITE: case PHXN_SEL_DBG_DOORBELL: case PHXN_SEL_DBG_ADOPTQ: case PHXN_SEL_DBG_CLAIM:
    case PHXN_SEL_DBG_EDID:   // drives the display engine's I2C engine
        return true;
    default:
        return false;
    }
}

IOReturn IOAccelPhxNativeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector == PHXN_SEL_WAITSEQ)
        return doWaitSeq(a);

    IOLockLock(lock);
    IOReturn r;
    if (selector != PHXN_SEL_HELLO && !hello) {
        r = kIOReturnNotReady;
    } else if (writesGpu(selector) && !owner->hwWritesAllowed()) {
        r = kIOReturnUnsupported;   // unknown chip, or one whose bring-up is not implemented yet: reads only
    } else {
        switch (selector) {
        case PHXN_SEL_HELLO:     r = doHello(a); break;
        case PHXN_SEL_QUERYINFO: r = doQueryInfo(a); break;
        case PHXN_SEL_READREGS:  r = doReadRegs(a); break;
        case PHXN_SEL_BOCREATE:  r = doBoCreate(a); break;
        case PHXN_SEL_BOFREE:    r = doBoFree(a); break;
        case PHXN_SEL_BOIMPORTHOST: r = doBoImportHost(a); break;
        case PHXN_SEL_GEMVA:     r = doGemVa(a); break;
        case PHXN_SEL_CTX:       r = doCtx(a); break;
        case PHXN_SEL_SUBMIT:    r = doSubmit(a); break;
        case PHXN_SEL_DBG_SMU:   r = doDbgSmu(a); break;
        case PHXN_SEL_DBG_PSP_RING: r = doDbgPspRing(a); break;
        case PHXN_SEL_DBG_VRAM_READ: r = doDbgVramRead(a); break;
        case PHXN_SEL_DBG_PSP_SUBMIT: r = doDbgPspSubmit(a); break;
        case PHXN_SEL_DBG_FW_STAGE: r = doDbgFwStage(a); break;
        case PHXN_SEL_DBG_PSP_LOADIPFW: r = doDbgLoadIpFw(a); break;
        case PHXN_SEL_DBG_PSP_CTL: r = doDbgPspCtl(a); break;
        case PHXN_SEL_DBG_PSP_RAW: r = doDbgPspRaw(a); break;
        case PHXN_SEL_DBG_WRITE_REG: r = doDbgWriteReg(a); break;
        case PHXN_SEL_DBG_VRAM_WRITE: r = doDbgVramWrite(a); break;
        case PHXN_SEL_DBG_DOORBELL: r = doDbgDoorbell(a); break;
        case PHXN_SEL_DBG_SYSMEM: r = doDbgSysMem(a); break;
        case PHXN_SEL_DBG_ADOPTQ: r = doDbgAdoptQueue(a); break;
        case PHXN_SEL_DBG_CLAIM: r = doDbgClaim(a); break;
        case PHXN_SEL_DBG_EDID: r = doDbgEdid(a); break;
        default:                 r = kIOReturnBadArgument; break;
        }
    }
    IOLockUnlock(lock);
    return r;
}

IOReturn IOAccelPhxNativeClient::doHello(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    if (a->scalarInput[1] != 0 && a->scalarInput[1] != PHXN_HELLO_F_MINOR)
        return kIOReturnBadArgument;
    if (a->scalarInput[0] != PHXN_ABI_VERSION)
        return kIOReturnUnsupported;
    hello = true;
    a->scalarOutput[0] = PHXN_ABI_VERSION | (a->scalarInput[1] ? static_cast<uint64_t>(PHXN_ABI_MINOR) << 16 : 0);
    a->scalarOutput[1] = PHX_KEXT_BUILD;
    a->scalarOutput[2] = PHXN_MAX_IBS;
    a->scalarOutput[3] = PHXN_VMID;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doQueryInfo(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phxn_info)))
        return kIOReturnBadArgument;
    phxn_info inf;
    bzero(&inf, sizeof(inf));
    inf.abi_version = PHXN_ABI_VERSION;
    inf.kext_build = PHX_KEXT_BUILD;
    inf.reserved[2] = PHXN_ABI_MINOR;
    inf.flags = PHXN_INFO_S1B_POSITIVE | PHXN_INFO_NATIVE_BOOT;
    inf.vmid = PHXN_VMID;
    inf.gb_addr_config = owner->gbAddrConfig();   // 0 on an unknown chip: RADV then refuses the device
    inf.gc_version = owner->gcVersion();
    inf.ring_size_dw = 4096;
    inf.max_ibs = PHXN_MAX_IBS;
    inf.vram_vis_total = kVisTotal;
    inf.vram_vis_free = kVisTotal - visUsed;
    inf.vram_hi_total = kHiTotal;
    inf.vram_hi_free = kHiTotal - hiUsed;
    inf.gtt_cap = kGttCap;
    inf.gtt_used = gttUsed;
    inf.gtt_max_bo = kGttMaxBo;
    inf.seq_emitted = emitted;
    inf.seq_retired = owner->gpuReady() ? owner->gpuRetired() : emitted;
    inf.va_low_first = 0x10000;
    inf.va_low_last = 0x7FFFFFFFFFFFull;
    inf.va_high_first = 0xFFFF800000000000ull;
    inf.va_high_last = 0xFFFFFFFFFFFFFFFFull;
    inf.max_bos = PHXN_MAX_BOS;
    inf.fence_slots = PHXN_FENCE_SLOTS;
    bcopy(&inf, a->structureOutput, sizeof(inf));
    return kIOReturnSuccess;
}

static constexpr uint64_t kGoldenTscUpper = 0x16a2d;
IOReturn IOAccelPhxNativeClient::doReadRegs(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 3 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t count = a->scalarInput[1];
    if (count < 1 || count > 16 || a->scalarInput[2] != 0xFFFFFFFFull || a->structureOutputSize != count * 4 ||
        a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    // SMUIO's GOLDEN_TSC (upper 0x16a2d, lower 0x16a2e on Phoenix, SMUIO 13.0.7 base 0x16a00) is the 100 MHz counter the
    // CP stamps command buffers with: every client may read it (Mesa's AMDGPU_INFO_TIMESTAMP, which calibrates GPU
    // times against the host's; without it Metal's GPUStartTime/GPUEndTime were 0 in every app).
    const uint64_t first = a->scalarInput[0];
    const bool tsc = first >= kGoldenTscUpper && first + count <= kGoldenTscUpper + 2;
    if (!privileged && !tsc)
        return kIOReturnNotPrivileged;
    return owner->readRegs(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doBoCreate(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 4, sizeof(phxn_gem_create_in), 0))
        return kIOReturnBadArgument;
    const phxn_gem_create_in *in = static_cast<const phxn_gem_create_in *>(a->structureInput);
    uint64_t size = in->bo_size;
    if (size == 0)
        return kIOReturnBadArgument;
    size = (size + 4095) & ~4095ull;
    uint64_t align = in->alignment;
    if (align && ((align & (align - 1)) || align > (2ull << 20)))
        return kIOReturnBadArgument;
    if (align < 4096)
        align = 4096;
    const uint64_t d = in->domains, f = in->domain_flags;
    if (d == 0 || (d & ~static_cast<uint64_t>(PHX_AMDGPU_GEM_DOMAIN_GTT | PHX_AMDGPU_GEM_DOMAIN_VRAM)))
        return kIOReturnUnsupported;
    if (f & (PHX_AMDGPU_GEM_CREATE_ENCRYPTED | PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9 | PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE))
        return kIOReturnUnsupported;

    uint32_t domain, bits;
    uint64_t *pool;
    if (d & PHX_AMDGPU_GEM_DOMAIN_VRAM) {
        if (visUsed + size <= kVisTotal) {
            domain = 4;
            bits = PHXN_PLACED_CPU_MAPPABLE | PHXN_PLACED_ZEROED;
            pool = &visUsed;
        } else if ((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(f & PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) &&
                   hiUsed + size <= kHiTotal) {
            domain = 4;
            bits = PHXN_PLACED_HI_POOL | PHXN_PLACED_ZEROED;
            pool = &hiUsed;
        } else {
            OSIncrementAtomic(&gBoNoMem);
            return kIOReturnNoMemory;
        }
    } else {
        if (size > kGttMaxBo || gttUsed + size > kGttCap) {
            OSIncrementAtomic(&gBoNoMem);
            return kIOReturnNoMemory;
        }
        domain = 2;
        bits = PHXN_PLACED_ZEROED;
        pool = &gttUsed;
    }

    const uint32_t h = takeHandle();
    if (!h) {
        OSIncrementAtomic(&gBoHandlesFull);
        return kIOReturnNoResources;
    }

    // Page alignment only: the GPU maps BOs page by page, and the client picks the VA (GEMVA) with its own alignment.
    // Handed to IOBufferMemoryDescriptor, RADV's 64 KiB..2 MiB alignment made every allocation PHYSICALLY CONTIGUOUS
    // (XNU allocates an alignment above the page size with kmem_alloc_contig): once memory was fragmented, every
    // buffer of 8 MiB or more failed after a 400 ms search, and each Metal app spent ~0.8 s at launch (2026-10-21).
    (void)align;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
    if (!md)
        return kIOReturnNoMemory;
    bzero(md->getBytesNoCopy(), size);
    *pool += size;
    bos[h] = Bo{md, size, domain, bits, static_cast<uint8_t *>(md->getBytesNoCopy()), false};
    handleTaken();
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = domain;
    a->scalarOutput[3] = bits;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doBoFree(IOExternalMethodArguments *a) {
    if (!shape(a, 1, 0, 0, 0))
        return kIOReturnBadArgument;
    uint32_t h = static_cast<uint32_t>(a->scalarInput[0]);
    Bo *b = findBo(h);
    if (!b)
        return kIOReturnNotFound;
    for (int i = 0; i < kMaxMaps; i++)
        if (maps[i].used && maps[i].handle == h) {
            maps[i].used = false;
            mapsUsed--;
        }
    if (b->host)
        hostUsed -= b->size;
    else if (b->domain == 2)
        gttUsed -= b->size;
    else if (b->bits & PHXN_PLACED_HI_POOL)
        hiUsed -= b->size;
    else
        visUsed -= b->size;
    boRelease(*b);
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::boRelease(Bo &b) {
    if (b.host) {
        b.md->complete();
        hostBytes(-(SInt64)b.size);
    }
    b.md->release();
    b = Bo{};
    if (handlesUsed)
        handlesUsed--;
}

// PHXN_SEL_BOIMPORTHOST (ABI 1.9, VK_EXT_external_memory_host): a BO over the caller's own pages, wired for as long as
// it lives, GPU-mapped later through GemVa like any other. The kext reported ABI 1.12 without it, so every import
// RADV made (no-copy buffers, IOSurface textures over their surfaces' pages) failed and fell back to copies
// (2026-10-20). in: [0] host VA [1] size (both page-aligned) [2] flags 0 [3] GPU VA 0; out: [0] handle [1] size
// [2] GPU VA 0 [3] placement bits.
IOReturn IOAccelPhxNativeClient::doBoImportHost(IOExternalMethodArguments *a) {
    if (!shape(a, 4, 4, 0, 0))
        return kIOReturnBadArgument;
    const uint64_t va = a->scalarInput[0], size = a->scalarInput[1];
    if (!va || !size || (va & 4095) || (size & 4095) || a->scalarInput[2] || a->scalarInput[3] || !task)
        return kIOReturnBadArgument;
    if (size > kHostMaxBo || hostUsed + size > ((uint64_t)gHostCapMb << 20)) {
        OSIncrementAtomic(&gHostNoMem);
        return kIOReturnNoMemory;
    }
    const uint32_t h = takeHandle();
    if (!h) {
        OSIncrementAtomic(&gBoHandlesFull);
        return kIOReturnNoResources;
    }
    // WindowServer maps other processes' IOSurfaces read-only: wiring those for device writes (InOut) fails, which
    // build 39 counted 323 times (debug.phxbo_host_wire_fail) as its IOSurface textures failed (2026-11-01). Such
    // pages are wired for device reads (kIODirectionOut: memory -> device) and GPU-mapped without write permission.
    bool readOnly = false;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(va, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnNoMemory;
    IOReturn r = md->prepare();
    if (r != kIOReturnSuccess) {
        gHostWireErr = (UInt32)r;
        md->release();
        if (!gHostRo) {
            OSIncrementAtomic(&gHostWireFail);
            return r;
        }
        md = IOMemoryDescriptor::withAddressRange(va, size, kIODirectionOut, task);
        if (!md)
            return kIOReturnNoMemory;
        r = md->prepare();
        if (r != kIOReturnSuccess) {
            OSIncrementAtomic(&gHostWireFail);
            gHostWireErrRo = (UInt32)r;
            md->release();
            return r;
        }
        readOnly = true;
        OSIncrementAtomic(&gHostReadOnly);
    }
    hostUsed += size;
    hostBytes((SInt64)size);
    bos[h] = Bo{md, size, 2, 0, nullptr, true, readOnly};
    handleTaken();
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = 0;
    a->scalarOutput[3] = 0;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doGemVa(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_gem_va), 0))
        return kIOReturnBadArgument;
    const phxn_gem_va *v = static_cast<const phxn_gem_va *>(a->structureInput);
    if (v->_pad || v->vm_timeline_point || v->vm_timeline_syncobj_out || v->num_syncobj_handles ||
        v->input_fence_syncobj_handles)
        return kIOReturnBadArgument;
    if (v->handle == 0 || (v->flags & PHX_AMDGPU_VM_PAGE_PRT))
        return kIOReturnUnsupported;
    if (v->operation == PHX_AMDGPU_VA_OP_CLEAR || v->operation == PHX_AMDGPU_VA_OP_REPLACE)
        return kIOReturnUnsupported;
    if (v->operation != PHX_AMDGPU_VA_OP_MAP && v->operation != PHX_AMDGPU_VA_OP_UNMAP)
        return kIOReturnBadArgument;
    Bo *b = findBo(v->handle);
    if (!b)
        return kIOReturnNotFound;
    if (!vaCanonical(v->va_address))
        return kIOReturnBadArgument;
    const uint64_t va = v->va_address & kVaMask;
    if (((v->va_address | v->offset_in_bo | v->map_size) & 4095) || v->map_size == 0)
        return kIOReturnBadArgument;
    if (va < 0x10000 || va + v->map_size > (1ull << 48))
        return kIOReturnBadArgument;
    if (v->offset_in_bo + v->map_size > b->size)
        return kIOReturnBadArgument;

    if (v->operation == PHX_AMDGPU_VA_OP_MAP) {
        int freei = -1;
        for (int i = 0; i < kMaxMaps; i++) {
            if (!maps[i].used) {
                if (freei < 0)
                    freei = i;
                continue;
            }
            if (va < maps[i].va + maps[i].size && maps[i].va < va + v->map_size)
                return kIOReturnBadArgument;
        }
        if (freei < 0) {
            OSIncrementAtomic(&gBoMapsFull);
            return kIOReturnNoMemory;
        }
        maps[freei] = Map{true, v->handle, va, v->map_size, v->offset_in_bo, v->flags};
        notePeak(&gBoPeakMaps, ++mapsUsed);
        if (owner->gpuReady()) {
            const IOReturn r = ensureVmid() ? owner->vmMap(vmid, va, b->md, v->offset_in_bo, v->map_size, !b->readOnly) : kIOReturnNoResources;
            if (r != kIOReturnSuccess) {
                OSIncrementAtomic(&gVmMapFail);
                maps[freei].used = false;
                mapsUsed--;
                return r;
            }
        }
        return kIOReturnSuccess;
    }
    for (int i = 0; i < kMaxMaps; i++) {
        if (maps[i].used && maps[i].handle == v->handle && maps[i].va == va && maps[i].size == v->map_size) {
            maps[i].used = false;
            mapsUsed--;
            if (vmid)
                owner->vmUnmap(vmid, va, v->map_size);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn IOAccelPhxNativeClient::doCtx(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_ctx), sizeof(phxn_ctx)))
        return kIOReturnBadArgument;
    phxn_ctx in, out;
    bcopy(a->structureInput, &in, sizeof(in));
    bzero(&out, sizeof(out));
    if (in.op == PHX_AMDGPU_CTX_OP_ALLOC_CTX) {
        uint32_t id = 1;
        while (id <= PHXN_MAX_CTX && ctx[id])
            id++;
        if (id > PHXN_MAX_CTX)
            return kIOReturnNoResources;
        ctx[id] = true;
        out.op = id;
    } else if (in.op == PHX_AMDGPU_CTX_OP_FREE_CTX) {
        if (in.ctx_id == 0 || in.ctx_id > PHXN_MAX_CTX || !ctx[in.ctx_id])
            return kIOReturnNotFound;
        ctx[in.ctx_id] = false;
    } else if (in.op != PHX_AMDGPU_CTX_OP_QUERY_STATE2) {
        return kIOReturnBadArgument;
    }
    bcopy(&out, a->structureOutput, sizeof(out));
    return kIOReturnSuccess;
}

bool IOAccelPhxNativeClient::ibCovered(uint64_t vaCanon, uint64_t bytes) {
    uint64_t cur = vaCanon & kVaMask;
    const uint64_t end = cur + bytes;
    while (cur < end) {
        bool ok = false;
        for (int i = 0; i < kMaxMaps; i++) {
            const Map &mp = maps[i];
            if (mp.used && (mp.flags & PHX_AMDGPU_VM_PAGE_EXECUTABLE) && cur >= mp.va && cur < mp.va + mp.size) {
                cur = mp.va + mp.size < end ? mp.va + mp.size : end;
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

IOReturn IOAccelPhxNativeClient::doSubmit(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 0 || a->scalarOutputCount != 1 || a->structureOutputSize != 0 ||
        a->structureInputDescriptor || a->structureInputSize < 64 ||
        a->structureInputSize > 32 + 32 * PHXN_MAX_IBS)
        return kIOReturnBadArgument;
    const uint8_t *in = static_cast<const uint8_t *>(a->structureInput);
    phxn_cs_in h;
    bcopy(in, &h, sizeof(h));
    if (h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS ||
        a->structureInputSize != sizeof(h) + h.num_ibs * sizeof(phxn_cs_ib) || h.reserved != 0 ||
        (h.flags & ~PHXN_CS_HAS_FENCE))
        return kIOReturnBadArgument;
    if (h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !ctx[h.ctx_id])
        return kIOReturnNotFound;

    for (uint32_t i = 0; i < h.num_ibs; i++) {
        phxn_cs_ib ib;
        bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
        if (ib._pad || (ib.ip_type != PHX_AMDGPU_HW_IP_GFX && ib.ip_type != PHX_AMDGPU_HW_IP_COMPUTE) || ib.ip_instance != 0 || ib.ring != 0)
            return kIOReturnBadArgument;
        if (ib.ib_bytes == 0 || (ib.ib_bytes & 3) || ib.ib_bytes > 0xFFFFFu * 4)
            return kIOReturnBadArgument;
        if (!vaCanonical(ib.va_start) || (ib.va_start & 3))
            return kIOReturnBadArgument;
        if (ib.flags & (PHX_AMDGPU_IB_FLAG_CE | PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID | PHX_AMDGPU_IB_FLAGS_SECURE |
                        PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC))
            return kIOReturnUnsupported;
        if (ib.flags & ~static_cast<uint32_t>(PHX_AMDGPU_IB_FLAG_PREAMBLE | PHX_AMDGPU_IB_FLAG_PREEMPT |
                                              PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE))
            return kIOReturnBadArgument;
        if (!ibCovered(ib.va_start, ib.ib_bytes))
            return kIOReturnBadArgument;
    }

    Bo *fb = nullptr;
    if (h.flags & PHXN_CS_HAS_FENCE) {
        fb = findBo(h.fence_handle);
        if (!fb)
            return kIOReturnNotFound;
        if (!fb->kva || (h.fence_offset & 7) || static_cast<uint64_t>(h.fence_offset) + 8 > fb->size)
            return kIOReturnBadArgument;   // the kernel writes the fence: not into a host import
    } else if (h.fence_handle || h.fence_offset) {
        return kIOReturnBadArgument;
    }

    if (owner->gpuReady()) {
        // E8: real execution on the adopted compute queue, IBs run in VMID 8
        uint64_t va[PHXN_MAX_IBS];
        uint32_t dw[PHXN_MAX_IBS];
        for (uint32_t i = 0; i < h.num_ibs; i++) {
            phxn_cs_ib ib;
            bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
            va[i] = ib.va_start & kVaMask;
            dw[i] = ib.ib_bytes / 4;
        }
        // log the IBs RADV sends (first 96 dwords each) for bring-up: a client's first submissions only, as every
        // WindowServer frame dumped its IBs and the log held nothing else (2026-10-20)
        if (owner->debugEnabled() && emitted < 4) {
            for (uint32_t i = 0; i < h.num_ibs; i++) {
                for (int m = 0; m < kMaxMaps; m++) {
                    if (!maps[m].used || va[i] < maps[m].va || va[i] >= maps[m].va + maps[m].size)
                        continue;
                    Bo *bo = findBo(maps[m].handle);
                    if (!bo || !bo->kva)
                        break;
                    const uint32_t *d = reinterpret_cast<const uint32_t *>(bo->kva + maps[m].off + (va[i] - maps[m].va));
                    const uint32_t cnt = dw[i] < 96 ? dw[i] : 96;
                    for (uint32_t j = 0; j < cnt; j += 8)
                        IOLog("PhxIB %u.%u +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)(emitted + 1), i, j,
                              d[j], j + 1 < cnt ? d[j + 1] : 0, j + 2 < cnt ? d[j + 2] : 0, j + 3 < cnt ? d[j + 3] : 0,
                              j + 4 < cnt ? d[j + 4] : 0, j + 5 < cnt ? d[j + 5] : 0, j + 6 < cnt ? d[j + 6] : 0,
                              j + 7 < cnt ? d[j + 7] : 0);
                    break;
                }
            }
        }
        flushBos();
        uint64_t seq = 0;
        phxn_cs_ib ib0;
        bcopy(in + sizeof(h), &ib0, sizeof(ib0));
        if (!ensureVmid())
            return kIOReturnNoResources;
        const IOReturn r = owner->gpuSubmit(vmid, ib0.ip_type == PHX_AMDGPU_HW_IP_GFX, va, dw, h.num_ibs, &seq);
        if (r != kIOReturnSuccess)
            return r;
        lastSeq = seq;
        emitted = seq;   // externalMethod already holds `lock`
        if (fb) {
            pendFenceHandle = h.fence_handle;
            pendFenceOffset = h.fence_offset;
            pendFenceSeq = seq;
        }
        a->scalarOutput[0] = seq;
        return kIOReturnSuccess;
    }
    // E1: no GPU behind the call; the submission retires at once, as in the mock.
    const uint64_t seq = ++emitted;
    if (fb)
        bcopy(&seq, fb->kva + h.fence_offset, sizeof(seq));
    a->scalarOutput[0] = seq;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doDbgSmu(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 2, 0, 0) || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    switch (a->scalarInput[0]) {
    case PHX_PPSMC_TestMessage:
    case PHX_PPSMC_GetPmfwVersion:
    case PHX_PPSMC_GetDriverIfVersion:
    case PHX_PPSMC_GetEnabledSmuFeatures:
    case PHX_PPSMC_AllowGfxOff:
    case PHX_PPSMC_DisallowGfxOff:
    case PHX_PPSMC_EnableGfxImu:
        break;
    default:
        return kIOReturnNotPermitted;
    }
    uint32_t resp = 0, after = 0;
    IOReturn r = owner->smuMessage(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                   &resp, &after);
    a->scalarOutput[0] = resp;
    a->scalarOutput[1] = after;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRing(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRing(static_cast<uint32_t>(a->scalarInput[0]), a->scalarInput[1], out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramRead(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 2 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t off = a->scalarInput[0], count = a->scalarInput[1];
    if (count < 1 || count > 256 || off > 0xFFFFFFFFull || a->structureOutputSize != count * 4)
        return kIOReturnBadArgument;
    return owner->vramRead(static_cast<uint32_t>(off), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doDbgPspSubmit(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspSubmit(static_cast<uint32_t>(a->scalarInput[0]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgFwStage(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 1 || a->scalarOutputCount != 0 || a->structureOutputSize != 0)
        return kIOReturnBadArgument;
    if (a->structureInputDescriptor || !a->structureInput || a->structureInputSize == 0 ||
        a->structureInputSize > 4096 || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->fwStage(static_cast<uint32_t>(a->scalarInput[0]), a->structureInput, a->structureInputSize);
}

IOReturn IOAccelPhxNativeClient::doDbgLoadIpFw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull ||
        a->scalarInput[2] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->loadIpFw(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                 static_cast<uint32_t>(a->scalarInput[2]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspCtl(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspCtl(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRaw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 7, 4, 0, 0))
        return kIOReturnBadArgument;
    uint32_t w[6];
    for (int i = 0; i < 6; i++)
        w[i] = static_cast<uint32_t>(a->scalarInput[i + 1]);
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRaw(static_cast<uint32_t>(a->scalarInput[0]), w, out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgWriteReg(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 1, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint32_t rb = 0;
    IOReturn r = owner->writeReg(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), &rb);
    a->scalarOutput[0] = rb;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgEdid(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 0, 2, 0, 256))
        return kIOReturnBadArgument;
    uint8_t edid[256];
    bzero(edid, sizeof(edid));
    uint32_t len = 0, result = 0;
    IOReturn r = owner->readEdid(edid, &len, &result);
    a->scalarOutput[0] = result;
    a->scalarOutput[1] = len;
    bcopy(edid, a->structureOutput, sizeof(edid));
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramWrite(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 0, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->vramWrite(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]));
}

IOReturn IOAccelPhxNativeClient::doDbgDoorbell(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->doorbellWrite(a->scalarInput[0], a->scalarInput[1] | (a->scalarInput[2] << 32));
}

IOReturn IOAccelPhxNativeClient::doDbgSysMem(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 2, 0, 0))
        return kIOReturnBadArgument;
    return owner->sysMem(a->scalarInput[0], a->scalarInput[1], a->scalarInput[2], &a->scalarOutput[0], &a->scalarOutput[1]);
}

IOReturn IOAccelPhxNativeClient::doDbgAdoptQueue(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 6, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->adoptQueue(a->scalarInput);
}

IOReturn IOAccelPhxNativeClient::doDbgClaim(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 0, 0, 0) || a->scalarInput[0] != 1)
        return kIOReturnBadArgument;
    return owner->claimAccel();
}

// Write back and invalidate the CPU caches over every BO: CPU-written data must be in memory before the GPU reads
// it, and the CPU must not read stale lines after the GPU wrote. Coarse: WindowServer's BOs (full-screen surfaces
// among them) made this its hottest loop, hence debug.phxbos_flush.
void IOAccelPhxNativeClient::flushBos() {
    const int mode = gBosFlush;
    if (!mode)
        return;
    uint64_t bytes = 0;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++) {
        if (!bos[h].kva)   // host imports: the CPU's own pages, which the GPU reads and writes snooped
            continue;
        const uint8_t *p = bos[h].kva;
        if (mode == 1) {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflushopt (%0)" ::"r"(p + o) : "memory");
        } else {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflush (%0)" ::"r"(p + o) : "memory");
        }
        bytes += bos[h].size;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    OSAddAtomic64((SInt64)bytes, &gBosFlushBytes);
}

IOReturn IOAccelPhxNativeClient::doWaitSeq(IOExternalMethodArguments *a) {
    if (!hello)
        return kIOReturnNotReady;
    if (!shape(a, 3, 3, 0, 0))
        return kIOReturnBadArgument;
    IOLockLock(lock);
    uint64_t target = a->scalarInput[0];
    const uint64_t em = emitted;
    IOLockUnlock(lock);
    if (target == PHXN_SEQ_LAST)
        target = em;
    if (target > em)
        return kIOReturnBadArgument;
    if (owner->gpuReady()) {
        // E8: poll the RELEASE_MEM fence the adopted queue writes after each submission
        uint64_t timeout = a->scalarInput[1];
        if (timeout > PHXN_WAIT_CAP_NS)
            timeout = PHXN_WAIT_CAP_NS;
        uint64_t retired = owner->gpuRetired();
        for (uint64_t waited = 0; retired < target && waited < timeout; waited += 100000) {
            IODelay(100);
            retired = owner->gpuRetired();
        }
        if (retired >= target) {
            // drop CPU cache lines so the CPU sees what the GPU wrote; under the lock, as another thread of the same
            // client (WindowServer) freed a BO mid-flush and the walk dereferenced its NULL descriptor (panic 2026-10-20)
            IOLockLock(lock);
            flushBos();
            if (pendFenceSeq && pendFenceSeq <= retired) {
                Bo *fb = findBo(pendFenceHandle);
                if (fb && fb->kva && pendFenceOffset + 8 <= fb->size)
                    bcopy(&pendFenceSeq, fb->kva + pendFenceOffset, 8);
                pendFenceSeq = 0;
            }
            IOLockUnlock(lock);
        }
        a->scalarOutput[0] = retired < target;
        a->scalarOutput[1] = retired;
        a->scalarOutput[2] = em;
        return kIOReturnSuccess;
    }
    a->scalarOutput[0] = 0;   // not busy: E1 retires every submission at once
    a->scalarOutput[1] = em;
    a->scalarOutput[2] = em;
    return kIOReturnSuccess;
}
