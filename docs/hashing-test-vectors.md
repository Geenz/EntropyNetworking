# Canonical String Hashing - Test Vectors

## Overview

This document provides reference test vectors for canonical string hashing used in Entropy Networking. These vectors are critical for implementing cross-language compatibility and validating independent implementations.

## Specification

### Hash Algorithm
- **Hash Function**: SHA-256
- **Output**: First 128 bits (16 bytes) of SHA-256 digest
- **Representation**: Two 64-bit unsigned integers (high, low)

### Canonical String Format

#### Component Schema Canonical Form
```
{appId}.{componentName}@{version}{prop1:type1:offset1:size1,...}
```

**Rules:**
1. Properties are sorted alphabetically by name (ASCII lexicographic order)
2. All identifiers must be ASCII: `[a-zA-Z0-9_]`, starting with letter or underscore
3. Property fields are colon-separated: `name:type:offset:size`
4. Properties are comma-separated
5. Version is decimal integer

**Example:**
```
TestApp.Transform@1{position:Vec3:0:12,rotation:Quat:12:16}
```

#### Property Hash Canonical Form
```
{entityId}:{componentTypeHashHex}:{propertyName}
```

**Rules:**
1. Entity ID is decimal integer
2. Component type hash is 32-character hex (16-byte hash as 32 hex digits)
3. Property name is ASCII identifier

**Example:**
```
12345:1a2b3c4d5e6f7890abcdef0123456789:health
```

### Property Types

Valid property type strings:
- Scalars: `Int32`, `Int64`, `Float32`, `Float64`, `Bool`, `String`, `Bytes`
- Vectors: `Vec2`, `Vec3`, `Vec4`, `Quat`
- Arrays: `Int32Array`, `Int64Array`, `Float32Array`, `Float64Array`, `Vec2Array`, `Vec3Array`, `Vec4Array`, `QuatArray`

## Test Vectors

### Vector 1: Simple Transform Component

**Input Properties:**
```
{"position", Vec3, offset=0, size=12}
{"rotation", Quat, offset=12, size=16}
```

**Metadata:**
- App ID: `TestApp`
- Component Name: `Transform`
- Version: `1`
- Total Size: `28`
- Public: `false`

**Canonical String:**
```
TestApp.Transform@1{position:Vec3:0:12,rotation:Quat:12:16}
```

**Structural Hash Canonical String:**
```
position:Vec3:0:12,rotation:Quat:12:16
```

**Expected Hashes:**
- Structural Hash: Computed from structural canonical string
- Type Hash: Computed from component canonical string

### Vector 2: Physics Component (Property Sorting)

**Input Properties (declared in this order):**
```
{"mass", Float32, offset=0, size=4}
{"velocity", Vec3, offset=4, size=12}
{"acceleration", Vec3, offset=16, size=12}
```

**Metadata:**
- App ID: `PhysicsEngine`
- Component Name: `RigidBody`
- Version: `2`
- Total Size: `28`
- Public: `true`

**Canonical String (note alphabetical sorting):**
```
PhysicsEngine.RigidBody@2{acceleration:Vec3:16:12,mass:Float32:0:4,velocity:Vec3:4:12}
```

**Key Observation:**
Properties are sorted: `acceleration` < `mass` < `velocity` (alphabetically), even though declared in different order.

### Vector 3: Minimal Single Property

**Input Properties:**
```
{"health", Int32, offset=0, size=4}
```

**Metadata:**
- App ID: `GameEngine`
- Component Name: `Health`
- Version: `1`
- Total Size: `4`
- Public: `false`

**Canonical String:**
```
GameEngine.Health@1{health:Int32:0:4}
```

### Vector 4: Complex Multi-Type Schema

**Input Properties (declared in this order):**
```
{"id", Int64, offset=0, size=8}
{"name", String, offset=8, size=64}
{"position", Vec3, offset=72, size=12}
{"rotation", Quat, offset=84, size=16}
{"scale", Vec3, offset=100, size=12}
{"visible", Bool, offset=112, size=1}
{"layer", Int32, offset=116, size=4}
```

**Metadata:**
- App ID: `RenderEngine`
- Component Name: `Drawable`
- Version: `3`
- Total Size: `120`
- Public: `true`

**Canonical String (alphabetically sorted):**
```
RenderEngine.Drawable@3{id:Int64:0:8,layer:Int32:116:4,name:String:8:64,position:Vec3:72:12,rotation:Quat:84:16,scale:Vec3:100:12,visible:Bool:112:1}
```

### Vector 5: Order Independence Verification

**Input Set A (declared in this order):**
```
{"z_last", Float32, offset=0, size=4}
{"a_first", Float32, offset=4, size=4}
{"m_middle", Float32, offset=8, size=4}
```

**Input Set B (declared in this order):**
```
{"a_first", Float32, offset=4, size=4}
{"m_middle", Float32, offset=8, size=4}
{"z_last", Float32, offset=0, size=4}
```

