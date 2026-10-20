/*
 * Copyright (c) 2026 Seaver Olson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/packet_access.hh"

namespace gem5
{

TEST(Packet, FunctionalAccessSkipsAtomicInputsAndOldResults)
{
    GTestTickHandler ticks;
    for (auto flags :
         {Request::MEM_SWAP, Request::MEM_SWAP_COND, Request::ATOMIC_RETURN_OP,
          Request::ATOMIC_NO_RETURN_OP, Request::LOCKED_RMW}) {
        for (bool response : {false, true}) {
            auto req = std::make_shared<Request>(0x1000, 8, flags, 0);
            Packet atomic(req, flags == Request::LOCKED_RMW
                                   ? MemCmd::LockedRMWReadReq
                                   : MemCmd::SwapReq);
            atomic.allocate();
            atomic.setLE<uint64_t>(0x1234);
            if (response) {
                atomic.makeResponse();
            }
            auto probe_req = std::make_shared<Request>(0x1000, 8, 0, 0);
            Packet read(probe_req, MemCmd::ReadReq);
            read.allocate();
            read.setLE<uint64_t>(0x5678);
            EXPECT_FALSE(read.trySatisfyFunctional(&atomic));
            EXPECT_EQ(read.getLE<uint64_t>(), 0x5678);
            Packet write(probe_req, MemCmd::WriteReq);
            write.allocate();
            write.setLE<uint64_t>(0xabcd);
            EXPECT_FALSE(write.trySatisfyFunctional(&atomic));
            EXPECT_EQ(atomic.getLE<uint64_t>(), 0x1234);
        }
    }
}

TEST(Packet, FunctionalAccessStillForwardsOrdinaryWriteData)
{
    GTestTickHandler ticks;
    auto req = std::make_shared<Request>(0x1000, 8, 0, 0);
    Packet pending(req, MemCmd::WriteReq);
    pending.allocate();
    pending.setLE<uint64_t>(0x1234);
    Packet read(req, MemCmd::ReadReq);
    read.allocate();
    EXPECT_TRUE(read.trySatisfyFunctional(&pending));
    EXPECT_EQ(read.getLE<uint64_t>(), 0x1234);
    Packet write(req, MemCmd::WriteReq);
    write.allocate();
    write.setLE<uint64_t>(0x5678);
    EXPECT_FALSE(write.trySatisfyFunctional(&pending));
    EXPECT_EQ(pending.getLE<uint64_t>(), 0x5678);
}

} // namespace gem5
