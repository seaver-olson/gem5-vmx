/* SPDX-License-Identifier: BSD-3-Clause */
#include "arch/x86/paging_tester.hh"

#include <array>

#include "arch/x86/ept_walker.hh"
#include "arch/x86/faults.hh"
#include "arch/x86/isa.hh"
#include "arch/x86/ldstflags.hh"
#include "arch/x86/mmu.hh"
#include "arch/x86/pagetable_walker.hh"
#include "arch/x86/tlb.hh"
#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "mem/port_proxy.hh"
#include "sim/system.hh"

namespace gem5::X86ISA
{
namespace
{

constexpr Addr EptRoot = 0x2800000;
constexpr Addr EptTableLeaves = EptRoot + 3 * 4096;
constexpr Addr HostTables = 0x3000000;
constexpr Addr HostData = 0x3200000;
constexpr Addr OtherHostData = 0x3400000;
constexpr Addr EptDataLeaf = EptRoot + 8192 + 8 * (0x2000000 >> 21);
constexpr uint64_t NestedCaps = 1 | (1ULL << 6) | (1ULL << 8) |
    (1ULL << 14) | (1ULL << 16);

class EptTranslation : public BaseMMU::Translation
{
  public:
    using Callback = std::function<void(const Fault &, const RequestPtr &,
                                       ThreadContext *, BaseMMU::Mode)>;
    explicit EptTranslation(Callback callback,
                            std::shared_ptr<bool> cancelled = nullptr)
        : callback(std::move(callback)), cancelled(std::move(cancelled)) {}
    bool squashed() const override { return cancelled && *cancelled; }
    void markDelayed() override
    {
        panic_if(finished || delayed, "EPT MMU marked delayed after completion");
        delayed = true;
    }
    void finish(const Fault &fault, const RequestPtr &req,
                ThreadContext *tc, BaseMMU::Mode mode) override
    {
        panic_if(finished, "EPT MMU completed a request twice");
        finished = true;
        callback(fault, req, tc, mode);
    }
  private:
    Callback callback;
    std::shared_ptr<bool> cancelled;
    bool finished = false;
    bool delayed = false;
};

void
checkResult(const ept::WalkResult &actual, const ept::WalkResult &expected,
            const char *label)
{
    panic_if(actual.status != expected.status ||
             actual.hostPhysical != expected.hostPhysical ||
             actual.permissions != expected.permissions ||
             actual.pageShift != expected.pageShift ||
             actual.memoryType != expected.memoryType ||
             actual.ignorePat != expected.ignorePat ||
             actual.level != expected.level ||
             actual.entryAddress != expected.entryAddress ||
             actual.entry != expected.entry,
             "EPT %s: lookup modes returned different results", label);
}

} // anonymous namespace

void
PagingTester::setupEpt(bool presetAD)
{
    setup(Long);
    std::array<uint8_t, 4 * 4096> zero{};
    system->physProxy.writeBlob(EptRoot, zero.data(), zero.size());
    system->physProxy.writeBlob(HostTables, zero.data(), zero.size());
    system->physProxy.write<uint64_t>(EptRoot,
        (EptRoot + 4096) | 7, ByteOrder::little);
    system->physProxy.write<uint64_t>(EptRoot + 4096,
        (EptRoot + 8192) | 7, ByteOrder::little);
    system->physProxy.write<uint64_t>(EptRoot + 8192 + 8 * (Tables >> 21),
        EptTableLeaves | 7, ByteOrder::little);
    system->physProxy.write<uint64_t>(EptDataLeaf,
        HostData | 7 | (6ULL << 3) | 128, ByteOrder::little);
    for (unsigned level = 0; level < descriptors.size(); ++level) {
        const auto value = descriptor(level) | (presetAD ? 32 : 0) |
            (presetAD && level == 3 ? 64 : 0);
        const Addr host = HostTables + level * 4096 +
            (descriptors[level] & 4095);
        descriptor(level, 0); // Direct GPA reads must not accidentally work.
        descriptors[level] = host;
        descriptor(level, value);
        system->physProxy.write<uint64_t>(EptTableLeaves + 8 * level,
            (HostTables + level * 4096) | 7 | (6ULL << 3), ByteOrder::little);
    }
    const Addr offset = Virtual & 4095;
    system->physProxy.write<uint8_t>(Physical + offset, 0xcc);
    system->physProxy.write<uint8_t>(HostData + offset, 0x90);
    system->physProxy.write<uint8_t>(HostData + 4096 + offset, 0xeb);
    system->physProxy.write<uint8_t>(OtherHostData + offset, 0xeb);
    auto *isa = static_cast<ISA *>(tc->getIsaPtr());
    panic_if(isa->vmxState().activeEptConfig,
             "EPT fixture overwrote an active EPT context");
    savedEptCapability = tc->readMiscRegNoEffect(misc_reg::VmxEptVpidCap);
    tc->setMiscRegNoEffect(misc_reg::VmxEptVpidCap, NestedCaps);
    isa->vmxState().activeEptConfig =
        *ept::decodeEptp(EptRoot | (3 << 3) | 6, NestedCaps, 48);
}

void
PagingTester::finishEpt()
{
    static_cast<ISA *>(tc->getIsaPtr())->vmxState().activeEptConfig.reset();
    tc->setMiscRegNoEffect(misc_reg::VmxEptVpidCap, savedEptCapability);
    inspectAttributes = {};
    inspectResponse = {};
    squash = {};
    awaiting = false;
    schedule(nextEvent, curTick() + 1);
}

void
PagingTester::eptMmuCases()
{
    for (auto mode : {BaseMMU::Read, BaseMMU::Write, BaseMMU::Execute}) {
        for (bool functional : {false, true}) {
            for (bool warm : {false, true}) {
                if (functional && warm) // Functional deliberately bypasses TLBs.
                    continue;
                for (auto test : {EptTest::Mapping, EptTest::DataMisconfig,
                     mode == BaseMMU::Read ? EptTest::DeniedRead :
                     mode == BaseMMU::Write ? EptTest::DeniedWrite :
                                             EptTest::DeniedExecute}) {
                    cases.emplace_back([=, this] {
                        eptMmuAccess(mode, functional, warm, test);
                    });
                }
            }
        }
    }
    for (bool functional : {false, true}) {
        for (unsigned level = 0; level < 4; ++level) {
            for (auto test : {EptTest::TableRead, EptTest::TableMisconfig,
                              EptTest::TableUpdate}) {
                if (functional && test == EptTest::TableUpdate)
                    continue;
                cases.emplace_back([=, this] {
                    eptMmuAccess(BaseMMU::Write, functional, false, test, level);
                });
            }
        }
        cases.emplace_back([=, this] {
            eptMmuAccess(BaseMMU::Read, functional, false, EptTest::GuestFault);
        });
    }
    cases.emplace_back([this] {
        eptMmuAccess(BaseMMU::Write, false, false, EptTest::Replace);
    });
    cases.emplace_back([this] {
        eptMmuAccess(BaseMMU::Read, false, true, EptTest::Remap);
    });
    for (bool warm : {false, true}) {
        for (auto test : {EptTest::InheritedWrite, EptTest::DirtyUpdate,
                          EptTest::Rmw}) {
            cases.emplace_back([=, this] {
                eptMmuAccess(test == EptTest::Rmw ? BaseMMU::Read :
                    BaseMMU::Write, false, warm, test, 3);
            });
        }
    }
    if (system->isTimingMode()) {
        for (unsigned point = 0; point < 4; ++point) {
            for (auto inject : {RetryRead, SquashRetryRead, SquashRead,
                                 InvalidateRead}) {
                cases.emplace_back([=, this] { eptMmuTiming(point, inject); });
            }
        }
        for (auto inject : {RetryRead, SquashRetryRead, SquashRead,
                             InvalidateRead}) {
            cases.emplace_back([=, this] { eptMmuTiming(2, inject, true); });
        }
        cases.emplace_back([this] { eptMmuTiming(4, SquashRead); });
    }
}

void
PagingTester::eptMmuAccess(BaseMMU::Mode mode, bool functional, bool warm,
                          EptTest test, unsigned level, bool primed)
{
    if (!primed) {
        setupEpt(!functional && test != EptTest::TableUpdate &&
                               test != EptTest::Replace);
        if (test == EptTest::DirtyUpdate)
            descriptor(3, descriptor(3) & ~64ULL);
    }
    const bool priming = warm && !primed;
    const auto operationMode = priming && test == EptTest::DirtyUpdate ?
        BaseMMU::Read : mode;
    auto *cache = static_cast<TLB *>(mode == BaseMMU::Execute ?
                                    mmu->itb : mmu->dtb);
    const TlbContext tag{tc->contextId(), 0};
    panic_if(bool(cache->lookup(Virtual, false, tag)) != primed,
             "EPT MMU test did not take the intended TLB hit/miss path");
    if (primed && test != EptTest::DirtyUpdate) {
        // Leave the TLB intact but poison the guest descriptors. This also
        // catches an unexpected rewalk through the independent I-TLB port.
        for (unsigned l = 0; l < 4; ++l)
            descriptor(l, 0);
    }
    const auto changeRights = [this](Addr address, uint64_t rights) {
        auto raw = system->physProxy.read<uint64_t>(address, ByteOrder::little);
        system->physProxy.write<uint64_t>(address, (raw & ~7ULL) | rights,
                                          ByteOrder::little);
    };
    const auto current = priming ? EptTest::Mapping : test;
    const Addr tableLeaf = EptTableLeaves + 8 * level;
    switch (current) {
      case EptTest::DeniedRead: changeRights(EptDataLeaf, 4); break;
      case EptTest::DeniedWrite: changeRights(EptDataLeaf, 5); break;
      case EptTest::DeniedExecute: changeRights(EptDataLeaf, 3); break;
      case EptTest::TableRead: changeRights(tableLeaf, 4); break;
      case EptTest::TableUpdate: changeRights(tableLeaf, 5); break;
      case EptTest::DirtyUpdate: changeRights(tableLeaf, 5); break;
      case EptTest::Rmw: changeRights(EptDataLeaf, 5); break;
      case EptTest::DataMisconfig: changeRights(EptDataLeaf, 2); break;
      case EptTest::TableMisconfig: changeRights(tableLeaf, 2); break;
      case EptTest::GuestFault:
        descriptor(3, descriptor(3) & ~1ULL);
        changeRights(EptDataLeaf, 2); // Guest #PF precedes the final EPT lookup.
        break;
      case EptTest::Replace:
        replacement = (Physical + 4096) | 7 | 64 | 512;
        injection = ReplaceLeaf;
        break;
      case EptTest::Remap:
        system->physProxy.write<uint64_t>(EptDataLeaf,
            OtherHostData | 7 | (6ULL << 3) | 128, ByteOrder::little);
        break;
      case EptTest::InheritedWrite:
        changeRights(EptRoot, 5);
        changeRights(EptDataLeaf, 3);
        break;
      default: break;
    }
    const bool tableAccess = current == EptTest::TableRead ||
        current == EptTest::TableUpdate || current == EptTest::DirtyUpdate ||
        current == EptTest::TableMisconfig;
    const bool misconfig = current == EptTest::DataMisconfig ||
        current == EptTest::TableMisconfig;
    const bool success = current == EptTest::Mapping ||
        current == EptTest::Replace || current == EptTest::Remap;
    const Addr expectedHpa = (current == EptTest::Remap ? OtherHostData :
        HostData + (current == EptTest::Replace ? 4096 : 0)) + (Virtual & 4095);
    std::vector<uint64_t> before;
    for (unsigned l = 0; l < 4; ++l)
        before.push_back(descriptor(l));
    auto eptBefore = std::make_shared<std::array<uint8_t, 4 * 4096>>();
    system->physProxy.readBlob(EptRoot, eptBefore->data(), eptBefore->size());
    auto tableRequests = std::make_shared<unsigned>(0);
    inspectAttributes = [=](PacketPtr pkt) {
        if (pkt->getAddr() >= HostTables && pkt->getAddr() < HostTables + 16384)
            ++*tableRequests;
    };
    const Tick start = curTick();
    auto flags = accessFlags;
    if (current == EptTest::Rmw)
        flags.set(Request::READ_MODIFY_WRITE);
    if (mode == BaseMMU::Execute)
        flags.set(Request::INST_FETCH);
    auto req = std::make_shared<Request>(Virtual, 1, flags,
        system->getRequestorId(this), Virtual, tc->contextId());
    const auto retired = [=, this] {
        std::array<uint8_t, 4 * 4096> eptAfter;
        system->physProxy.readBlob(EptRoot, eptAfter.data(), eptAfter.size());
        panic_if(eptAfter != *eptBefore, "Guest paging modified EPT entries");
        panic_if(primed && current != EptTest::DirtyUpdate && *tableRequests,
                 "EPT MMU TLB hit repeated a guest table access");
        for (unsigned l = 0; l < 4; ++l) {
            const Addr gpa = Tables + l * 4096 + (descriptors[l] & 4095);
            panic_if(system->physProxy.read<uint64_t>(gpa, ByteOrder::little),
                     "EPT MMU modified a GPA instead of its backing HPA");
            if (functional)
                panic_if(descriptor(l) != before[l],
                         "Functional EPT MMU changed guest A/D bits");
        }
        panic_if(functional && curTick() != start,
                 "Functional EPT MMU advanced simulated time");
        if (current == EptTest::Replace) {
            panic_if(injected != 1 || descriptor(3) != (replacement | 32) ||
                     (system->isTimingMode() && failedUpdates != 1),
                     "Nested A/D retry lost the new mapping, D or software bits");
        }
        if (current == EptTest::TableUpdate) {
            for (unsigned l = 0; l < 4; ++l)
                panic_if(bool(descriptor(l) & 32) != (l < level),
                         "Denied nested A/D update modified the wrong level");
        }
        if (current == EptTest::DirtyUpdate)
            panic_if(descriptor(3) & 64,
                     "EPT-denied guest dirty update set D");
        awaiting = false;
        if (priming) {
            schedule(new EventFunctionWrapper([=, this] {
                eptMmuAccess(mode, functional, warm, test, level, true);
            }, name() + ".ept_warm", true), curTick() + 1);
        } else {
            finishEpt();
        }
    };
    const auto translated = [=, this](const Fault &fault, const RequestPtr &out,
                                       ThreadContext *context,
                                       BaseMMU::Mode returnedMode) {
        panic_if(out != req || context != tc || returnedMode != operationMode,
                 "EPT MMU changed the request, thread or original mode");
        if (!success) {
            panic_if((functional || tableAccess ||
                      current == EptTest::GuestFault) &&
                     cache->lookup(Virtual, false, tag),
                     "Failed guest/functional walk filled the TLB");
            if (current == EptTest::GuestFault) {
                const auto *page = dynamic_cast<PageFault *>(fault.get());
                panic_if(!page || page->describe() != PageFault(Virtual,
                    false, mode, false, false).describe(),
                    "Nested paging lost guest page-fault priority");
            } else {
                const auto *exit = dynamic_cast<VmxExitFault *>(fault.get());
                panic_if(!exit, "EPT MMU failure was not a VM-exit fault");
                const auto &info = exit->info();
                const Addr gpa = tableAccess ? Tables + level * 4096 +
                    (descriptors[level] & 4095) : Physical + (Virtual & 4095);
                const uint64_t permissions = current == EptTest::DeniedRead ||
                    current == EptTest::TableRead ? 4 :
                    current == EptTest::DeniedExecute ? 3 :
                    current == EptTest::InheritedWrite ? 1 : 5;
                const uint64_t access = tableAccess ?
                    (current == EptTest::TableUpdate ||
                     current == EptTest::DirtyUpdate ? 2 : 1) :
                    current == EptTest::Rmw ? 2 :
                    mode == BaseMMU::Read ? 1 : mode == BaseMMU::Write ? 2 : 4;
                const uint64_t qualification = access | (permissions << 3) |
                    128 | (tableAccess ? 0 : 256);
                panic_if(info.reason != (misconfig ?
                         VmxExitReason::EptMisconfiguration :
                         VmxExitReason::EptViolation) || info.vmEntryFailure ||
                         !info.hasGuestPhysicalAddress ||
                         info.guestPhysicalAddress != gpa ||
                         info.hasQualification != !misconfig ||
                         info.hasGuestLinearAddress != !misconfig ||
                         info.hasInstructionLength || info.hasInstructionInfo ||
                         info.hasInterruptionInfo || info.hasInterruptionErrorCode ||
                         (!misconfig && (info.qualification != qualification ||
                          info.guestLinearAddress != Virtual)),
                         "EPT MMU reported wrong exit metadata");
            }
            retired();
            return;
        }
        panic_if(fault || out->getPaddr() != expectedHpa,
                 "EPT MMU returned wrong final HPA or fault");
        if (!functional) {
            const auto *entry = cache->lookup(Virtual, false, tag);
            panic_if(!entry || entry->paddr != Physical +
                     (current == EptTest::Replace ? 4096 : 0),
                     "EPT MMU cached an HPA instead of a guest-physical page");
        }
        auto *packet = operationMode == BaseMMU::Write ?
            Packet::createWrite(out) : Packet::createRead(out);
        packet->allocate();
        if (operationMode == BaseMMU::Write)
            packet->setLE<uint8_t>(0xa5);
        const auto dataDone = [=, this](PacketPtr response, bool cancelled) {
            panic_if(cancelled || response->isError() ||
                     (operationMode != BaseMMU::Write &&
                      response->getLE<uint8_t>() !=
                        (current == EptTest::Remap ? 0xeb : 0x90)),
                     "EPT MMU accessed incorrect instruction/data bytes");
            delete response;
            panic_if(system->physProxy.read<uint8_t>(Physical +
                     (Virtual & 4095)) != 0xcc ||
                     (operationMode == BaseMMU::Write &&
                      system->physProxy.read<uint8_t>(expectedHpa) != 0xa5),
                     "EPT MMU final write reached the wrong physical page");
            retired();
        };
        auto &port = tlb->getWalker()->pagingPort();
        if (functional) {
            port.sendFunctional(packet);
            dataDone(packet, false);
        } else if (system->isTimingMode()) {
            port.submit(packet, [] { return true; }, dataDone);
        } else {
            port.sendAtomic(packet);
            dataDone(packet, false);
        }
    };
    awaiting = true;
    if (functional) {
        translated(mmu->translateFunctional(req, tc, operationMode), req, tc,
                   operationMode);
    } else if (system->isTimingMode()) {
        auto callback = std::make_unique<EptTranslation>(translated);
        auto *raw = callback.get();
        continuations.push_back(std::move(callback));
        mmu->translateTiming(req, tc, raw, operationMode);
    } else {
        translated(mmu->translateAtomic(req, tc, operationMode), req, tc,
                   operationMode);
    }
}

void
PagingTester::eptMmuTiming(unsigned point, Injection inject, bool warm,
                          bool primed)
{
    if (!primed)
        setupEpt(point == 2);
    // Points select nested table-read lookup, A/D-write lookup, final lookup,
    // the conditional update packet, or cancellation before admission.
    if (warm && !primed) {
        auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
            system->getRequestorId(this), Virtual, tc->contextId());
        auto callback = std::make_unique<EptTranslation>(
            [=, this](const Fault &fault, const RequestPtr &, ThreadContext *,
                       BaseMMU::Mode) {
                panic_if(fault, "EPT lifecycle warm-up failed");
                schedule(new EventFunctionWrapper([=, this] {
                    eptMmuTiming(point, inject, warm, true);
                }, name() + ".ept_lifecycle_warm", true), curTick() + 1);
            });
        auto *raw = callback.get();
        continuations.push_back(std::move(callback));
        awaiting = true;
        mmu->translateTiming(req, tc, raw, BaseMMU::Read);
        return;
    }
    const auto mode = point == 2 ? BaseMMU::Read : BaseMMU::Write;
    auto cancelled = std::make_shared<bool>(point == 4);
    squash = [cancelled] { *cancelled = true; };
    auto roots = std::make_shared<unsigned>(0);
    auto armed = std::make_shared<bool>(false);
    const unsigned targetRoot = point == 1 ? 2 : point == 2 && !warm ? 5 : 1;
    inspectAttributes = [=, this](PacketPtr pkt) {
        if (pkt->getAddr() == EptRoot)
            ++*roots;
        const bool target = point == 3 ? pkt->req->isAtomicReturn() :
            pkt->getAddr() == EptRoot && *roots == targetRoot;
        if (*armed || !target)
            return;
        *armed = true;
        injection = point != 3 ? inject :
            inject == RetryRead ? RetryUpdate :
            inject == SquashRetryRead ? SquashRetryUpdate :
            inject == SquashRead ? SquashUpdate : InvalidateUpdate;
        if (inject == RetryRead) {
            // Every subsequent read must use the acceptance-time snapshot,
            // including the final stage after guest paging completes.
            static_cast<ISA *>(tc->getIsaPtr())->vmxState().activeEptConfig.reset();
            tc->setMiscRegNoEffect(misc_reg::VmxEptVpidCap, 0);
        }
    };
    inspectResponse = [=](PacketPtr pkt) {
        if (*cancelled && !pkt->req->isAtomicReturn())
            pkt->setBadAddress(); // Cancellation must win over memory errors.
    };
    auto req = std::make_shared<Request>(Virtual, 1, accessFlags,
        system->getRequestorId(this), Virtual, tc->contextId());
    auto callback = std::make_unique<EptTranslation>(
        [=, this](const Fault &fault, const RequestPtr &out,
                   ThreadContext *context, BaseMMU::Mode returnedMode) {
            panic_if(out != req || context != tc || returnedMode != mode ||
                     injected != (point == 4 ? 0 : 1),
                     "Nested EPT lifecycle callback or injection failed");
            const bool retry = inject == RetryRead;
            panic_if(retry ? bool(fault) || out->getPaddr() !=
                         HostData + (Virtual & 4095) :
                         !dynamic_cast<ReExec *>(fault.get()),
                     "Nested EPT lifecycle returned wrong HPA/fault");
            if (!retry) {
                panic_if(!warm && tlb->lookup(Virtual, false,
                    {tc->contextId(), 0}) && point != 2,
                    "Cancelled guest walk published a TLB entry");
                for (unsigned l = 0; l < 4; ++l) {
                    const bool accessed = point == 2 ||
                        (point == 3 && (inject == SquashRead ||
                                       inject == InvalidateRead) && l == 0);
                    panic_if(bool(descriptor(l) & 32) != accessed ||
                             bool(descriptor(l) & 64) != (point == 2 && l == 3),
                             "Nested EPT cancellation changed unexpected A/D bits");
                }
            }
            finishEpt();
        }, cancelled);
    auto *raw = callback.get();
    continuations.push_back(std::move(callback));
    awaiting = true;
    mmu->translateTiming(req, tc, raw, mode);
    panic_if(mmu->drain() != DrainState::Draining,
             "Nested EPT lost MMU ownership before retirement");
}

