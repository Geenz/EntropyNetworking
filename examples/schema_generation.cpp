/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2025 Jonathan "Geenz" Goodman
 * This file is part of the Entropy Networking project.
 */

/**
 * @file schema_generation.cpp
 * @brief Example demonstrating automatic ComponentSchema generation
 *
 * This example shows how to use EntropyNetworking's schema generation utilities
 * to automatically create ComponentSchema definitions from EntropyCore reflection.
 */

#include <Networking/Core/SchemaGeneration.h>
#include <Networking/Core/ComponentSchemaRegistry.h>
#include <TypeSystem/Reflection.h>
#include <Logging/Logger.h>
#include <format>

using namespace EntropyEngine::Networking;
using namespace EntropyEngine::Core::TypeSystem;

// ============================================================================
// Example 1: Basic component with EntropyNetworking types
// ============================================================================

/**
 * @brief Transform component using built-in Vec3 and Quat types
 */
struct Transform {
    ENTROPY_REGISTER_TYPE(Transform);

    ENTROPY_FIELD(Vec3, position) = {0.0f, 0.0f, 0.0f};
    ENTROPY_FIELD(Quat, rotation) = {0.0f, 0.0f, 0.0f, 1.0f};
    ENTROPY_FIELD(Vec3, scale) = {1.0f, 1.0f, 1.0f};
};

// ============================================================================
// Example 2: Component with various property types
// ============================================================================

/**
 * @brief Player component demonstrating multiple property types
 */
struct Player {
    ENTROPY_REGISTER_TYPE(Player);

    ENTROPY_FIELD(int32_t, id) = 0;
    ENTROPY_FIELD(std::string, name) = "Player";
    ENTROPY_FIELD(float, health) = 100.0f;
    ENTROPY_FIELD(bool, isAlive) = true;
    ENTROPY_FIELD(Vec3, velocity) = {0.0f, 0.0f, 0.0f};
};

// ============================================================================
// Example 3: Component with enum (requires custom mapping)
// ============================================================================

/**
 * @brief Light type enumeration
 */
enum class LightType : uint32_t {
    Point = 0,
    Directional = 1,
    Spot = 2
};

/**
 * @brief Light component with enum field
 */
struct Light {
    ENTROPY_REGISTER_TYPE(Light);

    ENTROPY_FIELD(LightType, lightType) = LightType::Point;
    ENTROPY_FIELD(Vec3, color) = {1.0f, 1.0f, 1.0f};
    ENTROPY_FIELD(float, intensity) = 1.0f;
    ENTROPY_FIELD(float, range) = 10.0f;
};

// Register enum mapping (required for enums)
namespace EntropyEngine::Networking {
    template<> struct TypeToPropertyType<LightType> {
        static constexpr PropertyType value = PropertyType::Int32;
    };
}

// ============================================================================
// Example 4: Custom type mapping extension
// ============================================================================

// If your application uses third-party vector libraries (e.g., GLM),
// you can extend the type mapping system:
//
// #include <glm/glm.hpp>
//
// namespace EntropyEngine::Networking {
//     template<> struct TypeToPropertyType<glm::vec3> {
//         static constexpr PropertyType value = PropertyType::Vec3;
//     };
//
//     // Extend runtime mapping
//     inline std::optional<PropertyType> mapTypeIdToPropertyType(TypeID typeId) {
//         if (typeId == createTypeId<glm::vec3>()) return PropertyType::Vec3;
//         return EntropyEngine::Networking::mapTypeIdToPropertyType(typeId);
//     }
//
//     inline size_t getFieldSize(TypeID typeId) {
//         if (typeId == createTypeId<glm::vec3>()) return sizeof(glm::vec3);
//         return EntropyEngine::Networking::getFieldSize(typeId);
//     }
// }

// ============================================================================
// Main example
// ============================================================================

void printSchema(const ComponentSchema& schema) {
    ENTROPY_LOG_INFO(std::format("{}.{} v{} (size: {} bytes, public: {})",
                             schema.appId, schema.componentName,
                             schema.schemaVersion, schema.totalSize,
                             schema.isPublic ? "yes" : "no"));

    ENTROPY_LOG_INFO(std::format("Type Hash: {}:{}", schema.typeHash.high, schema.typeHash.low));
    ENTROPY_LOG_INFO("Properties:");

    for (const auto& prop : schema.properties) {
        ENTROPY_LOG_INFO(std::format("  - {} ({}) @ offset {} (size: {} bytes)",
                                 prop.name,
                                 propertyTypeToString(prop.type),
                                 prop.offset,
                                 prop.size));
    }
}

