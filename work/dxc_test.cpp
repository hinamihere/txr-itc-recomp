// Checks that dxcompiler.dll can be used in-process from MinGW (HLSL -> SPIR-V). Usage: dxc_test.exe file.hlsl ps_6_0
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include <dxcapi.h>
// MinGW ignores __declspec(uuid): give __uuidof the interface IDs explicitly.
__CRT_UUID_DECL(IDxcBlob, 0x8BA5FB08, 0x5195, 0x40e2, 0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02)
__CRT_UUID_DECL(IDxcBlobEncoding, 0x7241d424, 0x2646, 0x4191, 0x97, 0xc0, 0x98, 0xe9, 0x6e, 0x42, 0xfc, 0x68)
__CRT_UUID_DECL(IDxcBlobUtf8, 0x3DA636C9, 0xBA71, 0x4024, 0xA3, 0x01, 0x30, 0xCB, 0xF1, 0x25, 0x30, 0x5B)
__CRT_UUID_DECL(IDxcOperationResult, 0xCEDB484A, 0xD4E9, 0x445A, 0xB9, 0x91, 0xCA, 0x21, 0xCA, 0x15, 0x7D, 0xC2)
__CRT_UUID_DECL(IDxcUtils, 0x4605C4CB, 0x2019, 0x492A, 0xAD, 0xA4, 0x65, 0xF2, 0x0B, 0xB7, 0xD6, 0x7F)
__CRT_UUID_DECL(IDxcResult, 0x58346CDA, 0xDDE7, 0x4497, 0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59)
__CRT_UUID_DECL(IDxcCompiler3, 0x228B4687, 0x5A6A, 0x4730, 0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54)

int main(int argc, char** argv)
{
    if (argc < 3) return 1;
    HMODULE m = LoadLibraryA("C:\\itc-work\\deps\\dxc\\bin\\x64\\dxcompiler.dll");
    if (!m) { printf("cannot load dxcompiler.dll (%lu)\n", GetLastError()); return 2; }
    auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    IDxcUtils* utils = nullptr; IDxcCompiler3* comp = nullptr;
    HRESULT hr = create(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils);
    printf("utils hr=%08lX\n", hr);
    hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp);
    printf("compiler hr=%08lX\n", hr);
    if (!comp) return 3;
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); std::string src(n, 0); fread(&src[0], 1, n, f); fclose(f);
    std::wstring prof(argv[2], argv[2] + strlen(argv[2]));
    const wchar_t* args[] = { L"-E", L"main", L"-T", prof.c_str(), L"-spirv", L"-HV", L"2018", L"-fvk-use-dx-layout", L"-O3" };
    DxcBuffer buf = { src.data(), src.size(), DXC_CP_UTF8 };
    IDxcResult* res = nullptr;
    const DWORD t0 = GetTickCount();
    hr = comp->Compile(&buf, args, sizeof args / sizeof args[0], nullptr, __uuidof(IDxcResult), (void**)&res);
    printf("compile hr=%08lX in %lu ms\n", hr, GetTickCount() - t0);
    HRESULT st = E_FAIL; res->GetStatus(&st);
    IDxcBlob* obj = nullptr; res->GetResult(&obj);
    IDxcBlobEncoding* errs = nullptr; res->GetErrorBuffer(&errs);
    printf("status %08lX, spirv %zu bytes, errors: %.300s\n", st, obj ? obj->GetBufferSize() : 0, errs && errs->GetBufferSize() ? (const char*)errs->GetBufferPointer() : "");
    return 0;
}
