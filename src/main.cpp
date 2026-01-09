#include "../deps/IL2CPP_Resolver/IL2CPP_Resolver.hpp"
#include "../deps/imgui/backends/imgui_impl_dx11.h"
#include "../deps/imgui/backends/imgui_impl_win32.h"
#include "../deps/imgui/imgui.h"
#include "../deps/minhook/include/MinHook.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

// Tyedefs for hooked functions
typedef HRESULT(WINAPI *Present)(IDXGISwapChain *pSwapChain, UINT SyncInterval,
                                 UINT Flags);
typedef HRESULT(WINAPI *ResizeBuffers)(IDXGISwapChain *pSwapChain,
                                       UINT BufferCount, UINT Width,
                                       UINT Height, DXGI_FORMAT NewFormat,
                                       UINT SwapChainFlags);
typedef void(WINAPI *DrawIndexed)(ID3D11DeviceContext *pContext,
                                  UINT IndexCount, UINT StartIndexLocation,
                                  INT BaseVertexLocation);
typedef LRESULT(CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);

// Original function pointers
static Present oPresent = NULL;
static ResizeBuffers oResizeBuffers = NULL;
WNDPROC oWndProc = NULL;

// Global variables
ID3D11Device *pDevice = NULL;
ID3D11DeviceContext *pContext = NULL;
ID3D11RenderTargetView *mainRenderTargetView = NULL;
HWND window = NULL;
bool init = false;
bool showMenu = true;

// Hack settings
bool g_DrawEsp = false;
bool g_ShowBaseAiESP = false;
bool g_ShowBaseAiHP = true;
bool g_ShowBaseAiName = true;

bool g_ShowGearItemESP = false;
bool g_ShowHarvestableESP = false;

float g_EspDistance = 250.0f;

static void *(*Camera_get_currentInternal)();
static void *(*GameManager_GetVpFPSCamera)();
static Unity::Vector3 (*Camera_WorldToScreenPoint)(void *camera,
                                                   Unity::Vector3 position,
                                                   int eye);
static void *(*Component_get_transform)(void *_this);
static void *(*Component_get_gameObject)(void *_this);
static void *(*GameObject_get_transform)(void *_this);
static bool (*GameObject_get_activeInHierarchy)(void *_this);
static Unity::Vector3 (*Transform_get_position)(void *_this);
static Unity::System_String *(*GearItem_get_DisplayNameWithCondition)(
    void *_this);
std::vector<void *> g_BaseAiList;
std::vector<void *> g_GearItemList;
std::vector<void *> g_HarvestableList;
std::mutex g_BaseAiMutex;
std::mutex g_GearItemMutex;
std::mutex g_HarvestableMutex;

uintptr_t off_CurrentHP;
uintptr_t off_MaxHP;
uintptr_t off_DisplayName;
uintptr_t off_Camera;

float Vector3_Distance(const Unity::Vector3 &v1, const Unity::Vector3 &v2) {
  float deltaX = v1.x - v2.x;
  float deltaY = v1.y - v2.y;
  float deltaZ = v1.z - v2.z;
  return std::sqrtf(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
}

// Forward declarations
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam,
                         LPARAM lParam) {
  if (showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
    return true;

  return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

using tBaseAiManagerAdd = void (*)(void *);
tBaseAiManagerAdd oBaseAiManagerAdd;

void hkBaseAiManagerAdd(void *__this) {
  oBaseAiManagerAdd(__this);

  std::lock_guard<std::mutex> lock(g_BaseAiMutex);
  g_BaseAiList.push_back(__this);
}

using tBaseAiManagerRemove = void (*)(void *);
tBaseAiManagerRemove oBaseAiManagerRemove;

void hkBaseAiManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_BaseAiMutex);
    auto it = std::find(g_BaseAiList.begin(), g_BaseAiList.end(), __this);

    if (it != g_BaseAiList.end()) {
      std::swap(*it,
                g_BaseAiList.back()); // Swap the element with the last one
      g_BaseAiList.pop_back();        // Remove the last element
    }
  }
  oBaseAiManagerRemove(__this);
}

using tGearManagerAdd = void (*)(void *);
tGearManagerAdd oGearManagerAdd;

