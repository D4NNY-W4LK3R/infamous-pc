// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Runs the native mouse camera's code cave for real: a small generated harness sets the
// registers the game has at the camera store (xmm0 = pan / tilt, r12 = camera data, r13 = camera,
// [rsp+0x50] = context), jumps into the cave and reads back what it stored.

#include <array>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "core/mouse_aim_cave.h"

#ifdef MOUSE_AIM_CAVE_AVAILABLE

using Core::MouseAim::CameraCave;
using Core::MouseAim::Mailbox;

namespace {

struct alignas(16) Regs {
    float in[4];     // +0  xmm0 the game is about to store
    u64 r12;         // +16 camera data
    u64 r13;         // +24 camera
    u64 ctx;         // +32 context ([rsp+0x50])
    u64 pad;         // +40
    float out[4];    // +48 xmm0 after the cave
};

struct alignas(16) Limits {
    u8 bytes[0x1c0];
    float lower, upper;
};

struct alignas(16) Camera {
    u8 bytes[0x330];
    float stored[4];
};

class Harness : public Xbyak::CodeGenerator {
public:
    Harness() : Xbyak::CodeGenerator(4096) {}

    void Build(bool clamp) {
        cave_at = const_cast<u8*>(getCode()) + 2048;
        Xbyak::Label back;
#ifdef _WIN32
        const Xbyak::Reg64 p = rcx;
#else
        const Xbyak::Reg64 p = rdi;
#endif
        push(r12);
        push(r13);
        push(rbx);
        sub(rsp, 0x60);
        mov(rbx, p);
        mov(r11, qword[rbx + 32]);
        mov(qword[rsp + 0x50], r11);
        mov(r12, qword[rbx + 16]);
        mov(r13, qword[rbx + 24]);
        vmovups(xmm0, xword[rbx]);
        mov(rax, reinterpret_cast<u64>(cave_at));
        jmp(rax);
        L(back);
        vmovups(xword[rbx + 48], xmm0);
        add(rsp, 0x60);
        pop(rbx);
        pop(r13);
        pop(r12);
        ret();
        ready();
        cave = std::make_unique<CameraCave>(cave_at, 1024, &mailbox, back.getAddress(), clamp);
    }

    void Run(Regs& r) {
        getCode<void (*)(Regs*)>()(&r);
    }

    Mailbox mailbox{};
    u8* cave_at = nullptr;
    std::unique_ptr<CameraCave> cave;
};

class CameraCaveTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&data, 0, sizeof(data));
        std::memset(&ctx, 0, sizeof(ctx));
        std::memset(&cam, 0, sizeof(cam));
        data.lower = ctx.lower = -1.0f;
        data.upper = ctx.upper = 1.0f;
        h.Build(true);
    }

    std::array<float, 2> Store(float pan, float tilt, Camera* camera = nullptr) {
        Regs r{};
        r.in[0] = pan;
        r.in[1] = tilt;
        r.r12 = reinterpret_cast<u64>(&data);
        r.r13 = reinterpret_cast<u64>(camera ? camera : &cam);
        r.ctx = reinterpret_cast<u64>(&ctx);
        h.Run(r);
        const Camera* c = camera ? camera : &cam;
        EXPECT_EQ(c->stored[0], r.out[0]);
        EXPECT_EQ(c->stored[1], r.out[1]);
        return {r.out[0], r.out[1]};
    }

    void Motion(float yaw, float pitch) {
        float f[2] = {yaw, pitch};
        std::memcpy(&h.mailbox.delta, f, sizeof(f));
    }

    Harness h;
    Limits data{}, ctx{};
    Camera cam{};
};

} // namespace

TEST_F(CameraCaveTest, NoMotionStoresTheGamesValues) {
    const auto out = Store(0.3f, -0.2f);
    EXPECT_FLOAT_EQ(out[0], 0.3f);
    EXPECT_FLOAT_EQ(out[1], -0.2f);
    EXPECT_EQ(h.mailbox.frames, 1u);
    EXPECT_FLOAT_EQ(h.mailbox.tilt, -0.2f);
}

TEST_F(CameraCaveTest, MotionIsAddedOnceAndClampedToTheLimits) {
    Motion(0.1f, 0.25f);
    auto out = Store(0.0f, 0.5f);
    EXPECT_FLOAT_EQ(out[0], 0.1f);
    EXPECT_FLOAT_EQ(out[1], 0.75f);
    EXPECT_EQ(h.mailbox.delta, 0u); // taken
    out = Store(0.0f, 0.5f);         // nothing new: the game's own values
    EXPECT_FLOAT_EQ(out[1], 0.5f);
    Motion(0.0f, 0.9f);
    out = Store(0.0f, 0.5f);
    EXPECT_FLOAT_EQ(out[1], 1.0f); // upper limit
}

TEST_F(CameraCaveTest, WithoutHoldTheGamePullsTheTiltBack) {
    Motion(0.0f, 0.4f);
    Store(0.0f, 0.0f); // tilt 0.4 after the mouse
    const auto out = Store(0.0f, 0.3f); // the ledge camera pulls towards level
    EXPECT_FLOAT_EQ(out[1], 0.3f);
}

TEST_F(CameraCaveTest, HoldKeepsTheTiltTheMouseSet) {
    h.mailbox.hold = 1;
    Motion(0.0f, 0.4f);
    auto out = Store(0.0f, 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.4f);
    for (int i = 0; i < 10; ++i) { // the game keeps pulling towards level
        out = Store(0.05f * i, 0.4f - 0.04f * (i + 1));
        EXPECT_FLOAT_EQ(out[1], 0.4f);
        EXPECT_FLOAT_EQ(out[0], 0.05f * i); // the pan stays the game's
    }
    Motion(0.0f, -0.1f); // the mouse still moves it from there
    out = Store(0.5f, 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.3f);
    h.mailbox.hold = 0; // hold over: the game's tilt again
    out = Store(0.5f, 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
}

TEST_F(CameraCaveTest, HoldOnlyAppliesToTheSameCamera) {
    h.mailbox.hold = 1;
    Motion(0.0f, 0.4f);
    Store(0.0f, 0.0f);
    Camera other{};
    const auto out = Store(0.0f, -0.3f, &other); // another camera (a new mode or scene)
    EXPECT_FLOAT_EQ(out[1], -0.3f);
}

TEST_F(CameraCaveTest, HeldTiltStaysInsideNewLimits) {
    h.mailbox.hold = 1;
    Motion(0.0f, 0.7f);
    Store(0.0f, 0.2f); // 0.9
    data.upper = 0.5f; // the game narrows the tilt range
    const auto out = Store(0.0f, 0.2f);
    EXPECT_FLOAT_EQ(out[1], 0.5f);
}

#endif // MOUSE_AIM_CAVE_AVAILABLE
