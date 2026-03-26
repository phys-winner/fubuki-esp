## 2025-05-22 - [ImGui Child Region Stack Management]
**Learning:** In Dear ImGui, `ImGui::EndChild()` must be called whenever `ImGui::BeginChild()` is called, even if the return value of `BeginChild()` is false (indicating it's clipped or collapsed). Failing to do so causes a stack imbalance.
**Action:** Always place `EndChild()` outside the `if (BeginChild(...))` block.

## 2025-05-22 - [Rendering Z-Order in ImGui Hooks]
**Learning:** Using `ImGui::GetForegroundDrawList()` for ESP labels will cause them to render on top of the ImGui menu windows, creating visual clutter and making the menu hard to read.
**Action:** Use `ImGui::GetBackgroundDrawList()` for ESP labels to ensure the menu (which uses the regular draw list) always stays on top.

## 2025-05-23 - [Accidental Action Prevention & Discoverability]
**Learning:** Destructive or irreversible actions (like unloading a DLL) require a confirmation barrier to prevent accidental clicks. Additionally, domain-specific terminology (e.g., "BaseAi") benefits significantly from contextual tooltips to improve accessibility for new users.
**Action:** Implement modal popups for "Unload/Exit" actions and use `ImGui::SetItemTooltip` for technical feature toggles.

## 2025-05-24 - [Filtering & Safety Design Patterns]
**Learning:** For destructive modals, setting default focus to the "Cancel" button using `ImGui::SetItemDefaultFocus()` prevents accidental confirmation. In search filters, `ImGui::InputTextWithHint` improves clarity, while bulk actions (Show/Hide All) that target only the filtered subset enhance user efficiency. Providing explicit empty state messages (e.g., "No items matching filter") reduces user confusion.
**Action:** Prioritize safety-first button focus, use hints in text inputs, and implement bulk actions on filtered subsets to improve UX flow.

## 2025-05-25 - [Unit Clarity & Quick-Reset Patterns]
**Learning:** Numerical sliders for distance or magnitude are more intuitive when paired with unit suffixes (e.g., "m" for meters). Furthermore, implementing a "right-click to reset" shortcut via `ImGui::IsItemClicked(1)` provides a powerful efficiency boost for returning to safe defaults without tedious manual adjustment.
**Action:** Use unit-aware format strings in sliders and always provide a right-click reset to the default value, documented via tooltip.
