# Sentinel's Journal - Critical Security Learnings

This journal tracks critical security learnings, vulnerability patterns, and reusable security patterns discovered in this codebase.

## 2025-05-14 - Initial Scan
- Started security audit of Fubuki-ESP.
- Focused on: Input validation (ImGui), Memory safety, DLL injection security, and potential information leaks.

## 2025-05-15 - [Hardening Hook Stability and Pointer Safety]
**Vulnerability:** Null pointer dereferences in entity property accessors and D3D11 resize hooks.
**Learning:** In hooked applications, the host process (game) may trigger callbacks like `ResizeBuffers` before the DLL's initialization logic (in `hkPresent`) has completed, or pass pointers to entities that have been invalidated.
**Prevention:** Implement global initialization flags (`init`) to guard hook execution and apply defensive null checks to all base entity pointers before performing pointer arithmetic or dereferencing.
