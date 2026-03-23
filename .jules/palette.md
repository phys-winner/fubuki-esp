## 2025-05-22 - [ImGui Child Region Stack Management]
**Learning:** In Dear ImGui, `ImGui::EndChild()` must be called whenever `ImGui::BeginChild()` is called, even if the return value of `BeginChild()` is false (indicating it's clipped or collapsed). Failing to do so causes a stack imbalance.
**Action:** Always place `EndChild()` outside the `if (BeginChild(...))` block.