void hkGearManagerAdd(void *__this) {
  oGearManagerAdd(__this);

  std::lock_guard<std::mutex> lock(g_GearItemMutex);
  g_GearItemList.push_back(__this);
}

using tGearManagerRemove = void (*)(void *);
tGearManagerRemove oGearManagerRemove;

void hkGearManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_GearItemMutex);
    auto it = std::find(g_GearItemList.begin(), g_GearItemList.end(), __this);

    if (it != g_GearItemList.end()) {
      std::swap(*it,
                g_GearItemList.back()); // Swap the element with the last one
      g_GearItemList.pop_back();        // Remove the last element
    }
  }
  oGearManagerRemove(__this);
}

using tHarvestableManagerAdd = void (*)(void *);
tHarvestableManagerAdd oHarvestableManagerAdd;

void hkHarvestableManagerAdd(void *__this) {
  oHarvestableManagerAdd(__this);

  std::lock_guard<std::mutex> lock(g_HarvestableMutex);
  g_HarvestableList.push_back(__this);
}

using tHarvestableManagerRemove = void (*)(void *);
tHarvestableManagerRemove oHarvestableManagerRemove;

void hkHarvestableManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_HarvestableMutex);
    auto it =
        std::find(g_HarvestableList.begin(), g_HarvestableList.end(), __this);

    if (it != g_HarvestableList.end()) {
      std::swap(*it,
                g_HarvestableList.back()); // Swap the element with the last one
      g_HarvestableList.pop_back();        // Remove the last element
    }
  }
  oHarvestableManagerRemove(__this);
}

float GetCurrentHP(void *ai) {
  return *(float *)((uintptr_t)ai + off_CurrentHP);
}

float GetMaxHP(void *ai) { return *(float *)((uintptr_t)ai + off_MaxHP); }

void *vp_FPSCamera_GetCamera(void *vp_FPSCamera) {
  return *(void **)((uintptr_t)vp_FPSCamera + off_Camera);
}

std::string GetDisplayName(void *ai) {
  auto ustr = *(Unity::System_String **)((uintptr_t)ai + off_DisplayName);
  return ustr ? ustr->ToString() : "Unknown";
}

std::string GetGearItemDisplayName(void *gearItem) {
  auto ustr = GearItem_get_DisplayNameWithCondition(gearItem);
  return ustr ? ustr->ToString() : "Unknown";
}

Unity::Vector3 GetGearItemWorldPosition(Unity::CComponent *gearItem) {
  if (gearItem == nullptr) {
    return {-9999.0f, -9999.0f, -9999.0f};
  }
  auto gameObject = Component_get_gameObject(gearItem);
  if (gameObject == nullptr) {
    return {-9999.0f, -9999.0f, -9999.0f};
  }

  if (!GameObject_get_activeInHierarchy(gameObject)) {
    return {-9999.0f, -9999.0f, -9999.0f};
  }

  auto transform = GameObject_get_transform(gameObject);
  auto pos = Transform_get_position(transform);

  return pos;
}

Unity::Vector3 GetWorldPosition(Unity::CComponent *ai) {
  auto transform = Component_get_transform(ai);
  auto pos = Transform_get_position(transform);

  return pos;
}

Unity::Vector3 GetCameraPosition(void *camera) {
  auto transform = Component_get_transform(camera);
  auto pos = Transform_get_position(transform);

  return pos;
}

