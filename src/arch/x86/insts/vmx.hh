#ifndef __ARCH_X86_VMX_HH__
#define __ARCH_X86_VMX_HH__

#include <cstdint>
#include <map>

#include "arch/x86/regs/segment.hh"
#include "arch/x86/vmcs.hh"
#include "base/types.hh"
#include "cpu/null_static_inst.hh"
#include "mem/request.hh"
#include "sim/faults.hh"
#include "sim/serialize.hh"

namespace gem5
{
class ExecContext;
class ThreadContext;

namespace X86ISA
{

Fault vmxMemoryOperandFault(ThreadContext *tc, Addr linear,
        size_t size, Request::Flags operandFlags);

// Intel SDM Vol. 3 Appendix C basic VM-exit reasons.
enum class VmxExitReason : uint32_t
{
    ExceptionOrNmi = 0,
    ExternalInterrupt = 1,
    TripleFault = 2,
    InitSignal = 3,
    StartupIpi = 4,
    IoSmi = 5,
    OtherSmi = 6,
    InterruptWindow = 7,
    NmiWindow = 8,
    TaskSwitch = 9,
    Cpuid = 10,
    Getsec = 11,
    Hlt = 12,
    Invd = 13,
    Invlpg = 14,
    Rdpmc = 15,
    Rdtsc = 16,
    Rsm = 17,
    Vmcall = 18,
    Vmclear = 19,
    Vmlaunch = 20,
    Vmptrld = 21,
    Vmptrst = 22,
    Vmread = 23,
    Vmresume = 24,
    Vmwrite = 25,
    Vmxoff = 26,
    Vmxon = 27,
    ControlRegisterAccess = 28,
    MovDr = 29,
    IoInstruction = 30,
    Rdmsr = 31,
    Wrmsr = 32,
    VmEntryInvalidGuestState = 33,
    VmEntryMsrLoad = 34,
    Mwait = 36,
    MonitorTrapFlag = 37,
    Monitor = 39,
    Pause = 40,
    VmEntryMachineCheck = 41,
    TprBelowThreshold = 43,
    ApicAccess = 44,
    VirtualizedEoi = 45,
    AccessGdtrOrIdtr = 46,
    AccessLdtrOrTr = 47,
    EptViolation = 48,
    EptMisconfiguration = 49,
    Invept = 50,
    Rdtscp = 51,
    VmxPreemptionTimerExpired = 52,
    Invvpid = 53,
    Wbinvd = 54,
    Xsetbv = 55,
    ApicWrite = 56,
    Rdrand = 57,
    Invpcid = 58,
    Vmfunc = 59,
    Encls = 60,
    Rdseed = 61,
    PageModificationLogFull = 62,
    Xsaves = 63,
    Xrstors = 64,
};

// Intel SDM Vol. 3 Appendix C VM-instruction error numbers.
enum class VmxInstructionError : uint32_t
{
    VmcallInRoot = 1,
    VmclearInvalidPhysicalAddress = 2,
    VmclearWithVmxonPointer = 3,
    VmlaunchNonClearVmcs = 4,
    VmresumeNonLaunchedVmcs = 5,
    VmresumeAfterVmxoff = 6,
    VmEntryInvalidControlFields = 7,
    VmEntryInvalidHostState = 8,
    VmptrldInvalidPhysicalAddress = 9,
    VmptrldWithVmxonPointer = 10,
    VmptrldIncorrectVmcsRevision = 11,
    UnsupportedVmcsComponent = 12,
    VmwriteReadOnlyVmcsComponent = 13,
    VmxonInRoot = 15,
    VmEntryInvalidExecutiveVmcsPointer = 16,
    VmEntryNonLaunchedExecutiveVmcs = 17,
    VmEntryExecutiveVmcsPointerNotVmxonPointer = 18,
    VmcallNonClearVmcs = 19,
    VmcallInvalidVmExitControls = 20,
    VmcallIncorrectMsegRevision = 22,
    VmxoffUnderDualMonitorTreatment = 23,
    VmcallInvalidSmmMonitorFeatures = 24,
    VmEntryInvalidExecutiveVmcsVmExecutionControls = 25,
    VmEntryEventsBlockedByMovSs = 26,
    InveptInvvpidInvalidOperand = 28,
};

enum class VmxInterruptionType : uint8_t
{
    ExternalInterrupt = 0,
    Nmi = 2,
    HardwareException = 3,
    SoftwareInterrupt = 4,
    PrivilegedSoftwareException = 5,
    SoftwareException = 6,
};

enum class VmxCrAccessType : uint8_t
{
    MovToCr = 0,
    MovFromCr = 1,
    Clts = 2,
    Lmsw = 3,
};

struct VmxExitInfo
{
    VmxExitReason reason = VmxExitReason::ExceptionOrNmi;
    bool vmEntryFailure = false;

