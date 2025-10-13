# Documentation Style Guide

This guide defines documentation standards for the Entropy Engine project. Our goal is clear, practical documentation that helps developers understand and use the code effectively.

## Core Principles

1. **Be Concise** - Aim for 3-5 lines of documentation for most methods
2. **Be Practical** - Focus on what users need to know to use the code
3. **Include Examples** - Show real usage with compilable code snippets
4. **Avoid Redundancy** - Don't repeat what the code already makes clear

## Documentation Requirements

### All Public APIs Must Have:
```cpp
/**
 * @brief One-line summary of what this does
 *
 * 2-3 lines explaining when/why to use this. Focus on practical usage,
 * not implementation details.
 *
 * @param paramName What this parameter controls
 * @return What you get back and when it might be invalid/empty
 *
 * If this code is non-obvious in its usage, provide an example.
 * @code
 * // Practical usage example
 * auto result = someFunction(value);
 * if (result.valid()) {
 *     processResult(result);
 * }
 * @endcode
 */
```

### Private APIs Should Have:
- **@brief** explaining internal purpose
- **Why it's private** (only if non-obvious)
- **Key assumptions** about object state
- **Invariants** it maintains

### Keep These Detailed:
- **Constructors/Destructors** - Resource allocation and cleanup behavior
- **Complex algorithms** - Step-by-step explanation if non-trivial
- **Thread safety** - For example: "Can be called from any thread"
- **Code examples** - Always include for non-trivial functionality (worth the space!)

### Keep These Brief:
- **Simple getters/setters** - One line is enough.  Perhaps even consider if you need to document these if it's extremely obvious what they do.
- **Internal methods** - Focus on purpose, not implementation
- **Parameter descriptions** - One line unless special behavior
- **Member variables** - One-line inline comments

## What to Avoid

### Don't Include:
- **Performance claims** without measurements ("fast", "efficient", "optimal")
- **Assumptions** about typical usage without data
- **Teaching comments** about general concepts
- **Redundant explanations** of why something is const/static/private
- **Implementation trivia** that doesn't affect usage

### Important: What NOT to Touch:
- **Implementation comments** (// comments) - These are for maintainers, not API docs
- **Code examples** - Never remove these, they're essential for understanding
- **Critical details** - Keep constructor/destructor behavior, thread safety notes

### Bad Example:
```cpp
/**
 * @brief Gets the count of active items in the container
 *
 * This method returns the number of items that are currently active
 * in the container. It's very efficient with O(1) complexity because
 * we maintain a counter. This is const because it doesn't modify state,
 * which is important for const-correctness. The counter is atomic for
 * thread-safety. This method is commonly used in high-performance scenarios
 * where you need to quickly check how many items are active.
 *
 * @return The number of active items (typically 1000-5000 in production)
 */
size_t getActiveCount() const { return _activeCount; }
```

### Good Example:
```cpp
/**
 * @brief Gets the count of active items
 * @return Number of items currently marked as active
 */
size_t getActiveCount() const { return _activeCount; }
```

## Common Patterns

### For Related Methods:
```cpp
/**
 * @brief Schedules a contract for execution
 * @param handle Contract to schedule
 * @return Success or failure reason
 * @note Called internally by WorkContractHandle::schedule()
 */
```

### For Simple Queries:
```cpp
/**
 * @brief Checks if the container is empty
 * @return true if no elements are present
 */
```

### For Factory Methods:
```cpp
/**
 * @brief Creates a work contract with the given function
 *
 * The returned handle controls when the work runs. Work functions
 * should capture any needed data in the lambda.
 *
 * @param work Function to execute (must be thread-safe)
 * @return Handle to the contract, or invalid if at capacity
 *
 * @code
 * auto handle = group.createContract([data]() {
 *     processData(data);
 * });
 * @endcode
 */
```

## Class Documentation

```cpp
/**
 * @brief Lock-free work contract pool for task scheduling
 *
 * Manages work contracts that can be scheduled and executed by worker threads.
 * Supports thousands of concurrent tasks without locks. Use with WorkService
 * for multi-threaded execution or executeAll() for single-threaded.
 *
 * @code
 * WorkContractGroup group(1024);
 * auto handle = group.createContract([]() { doWork(); });
 * handle.schedule();
 * group.wait();
 * @endcode
 */
```

## File-Level Documentation

Each header should start with:
```cpp
/**
 * @file Filename.h
 * @brief One-line description of this component
 *
 * This file contains [main class/functionality]. It provides [key capability]
 * for [what part of the system].
 */
```

## Member Variable Documentation

```cpp
private:
    std::atomic<size_t> _activeCount{0};      ///< Currently active items
    std::vector<Item> _items;                 ///< Main storage container
    mutable std::mutex _mutex;                ///< Protects item modifications
```

## Remember

Good documentation answers three questions:
1. **What** does this do? (brief)
2. **When** should I use it? (explanation)
3. **How** do I use it? (example)

Keep it concise. If you can't explain it in 3-5 lines, consider whether the API itself is too complex.