void DrawESP() {
  if (!g_DrawEsp)
    return;

  auto camera = Camera_get_currentInternal();
  ImDrawList *draw = ImGui::GetForegroundDrawList();
  Unity::Vector3 camera_position = GetCameraPosition(camera);

  if (g_ShowBaseAiESP) {
    std::lock_guard<std::mutex> lock(g_BaseAiMutex);
    for (auto &ai : g_BaseAiList) {
      Unity::Vector3 worldPos =
          GetWorldPosition(reinterpret_cast<Unity::CComponent *>(ai));
      Unity::Vector3 screenPos{};

      screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);

      if (screenPos.z < 0.1f)
        continue;

      float dist = Vector3_Distance(camera_position, worldPos);
      if (dist > g_EspDistance)
        continue;

      std::string text = "* ";
      if (g_ShowBaseAiHP) {
        int hp = (int)GetCurrentHP(ai);
        int maxHp = (int)GetMaxHP(ai);

        char hpBuf[32];
        char maxHpBuf[32];

        sprintf_s(hpBuf, "%d", hp);
        sprintf_s(maxHpBuf, "%d", maxHp);

        text += "[";
        text += hpBuf;
        text += "/";
        text += maxHpBuf;
        text += "]";
      }

      char distBuf[32];
      sprintf_s(distBuf, "%.1f", dist);
      text += " [";
      text += distBuf;
      text += "m.] ";
      if (g_ShowBaseAiName)
        text += GetDisplayName(ai);

      ImGuiIO &io = ImGui::GetIO();
      float x = screenPos.x;
      float y = io.DisplaySize.y - screenPos.y;

      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 100, 100, 255),
                    text.c_str());
    }
  }

  if (g_ShowGearItemESP) {
    std::lock_guard<std::mutex> lock(g_GearItemMutex);
    for (auto &item : g_GearItemList) {
      Unity::Vector3 worldPos =
          GetGearItemWorldPosition(reinterpret_cast<Unity::CComponent *>(item));
      if (worldPos.x == -9999.0f && worldPos.y == -9999.0f &&
          worldPos.z == -9999.0f) {
        continue;
      }
      Unity::Vector3 screenPos{};

      screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = Vector3_Distance(camera_position, worldPos);
      if (dist > g_EspDistance)
        continue;

      std::string text = "* ";

      char distBuf[32];
      sprintf_s(distBuf, "%.1f", dist);
      text += " [";
      text += distBuf;
      text += "m.] ";
      text += GetGearItemDisplayName(item);

      ImGuiIO &io = ImGui::GetIO();
      float x = screenPos.x;
      float y = io.DisplaySize.y - screenPos.y;

      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 255, 100, 255),
                    text.c_str());
    }
  }

  if (g_ShowHarvestableESP) {
    std::lock_guard<std::mutex> lock(g_HarvestableMutex);
    for (auto &harvestable : g_HarvestableList) {
      Unity::Vector3 worldPos = GetGearItemWorldPosition(
          reinterpret_cast<Unity::CComponent *>(harvestable));
      if (worldPos.x == -9999.0f && worldPos.y == -9999.0f &&
          worldPos.z == -9999.0f) {
        continue;
      }
      Unity::Vector3 screenPos{};

      screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = Vector3_Distance(camera_position, worldPos);
      if (dist > g_EspDistance)
        continue;

      std::string text = "* ";

      char distBuf[32];
      sprintf_s(distBuf, "%.1f", dist);
      text += "[";
      text += distBuf;
      text += "m.] ";

      ImGuiIO &io = ImGui::GetIO();
      float x = screenPos.x;
      float y = io.DisplaySize.y - screenPos.y;

      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 255, 100, 255),
                    text.c_str());
    }
  }
}