void
PagingTester::eptCases()
{
    using ept::WalkStatus;
    // Test capabilities only: this does not change the ISA's advertised MSR.
    constexpr uint64_t Caps = 1ULL | (1ULL << 6) | (1ULL << 7) |
        (1ULL << 8) | (1ULL << 14) | (1ULL << 16) | (1ULL << 17) |
        (1ULL << 21);
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    EptConfig config{};
    Addr gpa = 0;
    Addr mappingBase = 0;
    unsigned physicalBits = 48;
    unsigned leafLevel = 1;
    unsigned errorRead = 0;
    uint64_t capability = Caps;
    unsigned checks = 0;

    const auto setup = [&](unsigned levels = 4, unsigned leaf = 1,
                           uint8_t memoryType = 6) {
        std::array<uint8_t, 5 * 4096> zero{};
        system->physProxy.writeBlob(Tables, zero.data(), zero.size());
        descriptors.clear();
        descriptorSize = 8;
        physicalBits = 48;
        capability = Caps;
        errorRead = 0;
        leafLevel = leaf;
        // The translated HPA is deliberately outside installed RAM. Lookup
        // must return it, not read it or silently substitute the GPA.
        mappingBase = 0x80000000;
        gpa = 0xabc;
        for (unsigned level = 1; level <= levels; ++level)
            gpa |= Addr(3 * level + 1) << (12 + 9 * (level - 1));
        const uint64_t eptp = Tables | ((levels - 1) << 3) | memoryType;
        config = *ept::decodeEptp(eptp, capability, physicalBits);
        for (unsigned level = levels; level >= leaf; --level) {
            const Addr table = Tables + (levels - level) * 4096;
            const Addr slot = (gpa >> (12 + 9 * (level - 1))) & 511;
            descriptors.push_back(table + 8 * slot);
            const uint64_t raw = level == leaf ?
                mappingBase | 7 | (6 << 3) | (leaf > 1 ? 128 : 0) :
                (table + 4096) | 7;
            descriptor(descriptors.size() - 1, raw);
        }
    };

    const auto checkMode = [&](bool functional, const char *label,
                               WalkStatus expected, unsigned reads,
                               EptAccess access, uint8_t permissions) {
        std::vector<uint64_t> before;
        for (unsigned i = 0; i < descriptors.size(); ++i)
            before.push_back(descriptor(i));
        unsigned seen = 0;
        observedAtomicLatency = 0;
        inspectAttributes = [&](PacketPtr packet) {
            panic_if(seen >= reads || packet->getAddr() != descriptors[seen],
                     "EPT %s: unexpected entry read %u at %#x", label, seen,
                     packet->getAddr());
            panic_if(packet->cmd != MemCmd::ReadReq ||
                     packet->getSize() != 8 ||
                     !packet->req->getFlags().isSet(Request::PHYSICAL) ||
                     packet->req->requestorId() != requestor ||
                     packet->req->isUncacheable() != (config.memoryType == 0),
                     "EPT %s: incorrect physical read request", label);
            const auto attributes = PagingPort::attributes(packet);
            panic_if(attributes.pwt || attributes.pcd,
                     "EPT %s: guest paging attributes leaked into EPT", label);
            if (++seen == errorRead) {
                packet->makeResponse();
                packet->setBadAddress();
            }
        };
        const Tick start = curTick();
        const ept::LookupInput input(gpa, access, config);
        ept::WalkResult result;
        Tick latency = 0;
        if (functional) {
            result = ept::walkFunctional(port, requestor, input, physicalBits,
                                         capability);
        } else {
            const auto lookup = ept::walkAtomic(port, requestor, input,
                                                physicalBits, capability);
            result = lookup.result;
            latency = lookup.latency;
        }
        inspectAttributes = {};
        panic_if(result.status != expected || seen != reads,
                 "EPT %s: status %u/%u, reads %u/%u", label,
                 unsigned(result.status), unsigned(expected), seen, reads);
        panic_if(latency != observedAtomicLatency ||
                 curTick() != start ||
                 (!functional && reads > 0 && errorRead != 1 &&
                  latency == 0) ||
                 (reads == 0 && latency != 0),
                 "EPT %s: incorrect %s latency", label,
                 functional ? "functional" : "atomic");
        panic_if(!port.idle(), "EPT %s: retained port ownership", label);
        if (reads) {
            panic_if(result.level != config.walkLength - reads + 1 ||
                     result.entryAddress != descriptors[reads - 1] ||
                     (expected != WalkStatus::MemoryError &&
                      result.entry != before[reads - 1]),
                     "EPT %s: incorrect terminal diagnostics", label);
        } else {
            panic_if(result.level || result.entryAddress || result.entry,
                     "EPT %s: preflight failure reported an entry", label);
        }
        if (expected == WalkStatus::Mapping) {
            const unsigned shift = 12 + 9 * (leafLevel - 1);
            const Addr expectedHpa = mappingBase | (gpa & mask(shift));
            panic_if(result.hostPhysical != expectedHpa ||
                     result.permissions != permissions ||
                     result.pageShift != shift ||
                     result.memoryType != bits(before.back(), 5, 3) ||
                     result.ignorePat != bool(bits(before.back(), 6)),
                     "EPT %s: incorrect mapping or leaf attributes", label);
        } else {
            panic_if(result.hostPhysical ||
                     result.permissions !=
                         (expected == WalkStatus::Violation ? permissions : 0) ||
                     result.pageShift || result.memoryType || result.ignorePat,
                     "EPT %s: failure exposed a mapping or wrong rights", label);
        }
        for (unsigned i = 0; i < before.size(); ++i)
            panic_if(descriptor(i) != before[i],
                     "EPT %s: walk modified a table entry", label);
        ++checks;
        return result;
    };
    const auto check = [&](const char *label, WalkStatus expected,
                           unsigned reads, EptAccess access = EptAccess::Read,
                           uint8_t permissions = 7) {
        // Functional runs first, so it also sees cold tables, not just lines
        // already brought into the cache by an atomic lookup.
        const auto functional = checkMode(true, label, expected, reads, access,
                                           permissions);
        if (system->isAtomicMode()) {
            const auto atomic = checkMode(false, label, expected, reads,
                                           access, permissions);
            checkResult(atomic, functional, label);
        } else {
            // Replay the same tables asynchronously after the synchronous
            // suite finishes. Capture values, not this function's locals.
            const ept::LookupInput input(gpa, access, config);
            const auto slots = descriptors;
            std::vector<uint64_t> entries;
            for (unsigned i = 0; i < slots.size(); ++i)
                entries.push_back(descriptor(i));
            cases.emplace_back([=, this] {
                descriptors = slots;
                descriptorSize = 8;
                for (unsigned i = 0; i < entries.size(); ++i)
                    descriptor(i, entries[i]);
                struct Progress
                {
                    unsigned responses = 0;
                    unsigned callbacks = 0;
                    Tick lastResponse = 0;
                };
                auto progress = std::make_shared<Progress>();
                inspectAttributes = [=, this](PacketPtr packet) {
                    const unsigned next = progress->responses;
                    panic_if(pendingPackets || next >= reads ||
                             packet->getAddr() != slots[next] ||
                             packet->getSize() != 8 ||
                             packet->cmd != MemCmd::ReadReq ||
                             packet->req->requestorId() != requestor ||
                             !packet->req->getFlags().isSet(
                                 Request::PHYSICAL) ||
                             packet->req->isUncacheable() !=
                                 (input.config.memoryType == 0),
                             "EPT %s: invalid timing request", label);
                    const auto attrs = PagingPort::attributes(packet);
                    panic_if(attrs.pwt || attrs.pcd,
                             "EPT %s: leaked guest attributes", label);
                };
                inspectResponse = [=](PacketPtr packet) {
                    panic_if(progress->responses >= reads ||
                             packet->getAddr() != slots[progress->responses],
                             "EPT %s: response out of order", label);
                    if (++progress->responses == errorRead)
                        packet->setBadAddress();
                    progress->lastResponse = curTick();
                };
                const Tick start = curTick();
                awaiting = true;
                ept::walkTiming(tlb->getWalker()->pagingPort(), requestor,
                    input, physicalBits, capability, [] { return true; },
                    [=, this](const ept::WalkResult &result) {
                        panic_if(++progress->callbacks != 1 || !awaiting ||
                                 progress->responses != reads,
                                 "EPT %s: incorrect timing completion", label);
                        checkResult(result, functional, label);
                        auto &port = tlb->getWalker()->pagingPort();
                        panic_if(port.outstanding() != (reads ? 1 : 0) ||
                                 (reads ? curTick() <= start ||
                                     curTick() != progress->lastResponse :
                                     curTick() != start),
                                 "EPT %s: wrong timing/ownership", label);
                        for (unsigned i = 0; i < entries.size(); ++i)
                            panic_if(descriptor(i) != entries[i],
                                     "EPT %s: timing modified tables", label);
                        inspectAttributes = {};
                        inspectResponse = {};
                        awaiting = false;
                        schedule(nextEvent, curTick() + 1);
                    });
                panic_if(reads && (!awaiting || progress->callbacks),
                         "EPT %s: memory lookup completed inline", label);
            });
        }
    };

    for (unsigned levels : {4, 5}) {
        for (unsigned leaf : {1, 2, 3}) {
            for (uint8_t type : {0, 6}) {
                setup(levels, leaf, type);
                for (auto access : {EptAccess::Read, EptAccess::Write,
                                     EptAccess::Execute}) {
                    check("mapping", WalkStatus::Mapping, levels - leaf + 1,
                          access);
                }
            }
        }
    }
    for (unsigned width : {32, 52}) {
        for (unsigned leaf : {1, 2, 3}) {
            setup(4, leaf);
            physicalBits = width;
            const unsigned shift = 12 + 9 * (leaf - 1);
            mappingBase = (1ULL << width) - (1ULL << shift);
            descriptor(descriptors.size() - 1,
                       mappingBase | 7 | (6 << 3) | (leaf > 1 ? 128 : 0));
            gpa &= ~mask(shift);
            check("first page byte", WalkStatus::Mapping, 5 - leaf);
            gpa |= mask(shift);
            check("last host-physical byte", WalkStatus::Mapping, 5 - leaf);
        }
    }
    setup();
    descriptor(0, descriptor(0) & ~2ULL); // ancestor removes W
    descriptor(1, descriptor(1) & ~4ULL); // another removes X
    check("intersected read", WalkStatus::Mapping, 4, EptAccess::Read, 1);
    check("denied write", WalkStatus::Violation, 4, EptAccess::Write, 1);
    check("denied execute", WalkStatus::Violation, 4, EptAccess::Execute, 1);
    descriptor(3, (descriptor(3) & ~7ULL) | 4); // X-only leaf
    check("empty intersection", WalkStatus::Violation, 4, EptAccess::Execute, 0);
    for (unsigned i = 0; i < 4; ++i)
        descriptor(i, (descriptor(i) & ~7ULL) | 4);
    check("execute-only", WalkStatus::Mapping, 4, EptAccess::Execute, 4);
    check("denied read", WalkStatus::Violation, 4, EptAccess::Read, 4);
    capability &= ~1ULL;
    check("unsupported execute-only", WalkStatus::Misconfiguration, 1);

    for (unsigned levels : {4, 5}) {
        for (unsigned stop = 0; stop < levels; ++stop) {
            setup(levels);
            // Absent entries win over otherwise-reserved contents.
            descriptor(stop, (1ULL << 48) | 128 | (7 << 3));
            check("absent", WalkStatus::Violation, stop + 1,
                  EptAccess::Read, 0);
            descriptor(stop, (descriptor(stop) & ~7ULL) | 2);
            check("W without R", WalkStatus::Misconfiguration, stop + 1);
        }
    }
    setup();
    descriptor(0, descriptor(0) & ~2ULL);
    descriptor(3, (descriptor(3) & ~7ULL) | 2);
    check("misconfiguration below denial", WalkStatus::Misconfiguration, 4,
          EptAccess::Write);
    setup();
    descriptor(1, descriptor(1) | 8);
    check("reserved non-leaf", WalkStatus::Misconfiguration, 2);
    setup();
    descriptor(2, descriptor(2) | (1ULL << 48));
    check("wide table address", WalkStatus::Misconfiguration, 3);
    for (unsigned leaf : {1, 2, 3}) {
        setup(4, leaf);
        descriptor(descriptors.size() - 1, descriptor(descriptors.size() - 1) |
                   (1ULL << 48));
        check("wide leaf address", WalkStatus::Misconfiguration, 5 - leaf);
    }
    for (unsigned leaf : {2, 3}) {
        setup(4, leaf);
        capability &= ~(1ULL << (leaf == 2 ? 16 : 17));
        check("unsupported leaf", WalkStatus::Misconfiguration, 5 - leaf);
        capability = Caps;
        descriptor(descriptors.size() - 1, descriptor(descriptors.size() - 1) |
                   4096);
        check("misaligned large page", WalkStatus::Misconfiguration, 5 - leaf);
    }
    for (uint8_t type : {0, 1, 2, 3, 4, 5, 6, 7}) {
        setup();
        descriptor(3, (descriptor(3) & ~(7ULL << 3)) | (type << 3) | 64);
        check("leaf type and IPAT", type == 2 || type == 3 || type == 7 ?
              WalkStatus::Misconfiguration : WalkStatus::Mapping, 4);
    }
    setup();
    check("before replacement", WalkStatus::Mapping, 4);
    mappingBase = 0x40000000;
    descriptor(3, mappingBase | 7 | (6 << 3));
    check("after replacement", WalkStatus::Mapping, 4);
    for (unsigned stop : {1, 3, 4}) {
        setup();
        errorRead = stop;
        check("memory error", WalkStatus::MemoryError, stop);
    }

    setup();
    check("invalid access", WalkStatus::InvalidInput, 0,
          static_cast<EptAccess>(99));
    for (unsigned width : {31, 53}) {
        physicalBits = width;
        check("invalid host width", WalkStatus::InvalidInput, 0);
    }
    for (unsigned levels : {4, 5}) {
        setup(levels);
        gpa |= 1ULL << (12 + 9 * levels);
        check("GPA outside walk width", WalkStatus::InvalidInput, 0);
    }
    for (unsigned field = 0; field < 5; ++field) {
        setup();
        switch (field) {
          case 0: config.root += 4096; break;
          case 1: config.walkLength = 5; break;
          case 2: config.memoryType = 0; break;
          case 3: config.accessedDirty = true; break;
          case 4: config.eptp |= 128; break;
        }
        check("inconsistent captured config", WalkStatus::InvalidInput, 0);
    }
    for (unsigned bit : {6, 14}) {
        setup();
        capability &= ~(1ULL << bit);
        check("unsupported EPTP capability", WalkStatus::InvalidInput, 0);
    }
    setup();
    config = *ept::decodeEptp(config.eptp | 64, capability, physicalBits);
    check("A/D not implemented", WalkStatus::UnsupportedConfig, 0);
    inform("EPT fixture: %u lookups passed (%s)", checks,
           system->isAtomicMode() ? "functional + atomic" : "functional only");
    if (system->isTimingMode()) {
        // Stop 0 cancels before admission. Other stops select the level's
        // request boundary, either rejected/queued or already issued.
        cases.emplace_back([this] { eptTimingLifecycle(false, true, 0); });
        for (unsigned stop : {1, 2, 4}) {
            cases.emplace_back([=, this] {
                eptTimingLifecycle(true, false, stop);
            });
            cases.emplace_back([=, this] {
                eptTimingLifecycle(true, true, stop);
            });
            cases.emplace_back([=, this] {
                eptTimingLifecycle(false, true, stop);
            });
        }
        cases.emplace_back([this] { eptTimingConcurrent(); });
    }
}

