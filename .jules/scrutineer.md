# Scrutineer Journal 🔍

## Architectural Traps

- **Redundant Helper Functions**: The project contains several helper functions (`GetGearItemWorldPosition`, `GetWorldPosition`) that added layers of indirection (Transform -> GameObject -> Component) instead of directly accessing the required component properties. These have been removed.
- **Dead IL2CPP Pointers**: Unused method pointers (e.g., `GameObject_get_activeInHierarchy`) were being resolved and cached at startup, increasing initialization time and maintenance surface area. Always verify if a cached pointer is actually invoked in the rendering or logic loops.
- **Unused Mathematical Wrappers**: `Vector3_Distance` was defined but unused in favor of `Vector3_DistanceSquared` (which is standard for performance). Avoid adding boilerplate math functions until they are explicitly needed.
- **Out-of-Scope Hook Logic**: The project previously included hooks for skipping game fades and intro movies (`Panel_MainMenu`, `Panel_Sandbox`, `MoviePlayer`). These features were unrelated to the core ESP functionality and added unnecessary maintenance overhead (typedefs, offsets, and hook registrations). Always keep features focused and avoid "feature creep" that doesn't serve the primary purpose of the tool.