HRESULT WINAPI hkPresent(IDXGISwapChain *pSwapChain, UINT SyncInterval,
                         UINT Flags) {
  if (!init) {
    if (SUCCEEDED(
            pSwapChain->GetDevice(__uuidof(ID3D11Device), (void **)&pDevice))) {
      pDevice->GetImmediateContext(&pContext);
      DXGI_SWAP_CHAIN_DESC sd;
      pSwapChain->GetDesc(&sd);
      window = sd.OutputWindow;
      ID3D11Texture2D *pBackBuffer;
      pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            (LPVOID *)&pBackBuffer);
      pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
      pBackBuffer->Release();

      oWndProc =
          (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);

      ImGui::CreateContext();
      ImGuiIO &io = ImGui::GetIO();
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

      ImGui_ImplWin32_Init(window);
      ImGui_ImplDX11_Init(pDevice, pContext);
      init = true;
    } else
      return oPresent(pSwapChain, SyncInterval, Flags);
  }

  if (GetAsyncKeyState(VK_INSERT) & 1) {
    showMenu = !showMenu;
  }
  g_DrawEsp = g_ShowBaseAiESP || g_ShowGearItemESP || g_ShowHarvestableESP;

  if (g_DrawEsp || showMenu) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawESP();
    if (showMenu) {

      ImGui::Begin("Fubuki-ESP | The Long Dark", &showMenu,
                   ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::Text("Menu Controls:");
      ImGui::BulletText("INSERT: Toggle Menu");
      ImGui::BulletText("END: Unload DLL (Safely)");

      ImGui::Separator();

      ImGui::Checkbox("BaseAi ESP", &g_ShowBaseAiESP);
      ImGui::Checkbox("Show HP", &g_ShowBaseAiHP);
      ImGui::Checkbox("Show Name", &g_ShowBaseAiName);
      ImGui::Text("Found %zu BaseAi", g_BaseAiList.size());

      ImGui::Checkbox("GearItem ESP", &g_ShowGearItemESP);
      ImGui::Text("Found %zu GearItem", g_GearItemList.size());

      ImGui::Checkbox("Harvestable ESP", &g_ShowHarvestableESP);
      ImGui::Text("Found %zu Harvestable", g_HarvestableList.size());

      ImGui::SliderFloat("ESP Distance", &g_EspDistance, 10.0f, 1000.0f);

      ImGui::Separator();
      if (ImGui::Button("Unload DLL", ImVec2(120, 0))) {
        // Signal unload
      }
      ImGui::End();
    }

    ImGui::Render();

    pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  }

  return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT WINAPI hkResizeBuffers(IDXGISwapChain *pSwapChain, UINT BufferCount,
                               UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                               UINT SwapChainFlags) {
  if (mainRenderTargetView) {
    pContext->OMSetRenderTargets(0, 0, 0);
    mainRenderTargetView->Release();
  }

  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);

  ID3D11Texture2D *pBackBuffer;
  pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID *)&pBackBuffer);
  if (SUCCEEDED(hr)) {
    pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
    pBackBuffer->Release();
    pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
  }

  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)Width, (float)Height);

  return hr;
}