void
PagingTester::eptTimingLifecycle(bool retry, bool cancel, unsigned stop)
{
    setup(Long);
    constexpr uint64_t Caps = (1ULL << 6) | (1ULL << 14);
    auto config = *ept::decodeEptp(Tables | (3 << 3) | 6, Caps, 48);
    descriptor(3, descriptor(3) | (6 << 3));
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    const auto expected = ept::walkFunctional(port, requestor,
        ept::LookupInput(Virtual, EptAccess::Read, config), 48, Caps);
    std::vector<uint64_t> entries;
    for (unsigned i = 0; i < descriptors.size(); ++i)
        entries.push_back(descriptor(i));
    struct Progress
    {
        unsigned responses = 0;
        unsigned callbacks = 0;
        bool triggered = false;
    };
    auto progress = std::make_shared<Progress>();
    auto valid = std::make_shared<bool>(stop != 0);
    std::weak_ptr<bool> lifetime = valid;
    squash = [valid] { *valid = false; };
    inspectAttributes = [=, this](PacketPtr packet) {
        panic_if(pendingPackets || progress->responses >= 4 ||
                 packet->getAddr() != descriptors[progress->responses] ||
                 packet->cmd != MemCmd::ReadReq || packet->getSize() != 8,
                 "EPT lifecycle: unexpected read or overlapping requests");
        if (!progress->triggered && progress->responses + 1 == stop) {
            progress->triggered = true;
            injection = retry ? (cancel ? SquashRetryRead : RetryRead) :
                                SquashRead;
        }
    };
    inspectResponse = [=, this](PacketPtr packet) {
        panic_if(packet->getAddr() != descriptors[progress->responses],
                 "EPT lifecycle: response out of order");
        // Cancellation must discard even a failed response, not publish it.
        if (++progress->responses == stop && cancel && !retry)
            packet->setBadAddress();
    };
    const Tick start = curTick();
    awaiting = true;
    ept::walkTiming(port, requestor,
        ept::LookupInput(Virtual, EptAccess::Read, config), 48, Caps,
        [valid] { return *valid; },
        [=, this](const ept::WalkResult &result) {
            const unsigned reads = !cancel ? 4 :
                stop == 0 ? 0 : retry ? stop - 1 : stop;
            panic_if(++progress->callbacks != 1 || !awaiting ||
                     progress->responses != reads || pendingPackets ||
                     injected != (stop ? 1 : 0),
                     "EPT lifecycle: incorrect completion or read count");
            checkResult(result, cancel ?
                ept::WalkResult{ept::WalkStatus::Cancelled} : expected,
                "lifecycle");
            auto &port = tlb->getWalker()->pagingPort();
            panic_if(port.outstanding() != (stop ? 1 : 0) ||
                     (stop ? curTick() <= start : curTick() != start),
                     "EPT lifecycle: premature completion or retirement");
            for (unsigned i = 0; i < entries.size(); ++i)
                panic_if(descriptor(i) != entries[i],
                         "EPT lifecycle: table contents changed");
            inspectAttributes = {};
            inspectResponse = {};
            squash = {};
            awaiting = false;
            // Check after PagingPort releases its executing callback, not
            // inside it. A leaked walk/callback would retain the predicate.
            schedule(new EventFunctionWrapper([this, lifetime] {
                panic_if(!lifetime.expired() ||
                         !tlb->getWalker()->pagingPort().idle(),
                         "EPT lifecycle: retained state after completion");
                next();
            }, name() + ".ept_retired", true), curTick() + 1);
        });
    if (stop) {
        panic_if(!awaiting || port.outstanding() != 1,
                 "EPT lifecycle: request lost before completion");
    }
    if (retry && stop == 1) {
        // The root read is rejected and waiting for retry. Functional EPT
        // must still use the same port without consuming its pending owner.
        auto inspect = std::move(inspectAttributes);
        const auto functional = ept::walkFunctional(port, requestor,
            ept::LookupInput(Virtual, EptAccess::Read, config), 48, Caps);
        inspectAttributes = std::move(inspect);
        checkResult(functional, expected, "functional during retry");
        panic_if(!awaiting || port.outstanding() != 1 || curTick() != start,
                 "Functional EPT disturbed queued timing lookup");
    }
    // The caller's configuration and temporary LookupInput do not survive
    // this function. Outstanding reads must use their own captured copy.
    config = {};
}

