## 2025-05-15 - [Initial Profiling & Planning]
**Learning:** In internal game cheats, making repeated calls to IL2CPP resolver methods like `Component_get_transform` and `GameObject_get_transform` inside the rendering loop (usually `hkPresent`) is a significant performance bottleneck. These calls involve cross-domain overhead and can be executed hundreds of times per frame depending on the number of items.
**Action:** Always cache transform pointers and other static component references during item registration (Add/Remove hooks) to minimize per-frame IL2CPP overhead. Use squared distance comparisons to early-exit from the rendering loop before performing expensive `WorldToScreenPoint` calculations.

## 2025-05-16 - [ESP Rendering Loop Bottlenecks]
**Learning:** In the hot-path `DrawESP` loop, `std::string` concatenation and `std::map<std::string, bool>` lookups for visibility state were significant overhead sources. Even O(log N) lookups add up when processing many entities every frame.
**Action:** Use stack-allocated `char` buffers with `sprintf_s` for text formatting. Cache a pointer (e.g., `bool* pHidden`) directly to the value in the configuration map within the entity registration struct to achieve O(1) property access without string hashing or tree traversal.

## 2025-05-17 - [Advanced ESP Optimization Patterns]
**Learning:** Beyond caching static pointers, per-frame IL2CPP method calls for on-screen projections (`WorldToScreenPoint`) are major bottlenecks when processing 100+ entities. While staggered position updates are a common performance win, they can reduce visual accuracy. Plane-culling remains a zero-compromise optimization for maintaining high FPS.
**Action:** Prioritize per-frame position accuracy when requested by updating entity positions every frame (`Transform_get_position`). To offset the IL2CPP overhead, always implement a fast C++ dot-product plane check to cull off-screen entities before calling expensive projection methods. Avoid using member functions on POD structs like `Unity::Vector3` unless explicitly defined; use manual component math instead.
