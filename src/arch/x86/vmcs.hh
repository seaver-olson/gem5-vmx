#ifndef __ARCH_X86_VMCS_HH__
#define __ARCH_X86_VMCS_HH__

#include <map>
#include <vector>

#include "base/logging.hh"
#include "base/bitfield.hh"
#include "base/types.hh"
#include "sim/serialize.hh"

namespace gem5
{
namespace X86ISA
{

// This is gem5's shadow model of a VMCS region, not the hardware region itself.
class Vmcs
{
  public:
    using RawEncoding = uint64_t;
    using Encoding = uint32_t;
    using FieldMap = std::map<Encoding, uint64_t>;

    static constexpr size_t VmcsRegionSize = 4096;
    static constexpr RawEncoding VmcsEncodingMask = 0x7fff;
    static constexpr Encoding VmInstructionError = 0x4400;

    enum class LaunchState : uint8_t
    {
        Clear,
        Launched
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

    struct FieldInfo
    {
        Encoding encoding;
        const char *name;

        FieldWidth width;
        // Encoded SDM field type.
        VmcsFieldType type;
        // Logical SDM grouping. This is more specific than the encoded type
        // for control fields.
        VmcsFieldGroup group;
        bool writable;
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
    lookupField(Encoding encoding)
    {
        using Group = VmcsFieldGroup;
        using Type = VmcsFieldType;
        using Width = FieldWidth;

        static constexpr FieldInfo supportedFields[] = {
            {0x0000, "VPID", Width::U16, Type::Control,
             Group::VmExecutionControl, true},

            {0x0800, "GUEST_ES_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x0802, "GUEST_CS_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x0804, "GUEST_SS_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x0806, "GUEST_DS_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x0808, "GUEST_FS_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x080A, "GUEST_GS_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x080C, "GUEST_LDTR_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},
            {0x080E, "GUEST_TR_SELECTOR", Width::U16, Type::GuestState,
             Group::GuestState, true},

            {0x0C00, "HOST_ES_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},
            {0x0C02, "HOST_CS_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},
            {0x0C04, "HOST_SS_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},
            {0x0C06, "HOST_DS_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},
            {0x0C08, "HOST_FS_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},
            {0x0C0A, "HOST_GS_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},
            {0x0C0C, "HOST_TR_SELECTOR", Width::U16, Type::HostState,
             Group::HostState, true},

            {0x2000, "IO_BITMAP_A", Width::U64, Type::Control,
             Group::VmExecutionControl, true},
            {0x2002, "IO_BITMAP_B", Width::U64, Type::Control,
             Group::VmExecutionControl, true},
            {0x2004, "MSR_BITMAP", Width::U64, Type::Control,
             Group::VmExecutionControl, true},
            {0x2006, "VM_EXIT_MSR_STORE_ADDR", Width::U64, Type::Control,
             Group::VmExitControl, true},
            {0x2008, "VM_EXIT_MSR_LOAD_ADDR", Width::U64, Type::Control,
             Group::VmExitControl, true},
            {0x200A, "VM_ENTRY_MSR_LOAD_ADDR", Width::U64, Type::Control,
             Group::VmEntryControl, true},
            {0x2010, "TSC_OFFSET", Width::U64, Type::Control,
             Group::VmExecutionControl, true},

            {0x2800, "VMCS_LINK_POINTER", Width::U64, Type::GuestState,
             Group::GuestState, true},
            {0x2802, "GUEST_IA32_DEBUGCTL", Width::U64, Type::GuestState,
             Group::GuestState, true},
            {0x2804, "GUEST_IA32_PAT", Width::U64, Type::GuestState,
             Group::GuestState, true},
            {0x2806, "GUEST_IA32_EFER", Width::U64, Type::GuestState,
             Group::GuestState, true},
            {0x2808, "GUEST_IA32_PERF_GLOBAL_CTRL", Width::U64,
             Type::GuestState, Group::GuestState, true},

            {0x2C00, "HOST_IA32_PAT", Width::U64, Type::HostState,
             Group::HostState, true},
            {0x2C02, "HOST_IA32_EFER", Width::U64, Type::HostState,
             Group::HostState, true},
            {0x2C04, "HOST_IA32_PERF_GLOBAL_CTRL", Width::U64,
             Type::HostState, Group::HostState, true},

            {0x4000, "PIN_BASED_VM_EXEC_CONTROL", Width::U32,
             Type::Control, Group::VmExecutionControl, true},
            {0x4002, "CPU_BASED_VM_EXEC_CONTROL", Width::U32,
             Type::Control, Group::VmExecutionControl, true},
            {0x4004, "EXCEPTION_BITMAP", Width::U32, Type::Control,
             Group::VmExecutionControl, true},
            {0x4006, "PAGE_FAULT_ERROR_CODE_MASK", Width::U32,
             Type::Control, Group::VmExecutionControl, true},
            {0x4008, "PAGE_FAULT_ERROR_CODE_MATCH", Width::U32,
             Type::Control, Group::VmExecutionControl, true},
            {0x400A, "CR3_TARGET_COUNT", Width::U32, Type::Control,
             Group::VmExecutionControl, true},
            {0x400C, "VM_EXIT_CONTROLS", Width::U32, Type::Control,
             Group::VmExitControl, true},
            {0x400E, "VM_EXIT_MSR_STORE_COUNT", Width::U32,
             Type::Control, Group::VmExitControl, true},
            {0x4010, "VM_EXIT_MSR_LOAD_COUNT", Width::U32,
             Type::Control, Group::VmExitControl, true},
            {0x4012, "VM_ENTRY_CONTROLS", Width::U32, Type::Control,
             Group::VmEntryControl, true},
            {0x4014, "VM_ENTRY_MSR_LOAD_COUNT", Width::U32,
             Type::Control, Group::VmEntryControl, true},
            {0x4016, "VM_ENTRY_INTR_INFO_FIELD", Width::U32,
             Type::Control, Group::VmEntryControl, true},
            {0x4018, "VM_ENTRY_EXCEPTION_ERROR_CODE", Width::U32,
             Type::Control, Group::VmEntryControl, true},
            {0x401A, "VM_ENTRY_INSTRUCTION_LEN", Width::U32,
             Type::Control, Group::VmEntryControl, true},
            {0x401C, "TPR_THRESHOLD", Width::U32, Type::Control,
             Group::VmExecutionControl, true},
            {0x401E, "SECONDARY_VM_EXEC_CONTROL", Width::U32,
             Type::Control, Group::VmExecutionControl, true},

            {0x4400, "VM_INSTRUCTION_ERROR", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},
            {0x4402, "VM_EXIT_REASON", Width::U32, Type::VmExitInfo,
             Group::VmExitInformation, false},
            {0x4404, "VM_EXIT_INTERRUPTION_INFO", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},
            {0x4406, "VM_EXIT_INTERRUPTION_ERROR_CODE", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},
            {0x4408, "IDT_VECTORING_INFO_FIELD", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},
            {0x440A, "IDT_VECTORING_ERROR_CODE", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},
            {0x440C, "VM_EXIT_INSTRUCTION_LEN", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},
            {0x440E, "VMX_INSTRUCTION_INFO", Width::U32,
             Type::VmExitInfo, Group::VmExitInformation, false},

            {0x4800, "GUEST_ES_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4802, "GUEST_CS_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4804, "GUEST_SS_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4806, "GUEST_DS_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4808, "GUEST_FS_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x480A, "GUEST_GS_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x480C, "GUEST_LDTR_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x480E, "GUEST_TR_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4810, "GUEST_GDTR_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4812, "GUEST_IDTR_LIMIT", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x4814, "GUEST_ES_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4816, "GUEST_CS_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4818, "GUEST_SS_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x481A, "GUEST_DS_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x481C, "GUEST_FS_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x481E, "GUEST_GS_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4820, "GUEST_LDTR_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4822, "GUEST_TR_ACCESS_RIGHTS", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4824, "GUEST_INTERRUPTIBILITY_STATE", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4826, "GUEST_ACTIVITY_STATE", Width::U32,
             Type::GuestState, Group::GuestState, true},
            {0x4828, "GUEST_SMBASE", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x482A, "GUEST_SYSENTER_CS", Width::U32, Type::GuestState,
             Group::GuestState, true},
            {0x482E, "VMX_PREEMPTION_TIMER_VALUE", Width::U32,
             Type::GuestState, Group::GuestState, true},

            {0x6800, "GUEST_CR0", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6802, "GUEST_CR3", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6804, "GUEST_CR4", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6806, "GUEST_ES_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6808, "GUEST_CS_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x680A, "GUEST_SS_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x680C, "GUEST_DS_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x680E, "GUEST_FS_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6810, "GUEST_GS_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6812, "GUEST_LDTR_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6814, "GUEST_TR_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6816, "GUEST_GDTR_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6818, "GUEST_IDTR_BASE", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x681A, "GUEST_DR7", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x681C, "GUEST_RSP", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x681E, "GUEST_RIP", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6820, "GUEST_RFLAGS", Width::Natural, Type::GuestState,
             Group::GuestState, true},
            {0x6822, "GUEST_PENDING_DBG_EXCEPTIONS", Width::Natural,
             Type::GuestState, Group::GuestState, true},
            {0x6824, "GUEST_SYSENTER_ESP", Width::Natural,
             Type::GuestState, Group::GuestState, true},
            {0x6826, "GUEST_SYSENTER_EIP", Width::Natural,
             Type::GuestState, Group::GuestState, true},

            {0x6C00, "HOST_CR0", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C02, "HOST_CR3", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C04, "HOST_CR4", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C06, "HOST_FS_BASE", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C08, "HOST_GS_BASE", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C0A, "HOST_TR_BASE", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C0C, "HOST_GDTR_BASE", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C0E, "HOST_IDTR_BASE", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C10, "HOST_IA32_SYSENTER_ESP", Width::Natural,
             Type::HostState, Group::HostState, true},
            {0x6C12, "HOST_IA32_SYSENTER_EIP", Width::Natural,
             Type::HostState, Group::HostState, true},
            {0x6C14, "HOST_RSP", Width::Natural, Type::HostState,
             Group::HostState, true},
            {0x6C16, "HOST_RIP", Width::Natural, Type::HostState,
             Group::HostState, true},
        };

        if (encoding > VmcsEncodingMask) return nullptr;

        for (const auto &field : supportedFields) {
            if (field.encoding == encoding) {
                return &field;
            }
        }

        return nullptr;
    }

    static bool
    fieldSupported(Encoding encoding)
    {
        return lookupField(encoding) != nullptr;
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
    decodeEncoding(RawEncoding raw_encoding, Encoding &encoding)
    {
        if (raw_encoding & ~VmcsEncodingMask) {
            return false;
        }

        encoding = static_cast<Encoding>(raw_encoding);
        return true;
    }

    static uint64_t
    sanitizeValue(Encoding encoding, uint64_t value)
    {
        const auto *field = lookupField(encoding);
        return field ? (value & widthMask(field->width)) : value;
    }

    bool
    read(Encoding encoding, uint64_t &value) const
    {
        if (!fieldSupported(encoding)) {
            return false;
        }

        auto it = fields.find(encoding);
        if (it == fields.end()) {
            value = 0;
            return true;
        }

        value = sanitizeValue(encoding, it->second);
        return true;
    }

    bool
    write(Encoding encoding, uint64_t value)
    {
        if (!fieldWritable(encoding)) {
            return false;
        }

        fields[encoding] = sanitizeValue(encoding, value);
        return true;
    }

    void
    writeUnchecked(Encoding encoding, uint64_t value)
    {
        panic_if(!fieldSupported(encoding),
                "Unsupported VMCS field %#x for unchecked write", encoding);
        fields[encoding] = sanitizeValue(encoding, value);
    }

    void
    setInstructionError(uint32_t error)
    {
        writeUnchecked(VmInstructionError, error);
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

        launchState = static_cast<LaunchState>(launch_state);
        fields.clear();
        for (size_t i = 0; i < encodings.size(); ++i) {
            fields.emplace(encodings[i], values[i]);
        }
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
