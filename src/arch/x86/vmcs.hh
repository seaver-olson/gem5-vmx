#ifndef __ARCH_X86_VMCS_HH__
#define __ARCH_X86_VMCS_HH__

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "base/bitfield.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "sim/serialize.hh"

namespace gem5
{
namespace X86ISA
{

// This is gem5's shadow model of a VMCS region, not the hardware region
// itself. Field encoding follows Intel SDM Vol. 3C, Section 26.11.2.
class Vmcs
{
  public:
    using RawEncoding = uint64_t;
    using Encoding = uint32_t;
    using FieldMap = std::map<Encoding, uint64_t>;

    static constexpr size_t VmcsRegionSize = 4096;
    static constexpr RawEncoding VmcsEncodingMask = 0x7fff;
    static constexpr Encoding VmcsEncodingReservedBit = 1u << 12;

    enum class Field : Encoding
    {
        VirtualProcessorId = 0x0000,
        PostedInterruptNotificationVector = 0x0002,
        EptpIndex = 0x0004,
        HlatPrefixSize = 0x0006,
        LastPidPointerIndex = 0x0008,

        GuestEsSelector = 0x0800,
        GuestCsSelector = 0x0802,
        GuestSsSelector = 0x0804,
        GuestDsSelector = 0x0806,
        GuestFsSelector = 0x0808,
        GuestGsSelector = 0x080A,
        GuestLdtrSelector = 0x080C,
        GuestTrSelector = 0x080E,
        GuestInterruptStatus = 0x0810,
        PmlIndex = 0x0812,
        GuestUinv = 0x0814,

        HostEsSelector = 0x0C00,
        HostCsSelector = 0x0C02,
        HostSsSelector = 0x0C04,
        HostDsSelector = 0x0C06,
        HostFsSelector = 0x0C08,
        HostGsSelector = 0x0C0A,
        HostTrSelector = 0x0C0C,

        IoBitmapA = 0x2000,
        IoBitmapB = 0x2002,
        MsrBitmap = 0x2004,
        VmExitMsrStoreAddress = 0x2006,
        VmExitMsrLoadAddress = 0x2008,
        VmEntryMsrLoadAddress = 0x200A,
        ExecutiveVmcsPointer = 0x200C,
        PmlAddress = 0x200E,
        TscOffset = 0x2010,
        VirtualApicAddress = 0x2012,
        ApicAccessAddress = 0x2014,
        PostedInterruptDescriptorAddress = 0x2016,
        VmFunctionControls = 0x2018,
        EptPointer = 0x201A,
        EoiExitBitmap0 = 0x201C,
        EoiExitBitmap1 = 0x201E,
        EoiExitBitmap2 = 0x2020,
        EoiExitBitmap3 = 0x2022,
        EptpListAddress = 0x2024,
        VmreadBitmapAddress = 0x2026,
        VmwriteBitmapAddress = 0x2028,
        VirtualizationExceptionInformationAddress = 0x202A,
        XssExitingBitmap = 0x202C,
        EnclsExitingBitmap = 0x202E,
        SubPagePermissionTablePointer = 0x2030,
        TscMultiplier = 0x2032,
        TertiaryProcessorBasedVmExecutionControls = 0x2034,
        LowPasidDirectoryAddress = 0x2038,
        HighPasidDirectoryAddress = 0x203A,
        SeamSharedEptPointer = 0x203C,
        PconfigExitingBitmap = 0x203E,
        HlatPointer = 0x2040,
        PidPointerTableAddress = 0x2042,
        SecondaryVmExitControls = 0x2044,
        Ia32SpecCtrlMask = 0x204A,
        Ia32SpecCtrlShadow = 0x204C,
        InjectedEventData = 0x2052,

        GuestPhysicalAddress = 0x2400,
        MsrData = 0x2402,
        OriginalEventData = 0x2404,