    bool hasQualification = false;
    uint64_t qualification = 0;

    bool hasInstructionLength = false;
    uint32_t instructionLength = 0;

    bool hasInstructionInfo = false;
    uint32_t instructionInfo = 0;

    bool hasInterruptionInfo = false;
    uint32_t interruptionInfo = 0;

    bool hasInterruptionErrorCode = false;
    uint32_t interruptionErrorCode = 0;

    bool hasGuestLinearAddress = false;
    uint64_t guestLinearAddress = 0;

    bool hasGuestPhysicalAddress = false;
    uint64_t guestPhysicalAddress = 0;
};

class VmxExitFault : public FaultBase
{
  private:
    VmxExitInfo exitInfo;

  public:
    explicit VmxExitFault(const VmxExitInfo &info) : exitInfo(info) {}

    const char *name() const override { return "vmx_exit"; }
    void invoke(ThreadContext *tc, const StaticInstPtr &inst =
            nullStaticInstPtr) override;
};

enum class VmxStatus : uint8_t
{
    Success,
    VmFailInvalid,
    VmFailValid
};

struct VmxResult
{
    Fault fault = NoFault;
    VmxStatus status = VmxStatus::Success;
    uint32_t instructionError = 0;
    bool redirectsNextPc = false;
    Addr nextPc = 0;

    bool
    succeeded() const
    {
        return fault == NoFault && status == VmxStatus::Success;
    }

    static VmxResult success()
    {
        return {};
    }

    static VmxResult successRedirect(Addr next_pc)
    {
        VmxResult result;
        result.redirectsNextPc = true;
        result.nextPc = next_pc;
        return result;
    }

    static VmxResult failInvalid()
    {
        VmxResult result;
        result.status = VmxStatus::VmFailInvalid;
        return result;
    }

    static VmxResult failValid(uint32_t error)
    {
        VmxResult result;
        result.status = VmxStatus::VmFailValid;
        result.instructionError = error;
        return result;
    }

    static VmxResult propagateFault(const Fault &fault)
    {
        VmxResult result;
        result.fault = fault;
        return result;
    }
};

class VmxState
{
  private:
    using VmcsMap = std::map<Addr, Vmcs>;
    static constexpr Addr InvalidVmcsPointer = ~Addr(0);

    bool vmxActive = false;
    bool inVmxNonRoot = false;
    Addr vmxonRegion = 0;
    Addr currentVmcsPtr = InvalidVmcsPointer;
    VmcsMap vmcsRegions;

    struct VmEntryValidationResult
    {
        bool valid = true;
        bool vmEntryFailure = false;
        VmxInstructionError instructionError =
            VmxInstructionError::VmEntryInvalidControlFields;
        VmxExitReason exitReason = VmxExitReason::VmEntryInvalidGuestState;
    };

    Vmcs *findVmcs(Addr regionPtr);
    const Vmcs *findVmcs(Addr regionPtr) const;
    Vmcs *currentVmcs();
    const Vmcs *currentVmcs() const;

