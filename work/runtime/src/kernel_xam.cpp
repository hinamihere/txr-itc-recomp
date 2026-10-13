// XAM / networking / input imports: answers for an offline console with no controller and no signed-in user.
#include "guest.h"
#include <windows.h>
#include <string>
#include <tuple>
#include <vector>

enum : uint32_t { ERR_SUCCESS = 0, ERR_DEVICE_NOT_CONNECTED = 0x48F, ERR_NO_MORE_FILES = 0x12, ERR_NOT_FOUND = 0x490 };

// --- Network: offline ---------------------------------------------------------------------------------
HOST_FUNC(NetDll_XNetGetTitleXnAddr)
{
    // (caller, XNADDR* out) -> flags; 0 = pending, 1 = no address (offline)
    if (ctx.r4.u32) memset(GP(ctx.r4.u32), 0, 36);
    ctx.r3.u64 = 1;
}
HOST_FUNC(NetDll_WSAStartup) { ctx.r3.u64 = 0; }
HOST_FUNC(NetDll_WSAGetLastError) { ctx.r3.u64 = 10065; }   // WSAEHOSTUNREACH
HOST_FUNC(NetDll_socket) { ctx.r3.u64 = 0xFFFFFFFF; }       // INVALID_SOCKET
HOST_FUNC(NetDll_XNetXnAddrToMachineId) { ctx.r3.u64 = 0x80070057; }
HOST_FUNC(NetDll_XNetXnAddrToInAddr) { ctx.r3.u64 = 0x80070057; }

// --- Input: user 0 = keyboard + any connected XInput pad ------------------------------------------------------------------
// Keyboard (only while the game window has focus):
//   Enter = START, Backspace = BACK, arrows = D-pad, A/D = steer (left stick X), W = accelerate (right trigger), S = brake / reverse (left trigger),
//   W/S also push the left stick up/down (menu navigation), I/J/K/L = right stick,
//   Space or Z = A, X = B, C = X, V = Y, Q / E = left / right bumper, Left Shift = right trigger, Left Ctrl = left trigger.
bool XGpuWindowFocused();
uint32_t XGpuFrame();
void XGpuWantThumb(bool want);
bool XGpuGetThumb(uint8_t* out, uint32_t* frame);