        VmcsLinkPointer = 0x2800,
        GuestIa32Debugctl = 0x2802,
        GuestIa32Pat = 0x2804,
        GuestIa32Efer = 0x2806,
        GuestIa32PerfGlobalCtrl = 0x2808,
        GuestPdpte0 = 0x280A,
        GuestPdpte1 = 0x280C,
        GuestPdpte2 = 0x280E,
        GuestPdpte3 = 0x2810,
        GuestIa32Bndcfgs = 0x2812,
        GuestIa32RtitCtl = 0x2814,
        GuestIa32LbrCtl = 0x2816,
        GuestIa32Pkrs = 0x2818,
        GuestIa32FredConfig = 0x281A,
        GuestIa32FredRsp1 = 0x281C,
        GuestIa32FredRsp2 = 0x281E,
        GuestIa32FredRsp3 = 0x2820,
        GuestIa32FredStklvls = 0x2822,
        GuestIa32FredSsp1 = 0x2824,
        GuestIa32FredSsp2 = 0x2826,
        GuestIa32FredSsp3 = 0x2828,

        HostIa32Pat = 0x2C00,
        HostIa32Efer = 0x2C02,
        HostIa32PerfGlobalCtrl = 0x2C04,
        HostIa32Pkrs = 0x2C06,
        HostIa32FredConfig = 0x2C08,
        HostIa32FredRsp1 = 0x2C0A,
        HostIa32FredRsp2 = 0x2C0C,
        HostIa32FredRsp3 = 0x2C0E,
        HostIa32FredStklvls = 0x2C10,
        HostIa32FredSsp1 = 0x2C12,
        HostIa32FredSsp2 = 0x2C14,
        HostIa32FredSsp3 = 0x2C16,

        PinBasedVmExecControl = 0x4000,
        CpuBasedVmExecControl = 0x4002,
        ExceptionBitmap = 0x4004,
        PageFaultErrorCodeMask = 0x4006,
        PageFaultErrorCodeMatch = 0x4008,
        Cr3TargetCount = 0x400A,
        VmExitControls = 0x400C,
        VmExitMsrStoreCount = 0x400E,
        VmExitMsrLoadCount = 0x4010,
        VmEntryControls = 0x4012,
        VmEntryMsrLoadCount = 0x4014,
        VmEntryIntrInfoField = 0x4016,
        VmEntryExceptionErrorCode = 0x4018,
        VmEntryInstructionLen = 0x401A,
        TprThreshold = 0x401C,
        SecondaryVmExecControl = 0x401E,
        PleGap = 0x4020,
        PleWindow = 0x4022,
        InstructionTimeoutControl = 0x4024,
        SeamGuestKeyId = 0x4026,

        VmInstructionError = 0x4400,
        VmExitReason = 0x4402,
        VmExitInterruptionInfo = 0x4404,
        VmExitInterruptionErrorCode = 0x4406,
        IdtVectoringInfoField = 0x4408,
        IdtVectoringErrorCode = 0x440A,
        VmExitInstructionLen = 0x440C,
        VmxInstructionInfo = 0x440E,

        GuestEsLimit = 0x4800,
        GuestCsLimit = 0x4802,
        GuestSsLimit = 0x4804,
        GuestDsLimit = 0x4806,
        GuestFsLimit = 0x4808,
        GuestGsLimit = 0x480A,
        GuestLdtrLimit = 0x480C,
        GuestTrLimit = 0x480E,
        GuestGdtrLimit = 0x4810,
        GuestIdtrLimit = 0x4812,
        GuestEsAccessRights = 0x4814,
        GuestCsAccessRights = 0x4816,
        GuestSsAccessRights = 0x4818,
        GuestDsAccessRights = 0x481A,
        GuestFsAccessRights = 0x481C,
        GuestGsAccessRights = 0x481E,
        GuestLdtrAccessRights = 0x4820,
        GuestTrAccessRights = 0x4822,
        GuestInterruptibilityState = 0x4824,
        GuestActivityState = 0x4826,
        GuestSmbase = 0x4828,
        GuestSysenterCs = 0x482A,
        VmxPreemptionTimerValue = 0x482E,

        HostIa32SysenterCs = 0x4C00,

        Cr0GuestHostMask = 0x6000,
        Cr4GuestHostMask = 0x6002,
        Cr0ReadShadow = 0x6004,
        Cr4ReadShadow = 0x6006,
        Cr3TargetValue0 = 0x6008,
        Cr3TargetValue1 = 0x600A,
        Cr3TargetValue2 = 0x600C,
        Cr3TargetValue3 = 0x600E,

