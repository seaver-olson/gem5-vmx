#ifndef __ARCH_X86_VMCS_HH__
#define __ARCH_X86_VMCS_HH__

#include <map>
#include <vector>

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
    using Encoding = uint64_t;
    using FieldMap = std::map<Encoding, uint64_t>;

    static constexpr size_t VmcsRegionSize = 4096;

    enum class LaunchState : uint8_t
    {
        Clear,
        Launched
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
    }

    bool
    read(Encoding encoding, uint64_t &value) const
    {
        auto it = fields.find(encoding);
        if (it == fields.end()) {
            return false;
        }

        value = it->second;
        return true;
    }

    void
    write(Encoding encoding, uint64_t value)
    {
        fields[encoding] = value;
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