namespace {
struct HostPad { uint16_t buttons = 0; uint8_t lt = 0, rt = 0; int16_t lx = 0, ly = 0, rx = 0, ry = 0; };

struct XInputState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD (WINAPI* XInputGetStateFn)(DWORD, XInputState*);

HostPad ReadHostPad()
{
    static XInputGetStateFn fn = []() -> XInputGetStateFn {
        const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        for (const char* d : dlls) if (HMODULE m = LoadLibraryA(d)) return (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
        return nullptr;
    }();
    HostPad pad;
    // The physical pad is only read while the game window has focus (XInput is global: an unfocused instance would otherwise react to
    // another instance's / application's controller input).
    static const bool noXInput = getenv("ITC_NO_XINPUT") != nullptr;
    if (fn && !noXInput && XGpuWindowFocused())
    {
        for (DWORD i = 0; i < 4; i++)
        {
            XInputState st{};
            if (fn(i, &st) == 0)
            {
                pad.buttons = st.buttons; pad.lt = st.lt; pad.rt = st.rt;
                pad.lx = st.lx; pad.ly = st.ly; pad.rx = st.rx; pad.ry = st.ry;
                break;
            }
        }
    }
    {
        // Debug: ITC_PAD_SCRIPT="frame:BUTTON:frames,..." presses buttons at given presented frames (START BACK A B X Y UP DOWN LEFT RIGHT LB RB, RT LT = triggers, LSL LSR = steer left / right).
        // "WAIT:<name>" waits until the presented frame looks like refs/<name>.thumb (64x36 grey thumbnail); the items after it use frame
        // numbers relative to that moment. Example: "WAIT:title,10:START:15,WAIT:prompt,30:DOWN:8,90:A:8".
        struct Item { uint32_t start; uint32_t mask; uint32_t len; };   // mask bits 16..19: RT, LT, left stick left, left stick right
        struct Segment { std::string wait; std::vector<Item> items; std::vector<uint8_t> ref; bool started = false; uint32_t base = 0; };
        static std::vector<Segment> segs = []() {
            std::vector<Segment> v(1);
            const char* e = getenv("ITC_PAD_SCRIPT");
            if (!e) return v;
            std::string str = e;
            size_t pos = 0;
            while (pos < str.size())
            {
                size_t end = str.find(',', pos); if (end == std::string::npos) end = str.size();
                std::string item = str.substr(pos, end - pos);
                pos = end + 1;
                if (item.compare(0, 5, "WAIT:") == 0)
                {
                    Segment sg; sg.wait = item.substr(5);
                    std::string path = "refs/" + sg.wait + ".thumb";
                    if (FILE* f = fopen(path.c_str(), "rb")) { sg.ref.resize(64 * 36); if (fread(sg.ref.data(), 1, sg.ref.size(), f) != sg.ref.size()) sg.ref.clear(); fclose(f); }
                    if (sg.ref.empty()) Log("[pad] reference screen '%s' not found (%s)", sg.wait.c_str(), path.c_str());
                    v.push_back(std::move(sg));
                    continue;
                }
                const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
                if (c1 == std::string::npos || c2 == std::string::npos) continue;
                const std::string name = item.substr(c1 + 1, c2 - c1 - 1);
                static const std::pair<const char*, uint32_t> names[] = { {"START", 0x10}, {"BACK", 0x20}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
                                                                           {"UP", 1}, {"DOWN", 2}, {"LEFT", 4}, {"RIGHT", 8}, {"LB", 0x100}, {"RB", 0x200},
                                                                           {"RT", 0x10000}, {"LT", 0x20000}, {"LSL", 0x40000}, {"LSR", 0x80000} };
                uint32_t mask = 0;
                for (auto& n : names) if (name == n.first) mask = n.second;
                v.back().items.push_back({ (uint32_t)atoi(item.c_str()), mask, (uint32_t)atoi(item.c_str() + c2 + 1) });
            }
            return v;
        }();
        static size_t cur = 0;
        const uint32_t f = XGpuFrame();
        if (segs.size() > 1) XGpuWantThumb(cur + 1 < segs.size());
        if (cur + 1 < segs.size())
        {
            Segment& nx = segs[cur + 1];
            uint8_t th[64 * 36]; uint32_t tf = 0;
            if (!nx.ref.empty() && XGpuGetThumb(th, &tf))
            {
                uint64_t diff = 0;
                for (int i = 0; i < 64 * 36; i++) diff += (uint64_t)abs((int)th[i] - (int)nx.ref[i]);
                static uint32_t lastLogged = 0;
                if (f - lastLogged >= 120) { lastLogged = f; Log("[pad] waiting for '%s': frame %u mean diff %.1f", nx.wait.c_str(), f, diff / (64.0 * 36.0)); }
                if (diff / (64.0 * 36.0) < 6.0) { cur++; nx.started = true; nx.base = f; Log("[pad] screen '%s' seen at frame %u", nx.wait.c_str(), f); }
            }
        }
        {
            const Segment& sg = segs[cur];
            const uint32_t rel = (cur == 0) ? f : f - sg.base;
            for (const Item& it : sg.items)
                if (rel >= it.start && rel < it.start + it.len)
                {
                    pad.buttons |= (uint16_t)it.mask;
                    if (it.mask & 0x10000) pad.rt = 255;
                    if (it.mask & 0x20000) pad.lt = 255;
                    if (it.mask & 0x40000) pad.lx = -32767;
                    if (it.mask & 0x80000) pad.lx = 32767;
                }
        }
    }
    if (XGpuWindowFocused())
    {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_UP)) pad.buttons |= 0x0001;
        if (down(VK_DOWN)) pad.buttons |= 0x0002;
        if (down(VK_LEFT)) pad.buttons |= 0x0004;
        if (down(VK_RIGHT)) pad.buttons |= 0x0008;
        if (down(VK_RETURN)) pad.buttons |= 0x0010;
        if (down(VK_BACK)) pad.buttons |= 0x0020;
        if (down('Q')) pad.buttons |= 0x0100;
        if (down('E')) pad.buttons |= 0x0200;
        if (down(VK_SPACE) || down('Z')) pad.buttons |= 0x1000;
        if (down('X')) pad.buttons |= 0x2000;
        if (down('C')) pad.buttons |= 0x4000;
        if (down('V')) pad.buttons |= 0x8000;
        if (down(VK_LSHIFT)) pad.rt = 255;
        if (down(VK_LCONTROL)) pad.lt = 255;
        if (down('A')) pad.lx = -32767;
        if (down('D')) pad.lx = 32767;
        if (down('W')) { pad.ly = 32767; pad.rt = 255; }    // accelerator (menus: up)
        if (down('S')) { pad.ly = -32767; pad.lt = 255; }   // brake / reverse (menus: down)
        if (down('J')) pad.rx = -32767;
        if (down('L')) pad.rx = 32767;
        if (down('I')) pad.ry = 32767;
        if (down('K')) pad.ry = -32767;
    }
    return pad;
}
}  // namespace