        ExitQualification = 0x6400,
        IoRcx = 0x6402,
        IoRsi = 0x6404,
        IoRdi = 0x6406,
        IoRip = 0x6408,
        GuestLinearAddress = 0x640A,

        GuestCr0 = 0x6800,
        GuestCr3 = 0x6802,
        GuestCr4 = 0x6804,
        GuestEsBase = 0x6806,
        GuestCsBase = 0x6808,
        GuestSsBase = 0x680A,
        GuestDsBase = 0x680C,
        GuestFsBase = 0x680E,
        GuestGsBase = 0x6810,
        GuestLdtrBase = 0x6812,
        GuestTrBase = 0x6814,
        GuestGdtrBase = 0x6816,
        GuestIdtrBase = 0x6818,
        GuestDr7 = 0x681A,
        GuestRsp = 0x681C,
        GuestRip = 0x681E,
        GuestRflags = 0x6820,
        GuestPendingDbgExceptions = 0x6822,
        GuestSysenterEsp = 0x6824,
        GuestSysenterEip = 0x6826,
        GuestIa32SCet = 0x6828,
        GuestSsp = 0x682A,
        GuestIa32InterruptSspTableAddr = 0x682C,

        HostCr0 = 0x6C00,
        HostCr3 = 0x6C02,
        HostCr4 = 0x6C04,
        HostFsBase = 0x6C06,
        HostGsBase = 0x6C08,
        HostTrBase = 0x6C0A,
        HostGdtrBase = 0x6C0C,
        HostIdtrBase = 0x6C0E,
        HostIa32SysenterEsp = 0x6C10,
        HostIa32SysenterEip = 0x6C12,
        HostRsp = 0x6C14,
        HostRip = 0x6C16,
        HostIa32SCet = 0x6C18,
        HostSsp = 0x6C1A,
        HostIa32InterruptSspTableAddr = 0x6C1C,
    };

    static constexpr Encoding VmInstructionError =
        static_cast<Encoding>(Field::VmInstructionError);

    enum class LaunchState : uint8_t
    {
        Clear,
        Launched
    };

    enum class AccessType : uint8_t
    {
        Full,
        High
    };

    enum class FieldWidth : uint8_t
    {
        U16,
        U32,
        U64,
        Natural
    };

    enum class VmcsFieldType : uint8_t
    {
        Control,
        VmExitInfo,
        GuestState,
        HostState
    };

    enum class VmcsFieldGroup : uint8_t
    {
        GuestState,
        HostState,
        VmExecutionControl,
        VmExitControl,
        VmEntryControl,
        VmExitInformation,
    };

    static constexpr Encoding
    encodingOf(Field field)
    {
        return static_cast<Encoding>(field);
    }

    static constexpr FieldWidth
    widthFromEncoding(Encoding encoding)
    {
        switch ((encoding >> 13) & 0x3) {
          case 0:
            return FieldWidth::U16;
          case 1:
            return FieldWidth::U64;
          case 2:
            return FieldWidth::U32;
          case 3:
            return FieldWidth::Natural;
        }

        return FieldWidth::Natural;
    }

    static constexpr VmcsFieldType
    typeFromEncoding(Encoding encoding)
    {
        switch ((encoding >> 10) & 0x3) {
          case 0:
            return VmcsFieldType::Control;
          case 1:
            return VmcsFieldType::VmExitInfo;
          case 2:
            return VmcsFieldType::GuestState;
          case 3:
            return VmcsFieldType::HostState;
        }

        return VmcsFieldType::Control;
    }

    struct FieldInfo
    {
        Field field;
        const char *name;
        VmcsFieldGroup group;
        bool writable;

        constexpr Encoding
        encoding() const
        {
            return encodingOf(field);
        }

        constexpr FieldWidth
        width() const
        {
            return widthFromEncoding(encoding());
        }

        constexpr VmcsFieldType
        type() const
        {
            return typeFromEncoding(encoding());
        }
    };

    class FieldEncoding
    {
      private:
        Encoding rawEncoding = 0;
        Field fieldId = Field::VirtualProcessorId;
        AccessType accessType = AccessType::Full;

      public:
        constexpr FieldEncoding() = default;

        constexpr FieldEncoding(Encoding raw_encoding, Field field,
                AccessType access_type)
            : rawEncoding(raw_encoding),
              fieldId(field),
              accessType(access_type)
        {
        }

