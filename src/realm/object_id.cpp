/*************************************************************************
 *
 * Copyright 2019 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#include <realm/object_id.hpp>
#include <realm/util/assert.hpp>
#include <chrono>
#include <atomic>
#include <random>

using namespace std::chrono;

namespace {
struct GeneratorState {
    // This just initializes all state randomly. The machine and process id fields are no longer supposed to use PIDs
    // or any machine-specific data, because that increases the probability of collisions.
    GeneratorState(std::random_device&& rnd = std::random_device{})
        : machine_id(rnd())
        , process_id(rnd())
        , seq(rnd())
    {
        static_assert(sizeof(std::random_device{}()) == 4, "4 bytes of random");
    }

    const int32_t machine_id;
    const int32_t process_id;
    std::atomic<uint32_t> seq;
} g_gen_state;
} // namespace


static const char hex_digits[] = "0123456789abcdef";

namespace realm {

ObjectId::ObjectId() noexcept
{
    memset(m_bytes, 0, sizeof(m_bytes));
}

ObjectId::ObjectId(const char* init)
{
    char buf[3];
    REALM_ASSERT(strlen(init) == 24);

    buf[2] = '\0';

    size_t j = 0;
    for (size_t i = 0; i < sizeof(m_bytes); i++) {
        buf[0] = init[j++];
        buf[1] = init[j++];
        m_bytes[i] = char(strtol(buf, nullptr, 16));
    }
}

ObjectId::ObjectId(Timestamp d, int machine_id, int process_id)
{
    auto sec = uint32_t(d.get_seconds());
    // Store in big endian so that memcmp can be used for comparison
    m_bytes[0] = sec >> 24;
    m_bytes[1] = (sec >> 16) & 0xff;
    m_bytes[2] = (sec >> 8) & 0xff;
    m_bytes[3] = sec & 0xff;

    memcpy(m_bytes + 4, &machine_id, 3);
    memcpy(m_bytes + 7, &process_id, 2);

    auto r = g_gen_state.seq.fetch_add(1, std::memory_order_relaxed);

    // Also store random number as big endian. This ensures that objects created
    // later within the same second will also be sorted correctly
    m_bytes[9] = (r >> 16) & 0xff;
    m_bytes[10] = (r >> 8) & 0xff;
    m_bytes[11] = r & 0xff;
}

ObjectId ObjectId::gen()
{
    return ObjectId(Timestamp(::time(nullptr), 0), g_gen_state.machine_id, g_gen_state.process_id);
}

Timestamp ObjectId::get_timestamp() const
{
    // Convert back to little endian
    uint32_t sec = (uint32_t(m_bytes[0]) << 24) + (uint32_t(m_bytes[1]) << 16) + (uint32_t(m_bytes[2]) << 8) +
                   uint32_t(m_bytes[3]);

    return Timestamp(sec, 0);
}

std::string ObjectId::to_string() const
{
    std::string ret;
    for (size_t i = 0; i < sizeof(m_bytes); i++) {
        ret += hex_digits[m_bytes[i] >> 4];
        ret += hex_digits[m_bytes[i] & 0xf];
    }
    return ret;
}


} // namespace realm