int main() {
    ENTROPY_LOG_INFO("=== EntropyNetworking Schema Generation Example ===");

    // Create component schema registry
    ComponentSchemaRegistry registry;

    // ========================================================================
    // Example 1: Generate Transform schema
    // ========================================================================

    ENTROPY_LOG_INFO("--- Example 1: Auto-generate Transform schema ---");

    auto transformResult = generateComponentSchema<Transform>("ExampleApp", 1, true);

    if (transformResult.failed()) {
        ENTROPY_LOG_ERROR("Failed to generate Transform schema: " + transformResult.errorMessage);
        return 1;
    }

    printSchema(transformResult.value);

    // Register with registry
    auto transformHash = registry.registerSchema(transformResult.value);
    if (transformHash.failed()) {
        ENTROPY_LOG_ERROR("Failed to register Transform schema");
        return 1;
    }

    ENTROPY_LOG_INFO("✓ Transform schema registered");

    // ========================================================================
    // Example 2: Generate Player schema
    // ========================================================================

    ENTROPY_LOG_INFO("--- Example 2: Auto-generate Player schema ---");

    auto playerResult = generateComponentSchema<Player>("ExampleApp", 1, false);

    if (playerResult.success()) {
        printSchema(playerResult.value);

        auto playerHash = registry.registerSchema(playerResult.value);
        if (playerHash.success()) {
            ENTROPY_LOG_INFO("✓ Player schema registered (private)");
        }
    }

    // ========================================================================
    // Example 3: Generate Light schema (with enum)
    // ========================================================================

    ENTROPY_LOG_INFO("--- Example 3: Auto-generate Light schema (with enum) ---");

    auto lightResult = generateComponentSchema<Light>("ExampleApp", 1, true);

    if (lightResult.success()) {
        printSchema(lightResult.value);

        auto lightHash = registry.registerSchema(lightResult.value);
        if (lightHash.success()) {
            ENTROPY_LOG_INFO("✓ Light schema registered");
        }
    }

    // ========================================================================
    // Example 4: Query registered schemas
    // ========================================================================

    ENTROPY_LOG_INFO("--- Example 4: Query public schemas ---");

    auto publicSchemas = registry.getPublicSchemas();
    ENTROPY_LOG_INFO(std::format("Found {} public schemas:", publicSchemas.size()));

    for (const auto& schema : publicSchemas) {
        ENTROPY_LOG_INFO(std::format("  - {}.{} v{}",
                                 schema.appId,
                                 schema.componentName,
                                 schema.schemaVersion));
    }

    // ========================================================================
    // Example 5: Schema compatibility checking
    // ========================================================================

    ENTROPY_LOG_INFO("--- Example 5: Schema compatibility ---");

    // Generate a second version of Transform with same structure
    auto transformV2Result = generateComponentSchema<Transform>("ExampleApp", 2, true);

    if (transformV2Result.success()) {
        // Same structural hash (same fields)
        bool structurallyCompatible = transformResult.value.isStructurallyCompatible(transformV2Result.value);
        ENTROPY_LOG_INFO(std::format("Transform v1 and v2 structurally compatible: {}",
                                 structurallyCompatible ? "yes" : "no"));

        // Different type hashes (different versions)
        bool sameTypeHash = (transformResult.value.typeHash == transformV2Result.value.typeHash);
        ENTROPY_LOG_INFO(std::format("Transform v1 and v2 same type hash: {}",
                                 sameTypeHash ? "yes" : "no"));
    }

    // ========================================================================
    // Example 6: Manual vs Auto-generated comparison
    // ========================================================================

    ENTROPY_LOG_INFO("--- Example 6: Manual vs Auto-generated ---");

    // Manual schema creation (old way)
    std::vector<PropertyDefinition> manualProps = {
        {"position", PropertyType::Vec3, offsetof(Transform, position), sizeof(Vec3)},
        {"rotation", PropertyType::Quat, offsetof(Transform, rotation), sizeof(Quat)},
        {"scale", PropertyType::Vec3, offsetof(Transform, scale), sizeof(Vec3)}
    };

    auto manualResult = ComponentSchema::create("ExampleApp", "Transform", 1, manualProps, sizeof(Transform), true);

    if (manualResult.success()) {
        ENTROPY_LOG_INFO(std::format("Manual schema: {} properties", manualResult.value.properties.size()));
        ENTROPY_LOG_INFO(std::format("Auto-generated schema: {} properties", transformResult.value.properties.size()));

        // Should produce same structural hash
        bool sameStructure = (manualResult.value.structuralHash == transformResult.value.structuralHash);
        ENTROPY_LOG_INFO(std::format("Schemas structurally identical: {}",
                                 sameStructure ? "yes" : "no"));
    }

    ENTROPY_LOG_INFO("=== Example complete ===");
    ENTROPY_LOG_INFO("");
    ENTROPY_LOG_INFO("Key benefits of auto-generation:");
    ENTROPY_LOG_INFO("  1. Eliminates manual PropertyDefinition boilerplate");
    ENTROPY_LOG_INFO("  2. Automatically extracts offsets and sizes from reflection");
    ENTROPY_LOG_INFO("  3. Reduces errors from manual offset/size calculations");
    ENTROPY_LOG_INFO("  4. Keeps schemas synchronized with component definitions");
    ENTROPY_LOG_INFO("  5. Extensible via template specialization");

    return 0;
}