**Metadata (both):**
- App ID: `App`
- Component Name: `Test`
- Version: `1`
- Total Size: `12`

**Canonical String (identical for both):**
```
App.Test@1{a_first:Float32:4:4,m_middle:Float32:8:4,z_last:Float32:0:4}
```

**Key Observation:**
Both input orders produce **identical** canonical strings and hashes, demonstrating order-independence.

### Vector 6: ASCII Identifier Variations

**Input Properties:**
```
{"_private", Int32, offset=0, size=4}
{"snake_case", Int32, offset=4, size=4}
{"camelCase", Int32, offset=8, size=4}
{"PascalCase", Int32, offset=12, size=4}
{"with123numbers", Int32, offset=16, size=4}
```

**Metadata:**
- App ID: `MyApp_v2`
- Component Name: `Test_Component`
- Version: `1`
- Total Size: `20`

**Canonical String:**
```
MyApp_v2.Test_Component@1{PascalCase:Int32:12:4,_private:Int32:0:4,camelCase:Int32:8:4,snake_case:Int32:4:4,with123numbers:Int32:16:4}
```

**Valid Identifier Examples:**
- Starting with underscore: `_private`
- snake_case: `snake_case`
- camelCase: `camelCase`
- PascalCase: `PascalCase`
- Numbers (not at start): `with123numbers`

### Vector 7: Version Differentiation

**Input Properties (identical for all versions):**
```
{"value", Float32, offset=0, size=4}
```

**Metadata:**
- App ID: `App`
- Component Name: `Component`
- Total Size: `4`

**Version 1 Canonical String:**
```
App.Component@1{value:Float32:0:4}
```

**Version 2 Canonical String:**
```
App.Component@2{value:Float32:0:4}
```

**Version 3 Canonical String:**
```
App.Component@3{value:Float32:0:4}
```

**Key Observations:**
- All three versions have **identical** structural hashes (same property layout)
- All three versions have **different** type hashes (version is part of type hash)
- Canonical strings differ only in version number

## Implementation Guidelines

### For C++ (Reference Implementation)

See `ComponentSchema.cpp` and `PropertyHash.cpp` for reference implementation.

### For Other Languages

When implementing canonical string hashing in other languages:

1. **String Encoding**: Use UTF-8 for all string operations
2. **Sorting**: Use standard library lexicographic string comparison (ASCII byte comparison)
3. **Number Formatting**:
   - Entity IDs: Decimal integers (no leading zeros)
   - Offsets/Sizes: Decimal integers (no leading zeros)
   - Versions: Decimal integers (no leading zeros)
   - Hash hex: Lowercase, zero-padded to 16 characters per 64-bit component
4. **Hash Computation**: SHA-256 of UTF-8 bytes, extract first 16 bytes (128 bits)

### Validation

To validate your implementation:

1. Implement the canonical string builder according to specification
2. Run all test vectors through your implementation
3. Compare canonical strings byte-for-byte with reference values
4. Compute hashes and verify structural/type hashes match

### Common Pitfalls

1. **Property Ordering**: Always sort properties alphabetically before building canonical string
2. **Case Sensitivity**: Property names are case-sensitive (`Health` ≠ `health`)
3. **Whitespace**: No spaces in canonical strings (except within String property values)
4. **Zero Padding**: Hash hex values must be zero-padded (use `%016llx` format or equivalent)
5. **Endianness**: When extracting hash bytes, use big-endian byte order (MSB first)

## Cross-Language Compatibility

This canonical string format is designed for maximum portability:

- **JavaScript**: All operations available in standard library
- **C#**: Use `String.Compare` for sorting, `SHA256` class for hashing
- **Rust**: Use `std::cmp::Ord` for sorting, `sha2` crate for hashing
- **Swift**: Use `String.localizedCompare` or simple `<` for ASCII
- **Python**: Built-in `sorted()` and `hashlib.sha256()`

No platform-specific or locale-dependent behavior required.

## Structural vs Type Hashes

### Structural Hash
- **Input**: Property definitions only (name, type, offset, size)
- **Purpose**: Fast equality check for field layout compatibility
- **Same Hash**: Means identical structure, can potentially read from each other
- **Different Apps**: Can have same structural hash with different type hashes

### Type Hash
- **Input**: App ID + Component Name + Version + Structural Hash
- **Purpose**: Globally unique component type identifier
- **Different Hash**: Different app, name, version, OR structure
- **Registry Key**: Used as primary key in ComponentSchemaRegistry

## Version History

- **2025-10-27**: Initial specification with ASCII-only identifiers
  - Chose canonical string format over binary for cross-language stability
  - No Unicode normalization required (ASCII-only identifiers)
  - 128-bit hashes (first 128 bits of SHA-256)
  - Order-independent property hashing via alphabetical sorting
