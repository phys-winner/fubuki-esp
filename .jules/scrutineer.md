# Scrutineer Journal 🔍

## Architectural Traps

- **Redundant Helper Functions**: The project contains several helper functions (`GetGearItemWorldPosition`, `GetWorldPosition`) that added layers of indirection (Transform -> GameObject -> Component) instead of directly accessing the required component properties. These have been removed.
- **Dead IL2CPP Pointers**: Unused method pointers (e.g., `GameObject_get_activeInHierarchy`) were being resolved and cached at startup, increasing initialization time and maintenance surface area. Always verify if a cached pointer is actually invoked in the rendering or logic loops.
- **Unused Mathematical Wrappers**: `Vector3_Distance` was defined but unused in favor of `Vector3_DistanceSquared` (which is standard for performance). Avoid adding boilerplate math functions until they are explicitly needed.
- **Problematic Static Caching in Helpers**: Redundant camera position/forward helper functions used `static` caching for transform pointers. This is "unsafe" and prone to Use-After-Free during scene transitions or camera re-allocation. These helpers have been removed in favor of per-frame direct retrieval in the rendering loop.
- **Cargo-Culted D3D11 Boilerplate**: Unused typedefs (`DrawIndexed`) and local vtable pointers (`pContextVTable`) were being initialized without any intent to use them. These have been removed to reduce cognitive load and initialization surface area.
