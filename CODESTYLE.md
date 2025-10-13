# Code Style Guide

This document outlines the overall code style expectations of the project.  Given Entropy has many public components, it's important to have some kind of cohesive style guide for its APIs at minimum.

I generally try to keep these quick and light, and try not to make a big deal of them.  These are more suggestions than rules.

## Indentation

Use spaces, not tabs.  Indent using four spaces.

## Case

### Variables, functions, and classes

Use camelCase.  Please avoid others such as snake_case.  Example:
```c++
class MyClass {
public:
    int myVar = 0;

    void myMethod(int someVar);
};
```

### Class names

Use PascalCase/UpperCamelCase.  Example:

```c++
class MyClass {
// Your class here.
};
```

### Const and constexpr variables

One of the few examples where SNAKE_CASE is permitted.  Please capitalize every letter.  Example:
```c++
const int SOME_INTEGER = 1ULL;
constexpr int A_THIRD_ONE = 1;
```

### Static variables

Prefix static variables with `s`.  Example:

```c++
static int sSomeInt = 1;
```

Static example:
```c++
static const int S_SOME_INTEGER = 1;
```

constexpr is not exempt:
```c++
static constexpr size_t S_ANOTHER_INTEGER = 1ULL;
```

### Thread local variables

Thread local variables are special.  Prefix with a t.  For example:
```c++
thread_local int tSomeInt = 1;

// Static thread local variables still need to be prefixed with s
static thread_local int stSomeInt = 1;
```

#### Modifiers stack

You might have noticed that in the above thread local example, we kind of ignore camelCase in the static + thread_local scenario.

Keep modifiers lower case in these scenarios and make the first word the follows them uppercase.

### Magic numbers/magic values

Avoid them.  We've all been there - "what the hell is 0b00100000!?".  Make sure that these are separated out into constants as appropriate.  Example:
```c++
const uint8_t BUMP_MASK = 0b00000001;
// ...
uint8_t maskedBump = val & BUMP_MASK;
```

## Curly braces

K&R "one true brace" style is preferred, Allman is permitted and may be preferable in some situations.  Some examples:

### K&R "one true brace"
```c++
int main() {
    if (true) {
        printf("Hello world!\n");
    }
}
```
### Allman
```c++
int main() {
    //if (true)
    {
        printf("Hello world!\n");
    }
}
```

## Private vs. protected vs. public

### Variables

Private and protected variables should be prepended with an underscore, and still follow camelCase.  Public should have no underscore.  Example:
```c++
class MyClass {
    int _someInt;
protected:
    int _anotherInt;
public:
    int aThirdInt;
};
```

This does not apply to method names.

## Const correctness

Use it.  Even if the performance benefits may be questionable in some situations, it helps a ton with code readability and maintainability.

If your method does not need to modify state in the class, mark it as const.  If it's returning something that should be immutable, mark its return as const.  If its parameters are not changed, mark them as const.

Some examples:
```c++
class MyClass {
    int _myVar;
public:
    int myVar() const { return _myVar; }
};
```

```c++
class MyClass {
    int _myVar;
public:
    int myCalc(const int firstInt, const int secondInt) const { return firstInt * secondInt + _myVar; }
};
```
```c++
class MyClass {
    int* _myPtr;
public:
    const int* myPtr() const { return _myPtr; }
};
```

## Pointers vs. smart pointers vs. references vs. handles

Massive gray area.

For internal stuff, raw pointers are okay provided there is a clear chain of ownership, but you should probably look at unique_ptr before using a raw pointer.

For things that may be shared around internally, shared_ptr and weak_ptr should largely be your go-to.

For values that don't really benefit so much from reference counting, consider returning and passing around references.  Same applies to "big" things that are getting passed into another function.  Example:
```c++
void someFunc(const std::string& aString);
int& someThing();

std::string theString("This is totally one megabyte of data, probably");
someFunc(theString);

auto &thing = someThing();
```
As always, just be cautious of dangling references.

### Handles
Handles are generally preferred for public facing APIs where there are opportunities to store collections of things that can be centrally managed.

Some great examples of this are gonna be the work contract system, and acyclic graphs.  Contracts are regularly invalidated, so it makes sense to make public facing contracts handles to the work contract group's internal contract storage.

With acyclic graphs, the graph owns the nodes and you simply get a handle to the node.  The graph manages that node for you.

Entropy has a GenericHandle class, and a TypedHandle class in the type system.  You should use them for these scenarios.

## Initialization

### Initializer lists

A bit of a hairy one, I'm generally flexible on it though.  Ideally:

```c++
MyClass::MyClass():
    _varA(0),
    _varB(1),
    _varC(2)
{
}
```

But this is okay too:
```c++
MyClass::MyClass():
    _varA(0), _varB(0), _varC(0), _varD(0)
{
}
```

So is this:
```c++
MyClass::MyClass(): _varA(0), _varB(1)
{
}
```

The main thing you'll notice is bumping the open bracket to a new line when you use them.

### Struct initialization

Pretty flexible here.  This is fine:
```c++
auto theStruct = {
    0, // varA
    1, // varB
    2 // varC
};
```

So long as you document the things that are being initialized.
