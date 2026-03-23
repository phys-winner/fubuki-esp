# Sentinel's Journal - Critical Security Learnings

This journal tracks critical security learnings, vulnerability patterns, and reusable security patterns discovered in this codebase.

## 2025-05-14 - Initial Scan
- Started security audit of Fubuki-ESP.
- Focused on: Input validation (ImGui), Memory safety, DLL injection security, and potential information leaks.

## 2025-05-15 - Use-After-Free during scene transitions
**Vulnerability:** Use-After-Free (UAF) leading to process crash.
**Learning:** Unity's internal scene management can destroy objects without triggering manager-specific `Remove` methods, leaving stale pointers in ESP caches.
**Prevention:** Hook `SceneManager.UnloadSceneNameIndexInternal` to clear all entity caches before unloads begin and implement defensive null checks in the rendering loop.