void
PagingTester::eptTimingConcurrent()
{
    setup(Long);
    constexpr uint64_t Caps = (1ULL << 6) | (1ULL << 8) | (1ULL << 14);
    constexpr Addr Delta = 0x10000;
    const auto configA = *ept::decodeEptp(Tables | (3 << 3) | 6, Caps, 48);
    const auto configB = *ept::decodeEptp(
        (Tables + Delta) | (3 << 3), Caps, 48);
    descriptor(3, descriptor(3) | (6 << 3));
    const auto slots = descriptors;
    std::array<uint64_t, 4> entriesA, entriesB;
    for (unsigned i = 0; i < 4; ++i) {
        entriesA[i] = descriptor(i);
        entriesB[i] = i == 3 ? (Physical + 0x100000) | 7 | (6 << 3) :
                              entriesA[i] + Delta;
        system->physProxy.write<uint64_t>(slots[i] + Delta, entriesB[i],
                                         ByteOrder::little);
    }
    auto &port = tlb->getWalker()->pagingPort();
    const auto requestor = system->getRequestorId(this);
    const ept::LookupInput inputA(Virtual, EptAccess::Read, configA);
    const ept::LookupInput inputB(Virtual, EptAccess::Write, configB);
    const auto expectedA = ept::walkFunctional(
        port, requestor, inputA, 48, Caps);
    const auto expectedB = ept::walkFunctional(
        port, requestor, inputB, 48, Caps);
    struct Progress
    {
        std::array<unsigned, 3> responses{};
        std::array<bool, 3> done{};
        unsigned completed = 0;
    };
    auto progress = std::make_shared<Progress>();
    auto valid = std::make_shared<bool>(true);
    std::weak_ptr<bool> lifetime = valid;
    inspectAttributes = [=](PacketPtr packet) {
        const bool second = packet->getAddr() >= Tables + Delta;
        const unsigned client = second ? 1 : progress->done[0] ? 2 : 0;
        const unsigned level = progress->responses[client];
        panic_if(level >= 4 || packet->cmd != MemCmd::ReadReq ||
                 packet->getSize() != 8 || packet->getAddr() !=
                     slots[level] + (second ? Delta : 0) ||
                 packet->req->isUncacheable() != second,
                 "Concurrent EPT lookup mixed roots, attributes or levels");
    };
    inspectResponse = [=](PacketPtr packet) {
        const bool second = packet->getAddr() >= Tables + Delta;
        const unsigned client = second ? 1 : progress->done[0] ? 2 : 0;
        const unsigned level = progress->responses[client]++;
        panic_if(level >= 4 || packet->getAddr() !=
                     slots[level] + (second ? Delta : 0),
                 "Concurrent EPT response was routed to the wrong lookup");
    };
    const Tick start = curTick();
    const auto finish = [=, this](unsigned client,
                                  const ept::WalkResult &result) {
        panic_if(progress->done[client] || progress->responses[client] != 4 ||
                 curTick() <= start || !awaiting ||
                 tlb->getWalker()->pagingPort().idle(),
                 "Concurrent EPT completion lost ownership or repeated");
        checkResult(result, client == 1 ? expectedB : expectedA, "concurrent");
        progress->done[client] = true;
        if (++progress->completed != 3)
            return;
        for (unsigned i = 0; i < 4; ++i) {
            panic_if(descriptor(i) != entriesA[i] ||
                     system->physProxy.read<uint64_t>(slots[i] + Delta,
                         ByteOrder::little) != entriesB[i],
                     "Concurrent EPT lookup modified a table");
        }
        inspectAttributes = {};
        inspectResponse = {};
        awaiting = false;
        schedule(new EventFunctionWrapper([this, lifetime] {
            panic_if(!lifetime.expired() ||
                     !tlb->getWalker()->pagingPort().idle(),
                     "Concurrent EPT retained state after retirement");
            next();
        }, name() + ".ept_concurrent_retired", true), curTick() + 1);
    };
    awaiting = true;
    injection = RetryRead;
    ept::walkTiming(port, requestor, inputA, 48, Caps,
        [valid] { return *valid; },
        [=, this](const ept::WalkResult &result) {
            finish(0, result);
            // Reenter while the original completion is still port-owned.
            ept::walkTiming(tlb->getWalker()->pagingPort(), requestor,
                inputA, 48, Caps, [valid] { return *valid; },
                [finish](const ept::WalkResult &nested) {
                    finish(2, nested);
                });
        });
    ept::walkTiming(port, requestor, inputB, 48, Caps,
        [valid] { return *valid; },
        [finish](const ept::WalkResult &result) { finish(1, result); });
    panic_if(port.outstanding() != 2 || progress->completed,
             "Independent EPT lookups entered a guest admission queue");
}

} // namespace gem5::X86ISA
