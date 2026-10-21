#pragma once
#include <cstddef>
#include <cstdint>

// Names of ECS component types in the running game (found once, from the registry).
namespace ecs {
// The component type's short name ("HighlightColor"). False if unknown.
bool Name(void* registry, uint16_t type, char* out, size_t size);
// The type index of a component by name, or -1.
int Find(void* registry, const char* name);
}
