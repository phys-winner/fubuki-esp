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
#include <fstream>
#include <map>
#include <mutex>
#include <string>
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

bool g_ShowVisibilityMenu = false;
std::map<std::string, bool> g_ItemHidden;

void SaveConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string exePath(path);
    std::string iniPath = exePath.substr(0, exePath.find_last_of("\\/")) + "\\fubuki_esp_visibility.ini";
    std::ofstream out(iniPath);
    out << "[HiddenItems]\n";
    for(const auto& kv : g_ItemHidden) {
        if (kv.second) {
            out << kv.first << "=1\n";
        }
    }
}

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string exePath(path);
    std::string iniPath = exePath.substr(0, exePath.find_last_of("\\/")) + "\\fubuki_esp_visibility.ini";
    std::ifstream in(iniPath);
    std::string line;
    bool inSection = false;
    while(std::getline(in, line)) {
        if (line == "[HiddenItems]") { inSection = true; continue; }
        if (line.empty() || line[0] == '[') { if(inSection && line[0] == '[') inSection = false; continue; }
        if (inSection) {
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string k = line.substr(0, pos);
                std::string v = line.substr(pos + 1);
                if (v == "1") g_ItemHidden[k] = true;
            }
        }
    }
}

static void *(*GameManager_GetMainCamera)();
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

struct CachedESPItem {
  void* ptr;
  void* transform; // Cached transform pointer
  std::string name;
};

std::vector<CachedESPItem> g_BaseAiList;
std::vector<CachedESPItem> g_GearItemList;
std::vector<CachedESPItem> g_HarvestableList;
std::mutex g_BaseAiMutex;
std::mutex g_GearItemMutex;
std::mutex g_HarvestableMutex;

uintptr_t off_CurrentHP;
uintptr_t off_MaxHP;
uintptr_t off_DisplayName;
uintptr_t off_Camera;

float Vector3_DistanceSquared(const Unity::Vector3 &v1, const Unity::Vector3 &v2) {
  float deltaX = v1.x - v2.x;
  float deltaY = v1.y - v2.y;
  float deltaZ = v1.z - v2.z;
  return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
}

float Vector3_Distance(const Unity::Vector3 &v1, const Unity::Vector3 &v2) {
  return std::sqrtf(Vector3_DistanceSquared(v1, v2));
}

// Forward declarations
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);
std::string GetDisplayName(void *ai);
std::string GetGearItemDisplayName(void *gearItem);
void SaveConfig();
void LoadConfig();

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

  std::string name = GetDisplayName(__this);
  void* transform = Component_get_transform(__this);

  std::lock_guard<std::mutex> lock(g_BaseAiMutex);
  g_BaseAiList.push_back({__this, transform, name});
}

using tBaseAiManagerRemove = void (*)(void *);
tBaseAiManagerRemove oBaseAiManagerRemove;

void hkBaseAiManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_BaseAiMutex);
    auto it = std::find_if(g_BaseAiList.begin(), g_BaseAiList.end(),
                           [__this](const CachedESPItem& item) { return item.ptr == __this; });

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

  std::string name = GetGearItemDisplayName(__this);
  void* transform = Component_get_transform(__this);

  std::lock_guard<std::mutex> lock(g_GearItemMutex);
  g_GearItemList.push_back({__this, transform, name});
}

using tGearManagerRemove = void (*)(void *);
tGearManagerRemove oGearManagerRemove;

void hkGearManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_GearItemMutex);
    auto it = std::find_if(g_GearItemList.begin(), g_GearItemList.end(),
                           [__this](const CachedESPItem& item) { return item.ptr == __this; });

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

  void* transform = Component_get_transform(__this);

  std::lock_guard<std::mutex> lock(g_HarvestableMutex);
  g_HarvestableList.push_back({__this, transform, ""}); // Name not used for harvestables yet
}

using tHarvestableManagerRemove = void (*)(void *);
tHarvestableManagerRemove oHarvestableManagerRemove;

void hkHarvestableManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_HarvestableMutex);
    auto it = std::find_if(g_HarvestableList.begin(), g_HarvestableList.end(),
                           [__this](const CachedESPItem& item) { return item.ptr == __this; });

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

  auto camera = GameManager_GetMainCamera();
  if (camera == nullptr)
      return;

  ImDrawList *draw = ImGui::GetForegroundDrawList();
  Unity::Vector3 camera_position = GetCameraPosition(camera);
  float maxDistSq = g_EspDistance * g_EspDistance;

  if (g_ShowBaseAiESP) {
    std::lock_guard<std::mutex> lock(g_BaseAiMutex);
    for (auto &ai : g_BaseAiList) {
      if (g_ItemHidden[ai.name]) continue;

      // Use cached transform to get position directly
      Unity::Vector3 worldPos = Transform_get_position(ai.transform);
      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);

      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      std::string text = "* ";
      if (g_ShowBaseAiHP) {
        int hp = (int)GetCurrentHP(ai.ptr);
        int maxHp = (int)GetMaxHP(ai.ptr);

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
        text += ai.name;

      ImGuiIO &io = ImGui::GetIO();
      float y = io.DisplaySize.y - screenPos.y;

      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 100, 100, 255),
                    text.c_str());
    }
  }

  if (g_ShowGearItemESP) {
    std::lock_guard<std::mutex> lock(g_GearItemMutex);
    for (auto &item : g_GearItemList) {
      if (g_ItemHidden[item.name]) continue;

      // Use cached transform to get position directly
      Unity::Vector3 worldPos = Transform_get_position(item.transform);
      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      std::string text = "* ";

      char distBuf[32];
      sprintf_s(distBuf, "%.1f", dist);
      text += " [";
      text += distBuf;
      text += "m.] ";
      text += item.name;

      ImGuiIO &io = ImGui::GetIO();
      float y = io.DisplaySize.y - screenPos.y;

      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 255, 100, 255),
                    text.c_str());
    }
  }

  if (g_ShowHarvestableESP) {
    std::lock_guard<std::mutex> lock(g_HarvestableMutex);
    for (auto &harvestable : g_HarvestableList) {
      // Use cached transform to get position directly
      Unity::Vector3 worldPos = Transform_get_position(harvestable.transform);
      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      std::string text = "* ";

      char distBuf[32];
      sprintf_s(distBuf, "%.1f", dist);
      text += "[";
      text += distBuf;
      text += "m.] ";

      ImGuiIO &io = ImGui::GetIO();
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
      if (ImGui::Button("Visibility Menu", ImVec2(120, 0))) {
        g_ShowVisibilityMenu = !g_ShowVisibilityMenu;
      }
      ImGui::SameLine();
      if (ImGui::Button("Unload DLL", ImVec2(120, 0))) {
        // Signal unload
      }
      ImGui::End();
    }

    if (g_ShowVisibilityMenu) {
      ImGui::Begin("ESP Item Visibility", &g_ShowVisibilityMenu, ImGuiWindowFlags_AlwaysAutoResize);
      
      static int sortType = 0; // 0 = name, 1 = count
      static bool sortAsc = true;
      
      ImGui::RadioButton("Sort by Name", &sortType, 0); ImGui::SameLine();
      ImGui::RadioButton("Sort by Quantity", &sortType, 1);
      ImGui::Checkbox("Ascending", &sortAsc);
      ImGui::Separator();
      
      struct ItemCount { std::string name; int count; bool visible; };
      std::map<std::string, int> counts;
      {
          std::lock_guard<std::mutex> lock1(g_GearItemMutex);
          for(auto& item : g_GearItemList) counts[item.name]++;
      }
      {
          std::lock_guard<std::mutex> lock2(g_BaseAiMutex);
          for(auto& ai : g_BaseAiList) counts[ai.name]++;
      }
      
      std::vector<ItemCount> items;
      for(auto& kv : counts) {
          items.push_back({kv.first, kv.second, !g_ItemHidden[kv.first]});
      }
      
      std::sort(items.begin(), items.end(), [](const ItemCount& a, const ItemCount& b) {
          if (sortType == 0) {
              return sortAsc ? a.name < b.name : a.name > b.name;
          } else {
              if (a.count == b.count) return sortAsc ? a.name < b.name : a.name > b.name;
              return sortAsc ? a.count < b.count : a.count > b.count;
          }
      });
      
      int totalItems = (int)items.size();
      int totalPages = (totalItems + 14) / 15;
      if (totalPages == 0) totalPages = 1;

      static int currentPage = 0;
      if (currentPage >= totalPages) currentPage = totalPages - 1;
      if (currentPage < 0) currentPage = 0;

      int startIdx = currentPage * 15;
      int endIdx = startIdx + 15 < totalItems ? startIdx + 15 : totalItems;

      for(int i = startIdx; i < endIdx; ++i) {
          auto& item = items[i];
          bool checked = item.visible;
          std::string label = "[" + std::to_string(item.count) + "] " + item.name + "##" + item.name;
          if (ImGui::Checkbox(label.c_str(), &checked)) {
              g_ItemHidden[item.name] = !checked;
              SaveConfig();
          }
      }

      ImGui::Separator();
      if (ImGui::Button("< Prev") && currentPage > 0) {
          currentPage--;
      }
      ImGui::SameLine();
      ImGui::Text("Page %d of %d", currentPage + 1, totalPages);
      ImGui::SameLine();
      if (ImGui::Button("Next >") && currentPage < totalPages - 1) {
          currentPage++;
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

  LoadConfig();

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

  GameManager_GetMainCamera = reinterpret_cast<void *(*)()>(
      IL2CPP::Class::Utils::GetMethodPointer("GameManager", "GetMainCamera", 0));

  Camera_WorldToScreenPoint = reinterpret_cast<Unity::Vector3 (*)(void *, Unity::Vector3, int)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Camera", "WorldToScreenPoint", 2));

  Component_get_transform = reinterpret_cast<void *(*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Component", "get_transform", 0));

  Component_get_gameObject = reinterpret_cast<void *(*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Component", "get_gameObject", 0));

  GameObject_get_transform = reinterpret_cast<void *(*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.GameObject", "get_transform", 0));

  GameObject_get_activeInHierarchy = reinterpret_cast<bool (*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.GameObject", "get_activeInHierarchy", 0));

  Transform_get_position = reinterpret_cast<Unity::Vector3 (*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Transform", "get_position", 0));

  GearItem_get_DisplayNameWithCondition = reinterpret_cast<Unity::System_String * (*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("GearItem", "get_DisplayNameWithCondition", 0));

  if (MH_OK != MH_EnableHook(MH_ALL_HOOKS))
    return 1;

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
