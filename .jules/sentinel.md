# Sentinel's Journal - Critical Security Learnings

This journal tracks critical security learnings, vulnerability patterns, and reusable security patterns discovered in this codebase.

## 2025-05-14 - Initial Scan
- Started security audit of Fubuki-ESP.
- Focused on: Input validation (ImGui), Memory safety, DLL injection security, and potential information leaks.

## 2025-05-15 - Use-After-Free during scene transitions
**Vulnerability:** Use-After-Free (UAF) leading to process crash.
**Learning:** Unity's internal scene management can destroy objects without triggering manager-specific `Remove` methods, leaving stale pointers in ESP caches. Returning to the main menu resets the managers but may skip individual `Remove` calls.
**Prevention:** Hook `GearManager.Reset`, `BaseAiManager.Reset`, and `HarvestableManager.Reset` to clear respective entity caches before the game resets the managers, and implement defensive null checks in the rendering loop.