bool InitBaseAiOffsets() {
  off_CurrentHP = IL2CPP::Class::Utils::GetFieldOffset("BaseAi", "m_CurrentHP");
  off_MaxHP = IL2CPP::Class::Utils::GetFieldOffset("BaseAi", "m_MaxHP");
  off_DisplayName =
      IL2CPP::Class::Utils::GetFieldOffset("BaseAi", "m_DisplayName");
  off_Camera = IL2CPP::Class::Utils::GetFieldOffset("vp_FPSCamera", "m_Camera");
  return off_CurrentHP != -1 && off_MaxHP != -1 && off_DisplayName != -1 &&
         off_Camera != -1;
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
  bool g_Running = true;

  // Create a dummy swapchain to get the vtable offsets
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
  DXGI_SWAP_CHAIN_DESC scd;
  ZeroMemory(&scd, sizeof(scd));
  scd.BufferCount = 1;
  scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scd.OutputWindow = GetForegroundWindow(); // Just a dummy window
  scd.SampleDesc.Count = 1;
  scd.Windowed = TRUE;
  scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  ID3D11Device *pDummyDevice = NULL;
  ID3D11DeviceContext *pDummyContext = NULL;
  IDXGISwapChain *pDummySwapChain = NULL;

  if (FAILED(D3D11CreateDeviceAndSwapChain(
          NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &featureLevel, 1,
          D3D11_SDK_VERSION, &scd, &pDummySwapChain, &pDummyDevice, NULL,
          &pDummyContext))) {
    return 1;
  }

  void **pVTable = *(void ***)pDummySwapChain;
  void **pContextVTable = *(void ***)pDummyContext;

  void *pPresent = pVTable[8];
  void *pResizeBuffers = pVTable[13];

  if (MH_Initialize() != MH_OK)
    return 1;

  if (MH_OK !=
      MH_CreateHook(pPresent, &hkPresent, reinterpret_cast<void **>(&oPresent)))
    return 1;

  if (MH_OK != MH_CreateHook(pResizeBuffers, &hkResizeBuffers,
                             reinterpret_cast<void **>(&oResizeBuffers)))
    return 1;
  if (!IL2CPP::Initialize())
    return 1;

  if (!InitBaseAiOffsets())
    return 1;

  auto gearManagerAddPtr =
      IL2CPP::Class::Utils::GetMethodPointer("GearManager", "Add", 1);
  if (MH_OK != MH_CreateHook(gearManagerAddPtr, &hkGearManagerAdd,
                             reinterpret_cast<void **>(&oGearManagerAdd)))
    return 1;

  auto gearManagerRemovePtr =
      IL2CPP::Class::Utils::GetMethodPointer("GearManager", "Remove", 1);
  if (MH_OK != MH_CreateHook(gearManagerRemovePtr, &hkGearManagerRemove,
                             reinterpret_cast<void **>(&oGearManagerRemove)))
    return 1;

  auto baseAiManagerAddPtr =
      IL2CPP::Class::Utils::GetMethodPointer("BaseAiManager", "Add", 1);
  if (MH_OK != MH_CreateHook(baseAiManagerAddPtr, &hkBaseAiManagerAdd,
                             reinterpret_cast<void **>(&oBaseAiManagerAdd)))
    return 1;

  auto baseAiManagerRemovePtr =
      IL2CPP::Class::Utils::GetMethodPointer("BaseAiManager", "Remove", 1);
  if (MH_OK != MH_CreateHook(baseAiManagerRemovePtr, &hkBaseAiManagerRemove,
                             reinterpret_cast<void **>(&oBaseAiManagerRemove)))
    return 1;

  auto harvestableManagerAddPtr =
      IL2CPP::Class::Utils::GetMethodPointer("HarvestableManager", "Add", 1);
  if (MH_OK !=
      MH_CreateHook(harvestableManagerAddPtr, &hkHarvestableManagerAdd,
                    reinterpret_cast<void **>(&oHarvestableManagerAdd)))
    return 1;

  auto harvestableManagerRemovePtr =
      IL2CPP::Class::Utils::GetMethodPointer("HarvestableManager", "Remove", 1);
  if (MH_OK !=
      MH_CreateHook(harvestableManagerRemovePtr, &hkHarvestableManagerRemove,
                    reinterpret_cast<void **>(&oHarvestableManagerRemove)))
    return 1;

  if (MH_OK != MH_EnableHook(MH_ALL_HOOKS))
    return 1;

  auto base_addr = (uintptr_t)IL2CPP::Globals.m_GameAssembly;

  Camera_get_currentInternal =
      (void *(*)(void))((uintptr_t)(base_addr + 0xC31B50)); // GetMainCamera()

  GameManager_GetVpFPSCamera =
      (void *(*)(void))((uintptr_t)(base_addr + 0xC31D30));

  Camera_WorldToScreenPoint = (Unity::Vector3 (*)(void *, Unity::Vector3, int))(
      (uintptr_t)(base_addr + 0x394B020));
  Component_get_transform =
      (void *(*)(void *))((uintptr_t)(base_addr + 0x39E5780));
  Component_get_gameObject =
      (void *(*)(void *))((uintptr_t)(base_addr + 0x39E5840));
  GameObject_get_transform =
      (void *(*)(void *))((uintptr_t)(base_addr + 0x39EB430));
  GameObject_get_activeInHierarchy =
      (bool (*)(void *))((uintptr_t)(base_addr + 0x39EB8E0));
  Transform_get_position =
      (Unity::Vector3 (*)(void *))((uintptr_t)(base_addr + 0x3A06800));

  GearItem_get_DisplayNameWithCondition =
      (Unity::System_String * (*)(void *))((uintptr_t)(base_addr + 0x8269F0));

  pDummySwapChain->Release();
  pDummyDevice->Release();
  pDummyContext->Release();

  while (g_Running) {
    if (GetAsyncKeyState(VK_END) & 1) {
      g_Running = false;
    }
    Sleep(10);
  }

  // Cleanup
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();

  if (oWndProc) {
    SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)oWndProc);
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  if (mainRenderTargetView)
    mainRenderTargetView->Release();
  if (pContext)
    pContext->Release();
  if (pDevice)
    pDevice->Release();

  FreeLibraryAndExitThread((HMODULE)lpReserved, 0);
  return 0;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved) {
  switch (dwReason) {
  case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(hMod);
    CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
    break;
  case DLL_PROCESS_DETACH:
    break;
  }
  return TRUE;
}