        static constexpr FieldEncoding
        full(Field field)
        {
            return FieldEncoding(encodingOf(field), field, AccessType::Full);
        }

        constexpr Encoding
        raw() const
        {
            return rawEncoding;
        }

        constexpr Field
        field() const
        {
            return fieldId;
        }

        constexpr Encoding
        fullEncoding() const
        {
            return encodingOf(fieldId);
        }

        constexpr AccessType
        access() const
        {
            return accessType;
        }

        constexpr bool
        highAccess() const
        {
            return accessType == AccessType::High;
        }
    };

    class VmcsHeader
    {
      public:
        uint32_t revisionId = 0;
        uint32_t abortIndicator = 0;
    };

  private:
    Addr regionPointer = 0;
    VmcsHeader header = {};
    LaunchState launchState = LaunchState::Clear;
    FieldMap fields;

  public:
    static const FieldInfo *
    lookupField(Field field_id)
    {
        using F = Field;
        using Group = VmcsFieldGroup;

#define VMCS_FIELD(_field, _group, _writable) \
            {F::_field, #_field, Group::_group, _writable}

        static constexpr FieldInfo supportedFields[] = {
            // Keep this list aligned with the VMX capabilities advertised in
            // ISA::clear() and the fields the VM-entry/VM-exit path consumes.
            VMCS_FIELD(GuestEsSelector, GuestState, true),
            VMCS_FIELD(GuestCsSelector, GuestState, true),
            VMCS_FIELD(GuestSsSelector, GuestState, true),
            VMCS_FIELD(GuestDsSelector, GuestState, true),
            VMCS_FIELD(GuestFsSelector, GuestState, true),
            VMCS_FIELD(GuestGsSelector, GuestState, true),
            VMCS_FIELD(GuestLdtrSelector, GuestState, true),
            VMCS_FIELD(GuestTrSelector, GuestState, true),

            VMCS_FIELD(HostEsSelector, HostState, true),
            VMCS_FIELD(HostCsSelector, HostState, true),
            VMCS_FIELD(HostSsSelector, HostState, true),
            VMCS_FIELD(HostDsSelector, HostState, true),
            VMCS_FIELD(HostFsSelector, HostState, true),
            VMCS_FIELD(HostGsSelector, HostState, true),
            VMCS_FIELD(HostTrSelector, HostState, true),

            VMCS_FIELD(IoBitmapA, VmExecutionControl, true),
            VMCS_FIELD(IoBitmapB, VmExecutionControl, true),
            VMCS_FIELD(MsrBitmap, VmExecutionControl, true),

            VMCS_FIELD(GuestPhysicalAddress, VmExitInformation, false),

            VMCS_FIELD(GuestIa32Efer, GuestState, true),

            VMCS_FIELD(HostIa32Efer, HostState, true),

            VMCS_FIELD(PinBasedVmExecControl, VmExecutionControl, true),
            VMCS_FIELD(CpuBasedVmExecControl, VmExecutionControl, true),
            VMCS_FIELD(VmExitControls, VmExitControl, true),
            VMCS_FIELD(VmEntryControls, VmEntryControl, true),

            VMCS_FIELD(VmInstructionError, VmExitInformation, false),
            VMCS_FIELD(VmExitReason, VmExitInformation, false),
            VMCS_FIELD(VmExitInterruptionInfo, VmExitInformation, false),
            VMCS_FIELD(VmExitInterruptionErrorCode, VmExitInformation, false),
            VMCS_FIELD(VmExitInstructionLen, VmExitInformation, false),
            VMCS_FIELD(VmxInstructionInfo, VmExitInformation, false),

            VMCS_FIELD(GuestEsLimit, GuestState, true),
            VMCS_FIELD(GuestCsLimit, GuestState, true),
            VMCS_FIELD(GuestSsLimit, GuestState, true),
            VMCS_FIELD(GuestDsLimit, GuestState, true),
            VMCS_FIELD(GuestFsLimit, GuestState, true),
            VMCS_FIELD(GuestGsLimit, GuestState, true),
            VMCS_FIELD(GuestLdtrLimit, GuestState, true),
            VMCS_FIELD(GuestTrLimit, GuestState, true),
            VMCS_FIELD(GuestGdtrLimit, GuestState, true),
            VMCS_FIELD(GuestIdtrLimit, GuestState, true),
            VMCS_FIELD(GuestEsAccessRights, GuestState, true),
            VMCS_FIELD(GuestCsAccessRights, GuestState, true),
            VMCS_FIELD(GuestSsAccessRights, GuestState, true),
            VMCS_FIELD(GuestDsAccessRights, GuestState, true),
            VMCS_FIELD(GuestFsAccessRights, GuestState, true),
            VMCS_FIELD(GuestGsAccessRights, GuestState, true),
            VMCS_FIELD(GuestLdtrAccessRights, GuestState, true),
            VMCS_FIELD(GuestTrAccessRights, GuestState, true),
            VMCS_FIELD(GuestInterruptibilityState, GuestState, true),
            VMCS_FIELD(GuestActivityState, GuestState, true),
            VMCS_FIELD(GuestSysenterCs, GuestState, true),

            VMCS_FIELD(HostIa32SysenterCs, HostState, true),

            VMCS_FIELD(ExitQualification, VmExitInformation, false),
            VMCS_FIELD(GuestLinearAddress, VmExitInformation, false),

            VMCS_FIELD(GuestCr0, GuestState, true),
            VMCS_FIELD(GuestCr3, GuestState, true),
            VMCS_FIELD(GuestCr4, GuestState, true),
            VMCS_FIELD(GuestEsBase, GuestState, true),
            VMCS_FIELD(GuestCsBase, GuestState, true),
            VMCS_FIELD(GuestSsBase, GuestState, true),
            VMCS_FIELD(GuestDsBase, GuestState, true),
            VMCS_FIELD(GuestFsBase, GuestState, true),
            VMCS_FIELD(GuestGsBase, GuestState, true),
            VMCS_FIELD(GuestLdtrBase, GuestState, true),
            VMCS_FIELD(GuestTrBase, GuestState, true),
            VMCS_FIELD(GuestGdtrBase, GuestState, true),
            VMCS_FIELD(GuestIdtrBase, GuestState, true),
            VMCS_FIELD(GuestDr7, GuestState, true),
            VMCS_FIELD(GuestRsp, GuestState, true),
            VMCS_FIELD(GuestRip, GuestState, true),
            VMCS_FIELD(GuestRflags, GuestState, true),
            VMCS_FIELD(GuestSysenterEsp, GuestState, true),
            VMCS_FIELD(GuestSysenterEip, GuestState, true),

            VMCS_FIELD(HostCr0, HostState, true),
            VMCS_FIELD(HostCr3, HostState, true),
            VMCS_FIELD(HostCr4, HostState, true),
            VMCS_FIELD(HostFsBase, HostState, true),
            VMCS_FIELD(HostGsBase, HostState, true),
            VMCS_FIELD(HostTrBase, HostState, true),
            VMCS_FIELD(HostGdtrBase, HostState, true),
            VMCS_FIELD(HostIdtrBase, HostState, true),
            VMCS_FIELD(HostIa32SysenterEsp, HostState, true),
            VMCS_FIELD(HostIa32SysenterEip, HostState, true),
            VMCS_FIELD(HostRsp, HostState, true),
            VMCS_FIELD(HostRip, HostState, true),
        };

#undef VMCS_FIELD

        for (const auto &field : supportedFields) {
            if (field.field == field_id) {
                return &field;
            }
        }

        return nullptr;
    }

    static const FieldInfo *
    lookupField(const FieldEncoding &encoding)
    {
        const auto *field = lookupField(encoding.field());
        if (!field) {
            return nullptr;
        }
        if (encoding.highAccess() && field->width() != FieldWidth::U64) {
            return nullptr;
        }
        return field;
    }

    static const FieldInfo *
    lookupField(Encoding encoding)
    {
        FieldEncoding decoded;
        return decodeEncoding(encoding, decoded) ? lookupField(decoded) :
            nullptr;
    }

    static bool
    fieldSupported(Field field)
    {
        return lookupField(field) != nullptr;
    }

    static bool
    fieldSupported(const FieldEncoding &encoding)
    {
        return lookupField(encoding) != nullptr;
    }

    static bool
    fieldSupported(Encoding encoding)
    {
        return lookupField(encoding) != nullptr;
    }

    static bool
    fieldWritable(Field field)
    {
        const auto *info = lookupField(field);
        return info && info->writable;
    }

    static bool
    fieldWritable(const FieldEncoding &encoding)
    {
        const auto *field = lookupField(encoding);
        return field && field->writable;
    }

    static bool
    fieldWritable(Encoding encoding)
    {
        const auto *field = lookupField(encoding);
        return field && field->writable;
    }

    Vmcs() = default;

    Vmcs(Addr region_ptr, uint32_t revision_id)
    {
        reset(region_ptr, revision_id);
    }

    Addr
    pointer() const
    {
        return regionPointer;
    }

    const VmcsHeader &
    vmcsHeader() const
    {
        return header;
    }

    uint32_t
    revisionId() const
    {
        return header.revisionId;
    }

    uint32_t
    abortIndicator() const
    {
        return header.abortIndicator;
    }

    void
    setAbortIndicator(uint32_t abort_indicator)
    {
        header.abortIndicator = abort_indicator;
    }

    LaunchState
    state() const
    {
        return launchState;
    }

    bool
    launched() const
    {
        return launchState == LaunchState::Launched;
    }

    void
    setLaunched(bool launched)
    {
        launchState = launched ? LaunchState::Launched : LaunchState::Clear;
    }

    void
    reset(Addr region_ptr, uint32_t revision_id)
    {
        regionPointer = region_ptr;
        header.revisionId = revision_id;
        clear();
    }

    void
    clear()
    {
        header.abortIndicator = 0;
        launchState = LaunchState::Clear;
        fields.clear();
        fields.emplace(VmInstructionError, 0);
    }

    static uint64_t
    widthMask(FieldWidth width)
    {
        switch (width) {
          case FieldWidth::U16:
            return mask(16);
          case FieldWidth::U32:
            return mask(32);
          case FieldWidth::U64:
          case FieldWidth::Natural:
            return mask(64);
        }

        return mask(64);
    }

    static bool
    decodeEncoding(RawEncoding raw_encoding, FieldEncoding &encoding)
    {
        if ((raw_encoding & ~VmcsEncodingMask) ||
                (raw_encoding & VmcsEncodingReservedBit)) {
            return false;
        }

        const auto raw = static_cast<Encoding>(raw_encoding);
        const bool high = bits(raw, 0);
        const auto full_encoding = high ?
            static_cast<Encoding>(raw & ~static_cast<Encoding>(1)) : raw;
        encoding = FieldEncoding(raw, static_cast<Field>(full_encoding),
                high ? AccessType::High : AccessType::Full);
        return true;
    }

    static bool
    decodeEncoding(RawEncoding raw_encoding, Encoding &encoding)
    {
        FieldEncoding decoded;
        if (!decodeEncoding(raw_encoding, decoded)) {
            return false;
        }

        encoding = decoded.fullEncoding();
        return true;
    }

    static uint64_t
    sanitizeValue(Field field, uint64_t value)
    {
        const auto *info = lookupField(field);
        return info ? (value & widthMask(info->width())) : value;
    }

    static uint64_t
    sanitizeValue(Encoding encoding, uint64_t value)
    {
        const auto *field = lookupField(encoding);
        return field ? sanitizeValue(field->field, value) : value;
    }

    bool
    read(Field field, uint64_t &value) const
    {
        return read(FieldEncoding::full(field), value);
    }

    bool
    read(Encoding encoding, uint64_t &value) const
    {
        FieldEncoding decoded;
        if (!decodeEncoding(encoding, decoded)) {
            return false;
        }
        return read(decoded, value);
    }

    bool
    read(const FieldEncoding &encoding, uint64_t &value) const
    {
        if (!lookupField(encoding)) {
            return false;
        }

        auto it = fields.find(encoding.fullEncoding());
        value = it == fields.end() ? 0 :
            sanitizeValue(encoding.field(), it->second);
        if (encoding.highAccess()) {
            value = bits(value, 63, 32);
        }
        return true;
    }

    bool
    write(Field field, uint64_t value)
    {
        return write(FieldEncoding::full(field), value);
    }

    bool
    write(Encoding encoding, uint64_t value)
    {
        FieldEncoding decoded;
        if (!decodeEncoding(encoding, decoded)) {
            return false;
        }
        return write(decoded, value);
    }

    bool
    write(const FieldEncoding &encoding, uint64_t value)
    {
        const auto *field = lookupField(encoding);
        if (!field || !field->writable) {
            return false;
        }

        if (encoding.highAccess()) {
            uint64_t current = 0;
            read(encoding.field(), current);
            replaceBits(current, 63, 32, bits(value, 31, 0));
            fields[encoding.fullEncoding()] =
                sanitizeValue(encoding.field(), current);
            return true;
        }

        fields[encoding.fullEncoding()] =
            sanitizeValue(encoding.field(), value);
        return true;
    }

    void
    writeUnchecked(Field field, uint64_t value)
    {
        panic_if(!fieldSupported(field),
                "Unsupported VMCS field %#x for unchecked write",
                encodingOf(field));
        fields[encodingOf(field)] = sanitizeValue(field, value);
    }

    void
    writeUnchecked(Encoding encoding, uint64_t value)
    {
        FieldEncoding decoded;
        panic_if(!decodeEncoding(encoding, decoded) || !lookupField(decoded),
                "Unsupported VMCS field %#x for unchecked write", encoding);

        if (decoded.highAccess()) {
            uint64_t current = 0;
            read(decoded.field(), current);
            replaceBits(current, 63, 32, bits(value, 31, 0));
            fields[decoded.fullEncoding()] =
                sanitizeValue(decoded.field(), current);
            return;
        }

        writeUnchecked(decoded.field(), value);
    }

    void
    setInstructionError(uint32_t error)
    {
        writeUnchecked(Field::VmInstructionError, error);
    }

    void
    serialize(CheckpointOut &cp) const
    {
        const uint8_t launch_state = static_cast<uint8_t>(launchState);
        std::vector<Encoding> encodings;
        std::vector<uint64_t> values;
        encodings.reserve(fields.size());
        values.reserve(fields.size());

        for (const auto &[encoding, value] : fields) {
            encodings.push_back(encoding);
            values.push_back(value);
        }

        SERIALIZE_SCALAR(regionPointer);
        SERIALIZE_SCALAR(header.revisionId);
        SERIALIZE_SCALAR(header.abortIndicator);
        SERIALIZE_SCALAR(launch_state);
        SERIALIZE_CONTAINER(encodings);
        SERIALIZE_CONTAINER(values);
    }

    void
    unserialize(CheckpointIn &cp)
    {
        uint8_t launch_state = 0;
        std::vector<Encoding> encodings;
        std::vector<uint64_t> values;

        UNSERIALIZE_SCALAR(regionPointer);
        UNSERIALIZE_SCALAR(header.revisionId);
        UNSERIALIZE_SCALAR(header.abortIndicator);
        UNSERIALIZE_SCALAR(launch_state);
        UNSERIALIZE_CONTAINER(encodings);
        UNSERIALIZE_CONTAINER(values);

        panic_if(encodings.size() != values.size(),
                "Malformed VMCS checkpoint: encoding/value count mismatch");
        panic_if(launch_state > static_cast<uint8_t>(LaunchState::Launched),
                "Malformed VMCS checkpoint: invalid launch state %u",
                launch_state);

        launchState = static_cast<LaunchState>(launch_state);
        fields.clear();
        for (size_t i = 0; i < encodings.size(); ++i) {
            FieldEncoding decoded;
            panic_if(!decodeEncoding(encodings[i], decoded) ||
                    !lookupField(decoded),
                    "Malformed VMCS checkpoint: unsupported field %#x",
                    encodings[i]);
            panic_if(decoded.highAccess(),
                    "Malformed VMCS checkpoint: high-access field %#x",
                    encodings[i]);
            fields.emplace(decoded.fullEncoding(),
                    sanitizeValue(decoded.field(), values[i]));
        }
        fields.emplace(VmInstructionError, 0);
    }

    static_assert(sizeof(VmcsHeader) == 8,
            "VMCS header must be exactly 8 bytes");
    static_assert(alignof(VmcsHeader) <= 4, "Unexpected alignment");
    static_assert(offsetof(VmcsHeader, abortIndicator) == 4,
            "abortIndicator must be at byte 4");
};

} // namespace X86ISA
} // namespace gem5

#endif // __ARCH_X86_VMCS_HH__