HOST_FUNC(XamInputGetState)
{
    // (user, flags, XINPUT_STATE* out)
    const uint32_t user = ctx.r3.u32;
    if (user != 0 && user != 0xFF && user != 0xFFFFFFFF) { ctx.r3.u64 = ERR_DEVICE_NOT_CONNECTED; return; }
    const HostPad pad = ReadHostPad();
    static uint32_t packet = 0;
    static HostPad last;
    if (memcmp(&pad, &last, sizeof pad) != 0) { packet++; last = pad; }
    const uint32_t out = ctx.r5.u32;
    if (out)
    {
        wr32(out, packet);
        wr16(out + 4, pad.buttons);
        wr8(out + 6, pad.lt); wr8(out + 7, pad.rt);
        wr16(out + 8, (uint16_t)pad.lx); wr16(out + 10, (uint16_t)pad.ly);
        wr16(out + 12, (uint16_t)pad.rx); wr16(out + 14, (uint16_t)pad.ry);
    }
    ctx.r3.u64 = ERR_SUCCESS;
}

HOST_FUNC(XamInputGetCapabilities)
{
    // (user, flags, XINPUT_CAPABILITIES* out): a standard gamepad
    const uint32_t user = ctx.r3.u32;
    if (user != 0 && user != 0xFF && user != 0xFFFFFFFF) { ctx.r3.u64 = ERR_DEVICE_NOT_CONNECTED; return; }
    const uint32_t out = ctx.r5.u32;
    if (out)
    {
        memset(GP(out), 0, 20);
        wr8(out + 0, 1);          // Type: gamepad
        wr8(out + 1, 1);          // SubType: gamepad
        wr16(out + 2, 0);         // Flags
        wr16(out + 4, 0xF3FF);    // buttons
        wr8(out + 6, 255); wr8(out + 7, 255);
        wr16(out + 8, 0xFFC0); wr16(out + 10, 0xFFC0); wr16(out + 12, 0xFFC0); wr16(out + 14, 0xFFC0);
        wr16(out + 16, 0xFFFF); wr16(out + 18, 0xFFFF);   // vibration
    }
    ctx.r3.u64 = ERR_SUCCESS;
}

HOST_FUNC(XamInputSetState) { ctx.r3.u64 = ERR_SUCCESS; }   // rumble: ignored

// --- Notifications / users ---------------------------------------------------------------------------------
HOST_FUNC(XNotifyGetNext) { ctx.r3.u64 = 0; }                    // nothing pending
HOST_FUNC(XamNotifyCreateListener) { ctx.r3.u64 = 0x1000; }      // any non-zero handle
HOST_FUNC(XamUserGetSigninState) { ctx.r3.u64 = 0; }             // not signed in
HOST_FUNC(XamUserCheckPrivilege)
{
    // (user, privilege, BOOL* result)
    if (ctx.r5.u32) wr32(ctx.r5.u32, 0);
    ctx.r3.u64 = ERR_SUCCESS;
}
HOST_FUNC(XamGetSystemVersion) { ctx.r3.u64 = 0x20000000 | 17559; }
HOST_FUNC(XamGetExecutionId) { ctx.r3.u64 = 0x80070057; }
HOST_FUNC(XamLoaderTerminateTitle) { Log("[xam] XamLoaderTerminateTitle"); ExitProcess(0); }
HOST_FUNC(XamLoaderLaunchTitle) { Log("[xam] XamLoaderLaunchTitle"); ExitProcess(0); }
HOST_FUNC(XamAlloc)
{
    // (flags, size) -> pointer
    ctx.r3.u64 = GuestAlloc(ctx.r4.u32 ? ctx.r4.u32 : 0x1000, 0x1000);
}
HOST_FUNC(XamFree) { GuestFree(ctx.r3.u32); ctx.r3.u64 = 0; }
HOST_FUNC(XamVoiceHeadsetPresent) { ctx.r3.u64 = 0; }
