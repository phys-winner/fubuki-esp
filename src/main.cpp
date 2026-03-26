#include "../deps/IL2CPP_Resolver/IL2CPP_Resolver.hpp"
#include "../deps/imgui/backends/imgui_impl_dx11.h"
#include "../deps/imgui/backends/imgui_impl_win32.h"
#include "../deps/imgui/imgui.h"
#include "../deps/minhook/include/MinHook.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
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
std::atomic<bool> g_Running{true};

// Hack settings
bool g_DrawEsp = false;
bool g_ShowBaseAiESP = false;
bool g_ShowBaseAiHP = true;
bool g_ShowBaseAiName = true;

bool g_ShowGearItemESP = false;
bool g_ShowHarvestableESP = false;
bool g_ShowCarcassESP = false;

float g_EspDistance = 250.0f;

bool g_ShowVisibilityMenu = false;
std::map<std::string, bool> g_ItemHidden;

void SaveConfig() {
    char path[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
    std::string exePath(path);
    std::string iniPath = exePath.substr(0, exePath.find_last_of("\\/")) + "\\fubuki_esp_visibility.ini";
    std::ofstream out(iniPath);
    out << "[HiddenItems]\n";
    for(const auto& kv : g_ItemHidden) {
        if (kv.second) {
            // Basic sanitization: skip if name contains newline or '=' to prevent INI corruption
            if (kv.first.find('\n') != std::string::npos || kv.first.find('\r') != std::string::npos || kv.first.find('=') != std::string::npos)
                continue;
            out << kv.first << "=1\n";
        }
    }
}

void LoadConfig() {
    char path[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return;
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
static Unity::Vector3 (*Transform_get_position)(void *_this);
static Unity::Vector3 (*Transform_get_forward)(void *_this);
static Unity::System_String *(*GearItem_get_DisplayNameWithCondition)(
    void *_this);
static bool (*InterfaceManager_IsOverlayActiveImmediate)();
static bool (*InterfaceManager_IsMainMenuEnabled)();

struct CachedESPItem {
  void* ptr;
  void* transform; // Cached transform pointer
  std::string name;
  bool* pHidden;   // Pointer to visibility state in g_ItemHidden for O(1) lookup
};

std::vector<CachedESPItem> g_BaseAiList;
std::vector<CachedESPItem> g_GearItemList;
std::vector<CachedESPItem> g_HarvestableList;
std::vector<CachedESPItem> g_CarcassList;
std::mutex g_BaseAiMutex;
std::mutex g_GearItemMutex;
std::mutex g_HarvestableMutex;
std::mutex g_CarcassMutex;

uintptr_t off_CurrentHP;
uintptr_t off_MaxHP;
uintptr_t off_DisplayName;
uintptr_t off_MeatAvailableKG;

uintptr_t off_PanelMainMenu_StartFadedOut = 0;
uintptr_t off_PanelMainMenu_InitialScreenFadeInDuration = 0;
uintptr_t off_PanelSandbox_InitialScreenFadeInDuration = 0;
Unity::il2cppClass* g_moviePlayerClass = nullptr;

// Skip Fake Hook Typedefs
typedef void (*tPanelMainMenu_Enable)(void*, bool);
static tPanelMainMenu_Enable oPanelMainMenu_Enable = nullptr;

typedef void (*tPanelMainMenu_UpdateFading)(void*);
static tPanelMainMenu_UpdateFading oPanelMainMenu_UpdateFading = nullptr;

typedef void (*tPanelMainMenu_SetPanelAlpha)(void*, float);
static tPanelMainMenu_SetPanelAlpha oPanelMainMenu_SetPanelAlpha = nullptr;

typedef void (*tPanelSandbox_UpdateFading)(void*);
static tPanelSandbox_UpdateFading oPanelSandbox_UpdateFading = nullptr;

typedef void (*tPanelSandbox_SetPanelAlpha)(void*, float);
static tPanelSandbox_SetPanelAlpha oPanelSandbox_SetPanelAlpha = nullptr;

void hkPanelSandbox_UpdateFading(void* __instance) {
    if (__instance) {
        if (off_PanelSandbox_InitialScreenFadeInDuration != -1)
            *(float*)((uintptr_t)__instance + off_PanelSandbox_InitialScreenFadeInDuration) = 0.0f;
    }
    oPanelSandbox_UpdateFading(__instance);
}

void hkPanelSandbox_SetPanelAlpha(void* __instance, float alpha) {
    oPanelSandbox_SetPanelAlpha(__instance, 1.0f);
}

void hkPanelMainMenu_Enable(void* __instance, bool enable) {
    if (__instance) {
        if (off_PanelMainMenu_StartFadedOut != -1)
            *(bool*)((uintptr_t)__instance + off_PanelMainMenu_StartFadedOut) = true;
        
        if (g_moviePlayerClass) {
            bool val = true;
            IL2CPP::Class::Utils::SetStaticField(g_moviePlayerClass, "m_HasIntroPlayedForMainMenu", &val);
        }
    }
    oPanelMainMenu_Enable(__instance, enable);
}

void hkPanelMainMenu_UpdateFading(void* __instance) {
    if (__instance) {
        if (off_PanelMainMenu_InitialScreenFadeInDuration != -1)
            *(float*)((uintptr_t)__instance + off_PanelMainMenu_InitialScreenFadeInDuration) = 0.0f;
    }
    oPanelMainMenu_UpdateFading(__instance);
}

void hkPanelMainMenu_SetPanelAlpha(void* __instance, float alpha) {
    oPanelMainMenu_SetPanelAlpha(__instance, 1.0f);
}

float Vector3_DistanceSquared(const Unity::Vector3 &v1, const Unity::Vector3 &v2) {
  float deltaX = v1.x - v2.x;
  float deltaY = v1.y - v2.y;
  float deltaZ = v1.z - v2.z;
  return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
}

// Forward declarations
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);
std::string GetDisplayName(void *ai);
std::string GetGearItemDisplayName(void *gearItem);
std::string GetCarcassDisplayName(void *carcass);
void SaveConfig();
void LoadConfig();

LRESULT CALLBACK WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam,
                         LPARAM lParam) {
  if (showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
    return true;

  if (oWndProc)
    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);

  return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

using tBaseAiManagerAdd = void (*)(void *);
tBaseAiManagerAdd oBaseAiManagerAdd;

void hkBaseAiManagerAdd(void *__this) {
  oBaseAiManagerAdd(__this);

  std::string name = GetDisplayName(__this);
  void* transform = Component_get_transform(__this);

  std::lock_guard<std::mutex> lock(g_BaseAiMutex);
  g_BaseAiList.push_back({__this, transform, name, &g_ItemHidden[name]});
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
  g_GearItemList.push_back({__this, transform, name, &g_ItemHidden[name]});
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
  g_HarvestableList.push_back({__this, transform, "", nullptr}); // Name not used for harvestables yet
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

using tBodyHarvestManagerAdd = void (*)(void *);
tBodyHarvestManagerAdd oBodyHarvestManagerAdd;

void hkBodyHarvestManagerAdd(void *__this) {
  oBodyHarvestManagerAdd(__this);

  std::string name = GetCarcassDisplayName(__this);
  void* transform = Component_get_transform(__this);

  std::lock_guard<std::mutex> lock(g_CarcassMutex);
  g_CarcassList.push_back({__this, transform, name, &g_ItemHidden[name]});
}

using tBodyHarvestManagerRemove = void (*)(void *);
tBodyHarvestManagerRemove oBodyHarvestManagerRemove;

void hkBodyHarvestManagerRemove(void *__this) {
  {
    std::lock_guard<std::mutex> lock(g_CarcassMutex);
    auto it = std::find_if(g_CarcassList.begin(), g_CarcassList.end(),
                           [__this](const CachedESPItem& item) { return item.ptr == __this; });

    if (it != g_CarcassList.end()) {
      std::swap(*it,
                g_CarcassList.back()); // Swap the element with the last one
      g_CarcassList.pop_back();        // Remove the last element
    }
  }
  oBodyHarvestManagerRemove(__this);
}

float GetCurrentHP(void *ai) {
  return *(float *)((uintptr_t)ai + off_CurrentHP);
}

float GetMaxHP(void *ai) { return *(float *)((uintptr_t)ai + off_MaxHP); }

std::string GetDisplayName(void *ai) {
  auto ustr = *(Unity::System_String **)((uintptr_t)ai + off_DisplayName);
  return ustr ? ustr->ToString() : "Unknown";
}

std::string GetGearItemDisplayName(void *gearItem) {
  auto ustr = GearItem_get_DisplayNameWithCondition(gearItem);
  return ustr ? ustr->ToString() : "Unknown";
}

std::string GetCarcassDisplayName(void *carcass) {
  typedef Unity::System_String *(*tGetDisplayName)(void *);
  static tGetDisplayName GetDisplayNameFunc = nullptr;
  if (!GetDisplayNameFunc) {
      GetDisplayNameFunc = reinterpret_cast<tGetDisplayName>(
          IL2CPP::Class::Utils::GetMethodPointer("BodyHarvest", "GetDisplayName", 0));
  }
  if (!GetDisplayNameFunc) return "Carcass";
  auto ustr = GetDisplayNameFunc(carcass);
  return ustr ? ustr->ToString() : "Carcass";
}

void DrawESP() {
  if (!g_DrawEsp)
    return;

  if (InterfaceManager_IsOverlayActiveImmediate && InterfaceManager_IsOverlayActiveImmediate())
    return;

  if (InterfaceManager_IsMainMenuEnabled && InterfaceManager_IsMainMenuEnabled())
    return;

  auto camera = GameManager_GetMainCamera();
  if (camera == nullptr)
      return;

  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  ImGuiIO &io = ImGui::GetIO();

  void* camera_transform = Component_get_transform(camera);
  if (!camera_transform) return;

  Unity::Vector3 camera_position = Transform_get_position(camera_transform);
  Unity::Vector3 camera_forward = Transform_get_forward(camera_transform);
  float maxDistSq = g_EspDistance * g_EspDistance;
  char textBuf[256];

  if (g_ShowBaseAiESP) {
    std::lock_guard<std::mutex> lock(g_BaseAiMutex);
    for (auto &ai : g_BaseAiList) {
      if (ai.pHidden && *ai.pHidden) continue;
      if (!ai.transform) continue;

      // Update position every frame for AI
      Unity::Vector3 worldPos = Transform_get_position(ai.transform);

      // Fast plane culling
      Unity::Vector3 dir = { worldPos.x - camera_position.x, worldPos.y - camera_position.y, worldPos.z - camera_position.z };
      if ((dir.x * camera_forward.x + dir.y * camera_forward.y + dir.z * camera_forward.z) <= 0.0f)
          continue;

      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);

      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      int offset = sprintf_s(textBuf, "* ");

      if (g_ShowBaseAiHP) {
        int hp = (int)GetCurrentHP(ai.ptr);
        int maxHp = (int)GetMaxHP(ai.ptr);
        offset += sprintf_s(textBuf + offset, sizeof(textBuf) - offset, "[%d/%d] ", hp, maxHp);
      }

      offset += sprintf_s(textBuf + offset, sizeof(textBuf) - offset, "[%.1fm.] ", dist);

      if (g_ShowBaseAiName)
        sprintf_s(textBuf + offset, sizeof(textBuf) - offset, "%s", ai.name.c_str());

      float y = io.DisplaySize.y - screenPos.y;
      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 100, 100, 255), textBuf);
    }
  }

  if (g_ShowGearItemESP) {
    std::lock_guard<std::mutex> lock(g_GearItemMutex);
    for (auto &item : g_GearItemList) {
      if (item.pHidden && *item.pHidden) continue;
      if (!item.transform) continue;

      // Update position every frame
      Unity::Vector3 worldPos = Transform_get_position(item.transform);

      // Fast plane culling
      Unity::Vector3 dir = { worldPos.x - camera_position.x, worldPos.y - camera_position.y, worldPos.z - camera_position.z };
      if ((dir.x * camera_forward.x + dir.y * camera_forward.y + dir.z * camera_forward.z) <= 0.0f)
          continue;

      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      sprintf_s(textBuf, "* [%.1fm.] %s", dist, item.name.c_str());

      float y = io.DisplaySize.y - screenPos.y;
      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 255, 100, 255), textBuf);
    }
  }

  if (g_ShowHarvestableESP) {
    std::lock_guard<std::mutex> lock(g_HarvestableMutex);
    for (auto &harvestable : g_HarvestableList) {
      if (!harvestable.transform) continue;

      // Update position every frame
      Unity::Vector3 worldPos = Transform_get_position(harvestable.transform);

      // Fast plane culling
      Unity::Vector3 dir = { worldPos.x - camera_position.x, worldPos.y - camera_position.y, worldPos.z - camera_position.z };
      if ((dir.x * camera_forward.x + dir.y * camera_forward.y + dir.z * camera_forward.z) <= 0.0f)
          continue;

      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      sprintf_s(textBuf, "* [%.1fm.]", dist);

      float y = io.DisplaySize.y - screenPos.y;
      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 255, 100, 255), textBuf);
    }
  }

  if (g_ShowCarcassESP) {
    std::lock_guard<std::mutex> lock(g_CarcassMutex);
    for (auto &carcass : g_CarcassList) {
      if (carcass.pHidden && *carcass.pHidden) continue;
      if (!carcass.transform) continue;

      Unity::Vector3 worldPos = Transform_get_position(carcass.transform);

      Unity::Vector3 dir = { worldPos.x - camera_position.x, worldPos.y - camera_position.y, worldPos.z - camera_position.z };
      if ((dir.x * camera_forward.x + dir.y * camera_forward.y + dir.z * camera_forward.z) <= 0.0f)
          continue;

      float distSq = Vector3_DistanceSquared(camera_position, worldPos);
      if (distSq > maxDistSq)
          continue;

      Unity::Vector3 screenPos = Camera_WorldToScreenPoint(
          camera, worldPos, Unity::m_eCameraEye::m_eCameraEye_Center);
      if (screenPos.z < 0.1f)
        continue;

      float dist = std::sqrtf(distSq);
      float meatKg = *(float*)((uintptr_t)carcass.ptr + off_MeatAvailableKG);
      sprintf_s(textBuf, "* [%.1fm.] %s (%.1fkg)", dist, carcass.name.c_str(), meatKg);

      float y = io.DisplaySize.y - screenPos.y;
      draw->AddText(ImVec2(screenPos.x, y), IM_COL32(255, 165, 0, 255), textBuf); // Orange for carcasses
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
  g_DrawEsp = g_ShowBaseAiESP || g_ShowGearItemESP || g_ShowHarvestableESP || g_ShowCarcassESP;

  if (g_DrawEsp || showMenu) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    DrawESP();
    if (showMenu) {
      ImGui::Begin("Fubuki-ESP | The Long Dark", &showMenu, ImGuiWindowFlags_AlwaysAutoResize);
      ImGui::TextDisabled("Controls: [INS] Menu | [END] Unload");
      ImGui::Separator();

      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
      ImGui::Checkbox("BaseAi ESP", &g_ShowBaseAiESP);
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Show locations of animals and other AI entities.");
      if (g_ShowBaseAiESP) {
        ImGui::Indent();
        ImGui::Checkbox("Show HP", &g_ShowBaseAiHP);
        ImGui::Checkbox("Show Name", &g_ShowBaseAiName);
        ImGui::Unindent();
      }
      {
        std::lock_guard<std::mutex> lock(g_BaseAiMutex);
        ImGui::Text("Found %zu BaseAi", g_BaseAiList.size());
      }

      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 100, 255));
      ImGui::Checkbox("GearItem ESP", &g_ShowGearItemESP);
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Show locations of tools, food, and other resources.");
      {
        std::lock_guard<std::mutex> lock(g_GearItemMutex);
        ImGui::Text("Found %zu GearItem", g_GearItemList.size());
      }

      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 100, 255));
      ImGui::Checkbox("Harvestable ESP", &g_ShowHarvestableESP);
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Highlight plants and harvestable objects in the environment.");
      {
        std::lock_guard<std::mutex> lock(g_HarvestableMutex);
        ImGui::Text("Found %zu Harvestable", g_HarvestableList.size());
      }

      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 165, 0, 255));
      ImGui::Checkbox("Carcass ESP", &g_ShowCarcassESP);
      ImGui::PopStyleColor();
      ImGui::SetItemTooltip("Show locations of animal carcasses (harvestable bodies).");
      {
        std::lock_guard<std::mutex> lock(g_CarcassMutex);
        ImGui::Text("Found %zu Carcass", g_CarcassList.size());
      }

      ImGui::SliderFloat("ESP Distance", &g_EspDistance, 10.0f, 1000.0f);

      ImGui::Separator();
      if (ImGui::Button("Visibility Menu", ImVec2(120, 0))) {
        g_ShowVisibilityMenu = !g_ShowVisibilityMenu;
      }
      ImGui::SetItemTooltip("Open a detailed menu to toggle visibility for individual item types.");
      ImGui::SameLine();
      if (ImGui::Button("Unload DLL", ImVec2(120, 0))) {
        ImGui::OpenPopup("Confirm Unload");
      }
      ImGui::SetItemTooltip("Safely detach the cheat from the game.");

      if (ImGui::BeginPopupModal("Confirm Unload", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure you want to unload the DLL?\nThis will safely detach the cheat from the game.");
        ImGui::Separator();

        if (ImGui::Button("Yes, Unload", ImVec2(120, 0))) {
          g_Running = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
      }
      ImGui::End();
    }

    if (g_ShowVisibilityMenu) {
      ImGui::Begin("ESP Item Visibility", &g_ShowVisibilityMenu);
      ImGui::SetWindowSize(ImVec2(300, 600), ImGuiCond_FirstUseEver);

      static char search[64] = "";
      ImGui::Text("Filter:"); ImGui::SameLine();
      ImGui::InputTextWithHint("##filter", "Search items...", search, IM_ARRAYSIZE(search));
      if (search[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::Button("X")) {
          search[0] = '\0';
        }
        ImGui::SetItemTooltip("Clear search filter");
      }

      static int sortType = 0; static bool sortAsc = true;
      ImGui::RadioButton("Name", &sortType, 0); ImGui::SameLine();
      ImGui::RadioButton("Qty", &sortType, 1); ImGui::SameLine();
      ImGui::Checkbox("Asc", &sortAsc);

      struct ItemCount { std::string name; int count; bool visible; };
      std::map<std::string, int> counts;
      { std::lock_guard<std::mutex> l1(g_GearItemMutex); for(auto& i : g_GearItemList) counts[i.name]++; }
      { std::lock_guard<std::mutex> l2(g_BaseAiMutex); for(auto& a : g_BaseAiList) counts[a.name]++; }
      { std::lock_guard<std::mutex> l3(g_CarcassMutex); for(auto& c : g_CarcassList) counts[c.name]++; }

      std::vector<ItemCount> items;
      std::string filter(search); std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
      for(auto& kv : counts) {
          std::string nameLower = kv.first; std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
          if (filter.empty() || nameLower.find(filter) != std::string::npos)
              items.push_back({kv.first, kv.second, !g_ItemHidden[kv.first]});
      }

      std::sort(items.begin(), items.end(), [](const ItemCount& a, const ItemCount& b) {
          if (sortType == 0) return sortAsc ? a.name < b.name : a.name > b.name;
          return (a.count == b.count) ? (sortAsc ? a.name < b.name : a.name > b.name) : (sortAsc ? a.count < b.count : a.count > b.count);
      });

      if (ImGui::Button("Show All")) {
          for (const auto& item : items) g_ItemHidden[item.name] = false;
          SaveConfig();
      }
      ImGui::SameLine();
      if (ImGui::Button("Hide All")) {
          for (const auto& item : items) g_ItemHidden[item.name] = true;
          SaveConfig();
      }

      if (ImGui::BeginChild("##scrolly", ImVec2(0, 0), true)) {
          if (items.empty()) {
              ImGui::TextDisabled("No items matching filter.");
          }
          for(auto& item : items) {
              bool checked = item.visible;
              if (ImGui::Checkbox(("[ " + std::to_string(item.count) + " ] " + item.name + "##" + item.name).c_str(), &checked)) {
                  g_ItemHidden[item.name] = !checked; SaveConfig();
              }
          }
      }
      ImGui::EndChild();
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

bool InitOffsets() {
  off_CurrentHP = IL2CPP::Class::Utils::GetFieldOffset("BaseAi", "m_CurrentHP");
  off_MaxHP = IL2CPP::Class::Utils::GetFieldOffset("BaseAi", "m_MaxHP");
  off_DisplayName =
      IL2CPP::Class::Utils::GetFieldOffset("BaseAi", "m_DisplayName");
  off_MeatAvailableKG = IL2CPP::Class::Utils::GetFieldOffset("BodyHarvest", "m_MeatAvailableKG");

  // Skip Fade Offsets
  off_PanelMainMenu_StartFadedOut = IL2CPP::Class::Utils::GetFieldOffset("Panel_MainMenu", "m_StartFadedOut");
  off_PanelMainMenu_InitialScreenFadeInDuration = IL2CPP::Class::Utils::GetFieldOffset("Panel_MainMenu", "m_InitialScreenFadeInDuration");
  off_PanelSandbox_InitialScreenFadeInDuration = IL2CPP::Class::Utils::GetFieldOffset("Panel_Sandbox", "m_InitialScreenFadeInDuration");

  auto moviePlayerClass = IL2CPP::Class::Find("MoviePlayer");
  if (moviePlayerClass) {
      g_moviePlayerClass = moviePlayerClass;
  }

  return off_CurrentHP != -1 && off_MaxHP != -1 && off_DisplayName != -1 &&
         off_MeatAvailableKG != -1;
}

DWORD WINAPI MainThread(LPVOID lpReserved) {
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

  if (!InitOffsets())
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

  auto bodyHarvestManagerAddPtr =
      IL2CPP::Class::Utils::GetMethodPointer("BodyHarvestManager", "AddBodyHarvest", 1);
  if (MH_OK != MH_CreateHook(bodyHarvestManagerAddPtr, &hkBodyHarvestManagerAdd,
                             reinterpret_cast<void **>(&oBodyHarvestManagerAdd)))
    return 1;

  auto bodyHarvestManagerRemovePtr =
      IL2CPP::Class::Utils::GetMethodPointer("BodyHarvestManager", "Destroy", 1);
  if (MH_OK != MH_CreateHook(bodyHarvestManagerRemovePtr, &hkBodyHarvestManagerRemove,
                             reinterpret_cast<void **>(&oBodyHarvestManagerRemove)))
    return 1;

  // Skip Fade Hooks
  auto panelMainMenuEnablePtr = IL2CPP::Class::Utils::GetMethodPointer("Panel_MainMenu", "Enable", 1);
  if (panelMainMenuEnablePtr) MH_CreateHook(panelMainMenuEnablePtr, &hkPanelMainMenu_Enable, reinterpret_cast<void**>(&oPanelMainMenu_Enable));

  auto panelMainMenuUpdateFadingPtr = IL2CPP::Class::Utils::GetMethodPointer("Panel_MainMenu", "UpdateFading", 0);
  if (panelMainMenuUpdateFadingPtr) MH_CreateHook(panelMainMenuUpdateFadingPtr, &hkPanelMainMenu_UpdateFading, reinterpret_cast<void**>(&oPanelMainMenu_UpdateFading));

  auto panelMainMenuSetPanelAlphaPtr = IL2CPP::Class::Utils::GetMethodPointer("Panel_MainMenu", "SetPanelAlpha", 1);
  if (panelMainMenuSetPanelAlphaPtr) MH_CreateHook(panelMainMenuSetPanelAlphaPtr, &hkPanelMainMenu_SetPanelAlpha, reinterpret_cast<void**>(&oPanelMainMenu_SetPanelAlpha));

  auto panelSandboxUpdateFadingPtr = IL2CPP::Class::Utils::GetMethodPointer("Panel_Sandbox", "UpdateFading", 0);
  if (panelSandboxUpdateFadingPtr) MH_CreateHook(panelSandboxUpdateFadingPtr, &hkPanelSandbox_UpdateFading, reinterpret_cast<void**>(&oPanelSandbox_UpdateFading));

  auto panelSandboxSetPanelAlphaPtr = IL2CPP::Class::Utils::GetMethodPointer("Panel_Sandbox", "SetPanelAlpha", 1);
  if (panelSandboxSetPanelAlphaPtr) MH_CreateHook(panelSandboxSetPanelAlphaPtr, &hkPanelSandbox_SetPanelAlpha, reinterpret_cast<void**>(&oPanelSandbox_SetPanelAlpha));

  GameManager_GetMainCamera = reinterpret_cast<void *(*)()>(
      IL2CPP::Class::Utils::GetMethodPointer("GameManager", "GetMainCamera", 0));

  Camera_WorldToScreenPoint = reinterpret_cast<Unity::Vector3 (*)(void *, Unity::Vector3, int)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Camera", "WorldToScreenPoint", 2));

  Component_get_transform = reinterpret_cast<void *(*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Component", "get_transform", 0));

  Transform_get_position = reinterpret_cast<Unity::Vector3 (*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Transform", "get_position", 0));

  Transform_get_forward = reinterpret_cast<Unity::Vector3 (*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("UnityEngine.Transform", "get_forward", 0));

  GearItem_get_DisplayNameWithCondition = reinterpret_cast<Unity::System_String * (*)(void *)>(
      IL2CPP::Class::Utils::GetMethodPointer("GearItem", "get_DisplayNameWithCondition", 0));

  InterfaceManager_IsOverlayActiveImmediate = reinterpret_cast<bool (*)()>(
      IL2CPP::Class::Utils::GetMethodPointer("InterfaceManager", "IsOverlayActiveImmediate", 0));

  InterfaceManager_IsMainMenuEnabled = reinterpret_cast<bool (*)()>(
      IL2CPP::Class::Utils::GetMethodPointer("InterfaceManager", "IsMainMenuEnabled", 0));

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