    VmEntryValidationResult validateVmEntry(ThreadContext *tc,
            Vmcs &vmcs) const;
    bool loadGuestState(ThreadContext *tc, Vmcs &vmcs, Addr &guestRip) const;
    bool loadHostState(ThreadContext *tc, Vmcs &vmcs, Addr &hostRip) const;
    void saveGuestState(ThreadContext *tc, Vmcs &vmcs) const;
    VmxResult failVmEntry(ThreadContext *tc, Vmcs &vmcs,
            VmxExitReason reason);
    VmxResult vmEntry(ExecContext *xc, bool launch, uint8_t instructionSize);

  public:
    bool active() const { return vmxActive; }
    bool nonRootActive() const { return inVmxNonRoot; }
    Addr vmxonPtr() const { return vmxonRegion; }
    Addr currentVmcsPointer() const { return currentVmcsPtr; }

    VmxResult vmxon(ExecContext *xc, Addr operandEA,
            Request::Flags operandFlags, uint8_t instructionSize);
    VmxResult vmxoff(ExecContext *xc, uint8_t instructionSize);
    VmxResult vmclear(ExecContext *xc, Addr operandEA,
            Request::Flags operandFlags, uint8_t instructionSize);
    VmxResult vmptrld(ExecContext *xc, Addr operandEA,
            Request::Flags operandFlags, uint8_t instructionSize);
    VmxResult vmptrst(ExecContext *xc, Addr operandEA,
            Request::Flags operandFlags, uint8_t instructionSize);
    VmxResult vmlaunch(ExecContext *xc, uint8_t instructionSize);
    VmxResult vmresume(ExecContext *xc, uint8_t instructionSize);
    VmxResult vmcall(ExecContext *xc, uint8_t instructionSize);

    VmxResult vmread(ExecContext *xc, Vmcs::RawEncoding encoding,
            uint64_t &value, uint8_t instructionSize);
    VmxResult vmwritePrecheck(ExecContext *xc, uint8_t instructionSize);
    VmxResult vmwrite(ExecContext *xc, Vmcs::RawEncoding encoding,
            uint64_t value, uint8_t instructionSize);

    bool shouldExitOnException(uint8_t vector, uint64_t errorCode) const;
    bool shouldExitOnExternalInterrupt() const;
    bool shouldExitOnNmi() const;
    bool hltCausesExit() const;
    bool invlpgCausesExit() const;
    bool movDrCausesExit() const;
    bool rdmsrCausesExit(ThreadContext *tc, uint32_t msr) const;
    bool wrmsrCausesExit(ThreadContext *tc, uint32_t msr) const;
    bool ioInstructionCausesExit(ThreadContext *tc, uint16_t port,
            size_t size) const;
    bool controlRegisterAccessCausesExit(uint8_t cr, VmxCrAccessType type,
            uint64_t value = 0) const;
    uint64_t controlRegisterReadValue(uint8_t cr, uint64_t liveValue) const;
    uint64_t controlRegisterWriteValue(uint8_t cr, uint64_t requestedValue,
            uint64_t liveValue) const;
    bool controlRegisterWriteAllowed(ThreadContext *tc, uint8_t cr,
            uint64_t value) const;

    VmxResult vmexitInstruction(ExecContext *xc, VmxExitReason reason,
            uint8_t instructionSize, uint64_t qualification = 0,
            uint32_t instructionInfo = 0);
    VmxResult controlRegisterExit(ExecContext *xc, uint8_t cr,
            VmxCrAccessType type, uint8_t gpr, uint64_t value,
            uint8_t instructionSize);
    VmxResult debugRegisterExit(ExecContext *xc, uint8_t dr, bool fromDr,
            uint8_t gpr, uint8_t instructionSize);
    bool vmexitEvent(ThreadContext *tc, const VmxExitInfo &exitInfo);
    bool vmexit(ThreadContext *tc, const VmxExitInfo &exitInfo,
            Addr *hostRip = nullptr);

    Fault ioExitFault(bool read, uint16_t port, size_t size) const;
    Fault msrExitFault(bool read, uint32_t msr) const;

    void serialize(CheckpointOut &cp) const;
    void unserialize(CheckpointIn &cp);
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_VMX_HH__
