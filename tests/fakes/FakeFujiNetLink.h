// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifndef POM2_TEST_FAKE_FUJINET_LINK_H
#define POM2_TEST_FAKE_FUJINET_LINK_H

#include "FujiNetLink.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace pom2::test {

class FakeFujiNetLink final : public FujiNetLink
{
public:
    enum class Operation {
        None, Status, ReadBlock, WriteBlock, Format, Control,
        Init, Open, Close, Read, Write
    };

    void queueResponse(Response response)
    {
        responses.push_back(std::move(response));
    }

    bool isConnected() const override { return connected; }
    std::vector<SpDevice> devices() const override { return deviceList; }
    std::size_t deviceCount() const override { return deviceList.size(); }

    Response status(uint8_t unit, uint8_t statusCode) override
    {
        record(Operation::Status, unit, statusCode, 0, nullptr, 0);
        return takeResponse();
    }

    Response readBlock(uint8_t unit, uint32_t block) override
    {
        record(Operation::ReadBlock, unit, 0, block, nullptr, 0);
        return takeResponse();
    }

    Response writeBlock(uint8_t unit, uint32_t block,
                        const uint8_t* data, std::size_t n) override
    {
        record(Operation::WriteBlock, unit, 0, block, data, n);
        return takeResponse();
    }

    Response format(uint8_t unit) override
    {
        record(Operation::Format, unit, 0, 0, nullptr, 0);
        return takeResponse();
    }

    Response control(uint8_t unit, uint8_t controlCode,
                     const uint8_t* list, std::size_t n) override
    {
        record(Operation::Control, unit, controlCode, 0, list, n);
        return takeResponse();
    }

    Response init(uint8_t unit) override
    {
        record(Operation::Init, unit, 0, 0, nullptr, 0);
        return takeResponse();
    }

    Response open(uint8_t unit) override
    {
        record(Operation::Open, unit, 0, 0, nullptr, 0);
        return takeResponse();
    }

    Response close(uint8_t unit) override
    {
        record(Operation::Close, unit, 0, 0, nullptr, 0);
        return takeResponse();
    }

    Response read(uint8_t unit, uint16_t byteCount, uint32_t address) override
    {
        record(Operation::Read, unit, 0, address, nullptr, byteCount);
        requestedByteCount = byteCount;
        return takeResponse();
    }

    Response write(uint8_t unit, uint16_t byteCount, uint32_t address,
                   const uint8_t* data, std::size_t n) override
    {
        record(Operation::Write, unit, 0, address, data, n);
        requestedByteCount = byteCount;
        return takeResponse();
    }

    void notifyGuestReset() override { ++guestResetCount; }
    void resync() override { ++resyncCount; }

    bool connected = true;
    std::vector<SpDevice> deviceList;
    std::deque<Response> responses;
    Operation lastOperation = Operation::None;
    uint8_t lastUnit = 0;
    uint8_t lastCode = 0;
    uint32_t lastAddressOrBlock = 0;
    uint16_t requestedByteCount = 0;
    std::vector<uint8_t> lastPayload;
    int callCount = 0;
    int guestResetCount = 0;
    int resyncCount = 0;

private:
    Response takeResponse()
    {
        if (responses.empty()) return {true, kSpOk, {}};
        Response response = std::move(responses.front());
        responses.pop_front();
        return response;
    }

    void record(Operation operation, uint8_t unit, uint8_t code,
                uint32_t addressOrBlock, const uint8_t* data, std::size_t n)
    {
        ++callCount;
        lastOperation = operation;
        lastUnit = unit;
        lastCode = code;
        lastAddressOrBlock = addressOrBlock;
        if (data && n) lastPayload.assign(data, data + n);
        else lastPayload.clear();
    }
};

} // namespace pom2::test

#endif // POM2_TEST_FAKE_FUJINET_LINK_H
