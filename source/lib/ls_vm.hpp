// ================================================================================================
// -*- C++ -*-
// File: ls_vm.hpp
// Author: LuisCM
// Created on: 24/08/2001
// Brief: The Virtual Machine class and Garbage Collection helpers.
// ================================================================================================

#ifndef LAVASCRIPT_VM_HPP
#define LAVASCRIPT_VM_HPP

#include "ls_runtime.hpp"
#include <initializer_list>
#include <functional>
#include <utility>
#include <vector>

namespace lavascript
{

using DataVector = std::vector<Variant>;
using CodeVector = std::vector<Instruction>;

// ========================================================
// Running object pools used by the GC:
// ========================================================

template<typename... Objects>
struct RtObjMemoryBlobImpl final
{
    static constexpr std::size_t Size  = ct::maxOfN(sizeof(Objects)...);
    static constexpr std::size_t Align = ct::maxOfN(alignof(Objects)...);

    alignas(Align) UInt8 blob[Size];
};

// NOTE: This has to be kept up-to-date if new runtime object types are added!
using RtObjMemoryBlobSmall   = RtObjMemoryBlobImpl<LSObject, LSStruct, LSEnum>;
using RtObjMemoryBlobBig     = RtObjMemoryBlobImpl<LSStr, LSArray>;

using SmallRuntimeObjectPool = LSPool<RtObjMemoryBlobSmall, LAVASCRIPT_RT_OBJECT_POOL_GRANULARITY>;
using BigRuntimeObjectPool   = LSPool<RtObjMemoryBlobBig,   LAVASCRIPT_RT_OBJECT_POOL_GRANULARITY>;

// ========================================================
// class LSGC:
// ========================================================

// Simple mark-and-sweep style Garbage Collector.
//
// The GC is only run at the end of script functions
// if there's a minimum number of alive objects, so it
// does not interfere with the cost of every allocation.
// It uses a pair of memory pools, one for small objects
// (up to 64 bytes) and one for big objects (up to 128 bytes),
// so allocation and deallocation costs are amortized constant.
//
// You can manually run the GC in the C++ code by calling
// GC::collectGarbage() and via script with gc_collect().
// Script code can also check for GC need with gc_need_to_collect().
//
// All objects are always reclaimed when the GC instance dies.
class LSGC final
{
public:

    LSGC() = default;

    // Not copyable.
    LSGC(const LSGC &) = delete;
    LSGC & operator = (const LSGC &) = delete;

    template<typename T, typename... Args>
    T * alloc(Args&&... args)
    {
        T * newObj;
        bool isSmall;

        if (sizeof(T) <= smallObjPool.getObjectSize())
        {
            newObj  = (T *)smallObjPool.allocate();
            isSmall = true;
        }
        else if (sizeof(T) <= bigObjPool.getObjectSize())
        {
            newObj  = (T *)bigObjPool.allocate();
            isSmall = false;
        }
        else
        {
            LAVASCRIPT_RUNTIME_EXCEPTION("bad GC allocation size request!");
        }

        ::new(newObj) T(std::forward<Args>(args)...);

        newObj->isSmall = isSmall;
        newObj->gcNext  = gcListHead;
        gcListHead      = newObj;
        ++gcAliveCount;

        return newObj;
    }

    void free(LSObject * obj)
    {
        LAVASCRIPT_ASSERT(obj != nullptr);

        const bool isSmall = obj->isSmall;
        obj->~LSObject();
        --gcAliveCount;

        if (isSmall)
        {
            smallObjPool.deallocate(obj);
        }
        else
        {
            bigObjPool.deallocate(obj);
        }
    }

    void collectGarbage(LSVM & vm);
    void print(std::ostream & os) const;

    const LSObject * getGCListHead() const noexcept { return gcListHead;   }
    int  getAliveObjectsCount()    const noexcept { return gcAliveCount; }
    bool needToCollectGarbage()    const noexcept { return gcAliveCount >= gcMaxCount; }

private:

    void markAll(LSVM & vm);
    void sweep();

    LSObject * gcListHead   = nullptr;
    int        gcAliveCount = 0;  // # alive objects at the moment (linked in the above list).
    int        gcMaxCount   = 16; // The number of objects required to trigger a garbage collection.

    // Depending on the size of allocations, the best size-matching pool is used.
    SmallRuntimeObjectPool smallObjPool; // <= 64  bytes
    BigRuntimeObjectPool   bigObjPool;   // <= 128 bytes
};

inline std::ostream & operator << (std::ostream & os, const LSGC & gc)
{
    // Prints a list of alive GC objects.
    gc.print(os);
    return os;
}

// ========================================================
// class LSGlobalsTable:
// ========================================================

// The Globals Table allows accessing a script global variable
// by its name, as declared in the script source code. You can
// disable this feature to save some memory if this is not needed.
#if LAVASCRIPT_GLOBALS_TABLE
class LSGlobalsTable final
    : public LSRegistry<UInt32>
{
public:

    explicit LSGlobalsTable(DataVector & data) noexcept
        : globalData{ data }
    { }

    // Finds a global variable by its name or returns a null pointer.
    const Variant * getGlobal(ConstRcString * const name) const
    {
        return getGlobalImpl(name);
    }
    const Variant * getGlobal(const char * name) const
    {
        return getGlobalImpl(name);
    }

    // Mutable access. False is returned and nothing is done if the variable is not found.
    bool setGlobal(ConstRcString * const name, const Variant value)
    {
        return setGlobalImpl(name, value);
    }
    bool setGlobal(const char * name, const Variant value)
    {
        return setGlobalImpl(name, value);
    }

    // Add a unique variable to the table. Asserts if an entry with the same name already exists.
    void addGlobal(ConstRcString * name, const UInt32 index)
    {
        LAVASCRIPT_ASSERT(isRcStringValid(name));
        LAVASCRIPT_ASSERT(!getGlobal(name) && "Variable already registered!");
        addInternal(name, index);
    }

    // Print the global variable list.
    void print(std::ostream & os) const
    {
        os << color::white() << "[[ begin global vars dump ]]" << color::restore() << "\n";
        if (!isEmpty())
        {
            for (auto && entry : *this)
            {
                const UInt32 index = entry.second;
                const char * name  = entry.first->chars;
                os << "\"" << name << "\": " << toString(globalData[index]) << "\n";
            }
        }
        else
        {
            os << "(empty)\n";
        }
        os << color::white() << "[[ listed " << getSize() << " variables ]]" << color::restore() << "\n";
    }

private:

    template<typename TName>
    const Variant * getGlobalImpl(TName name) const
    {
        const UInt32 index = findInternal(name, InvalidVarIndex);
        if (index == InvalidVarIndex)
        {
            return nullptr;
        }
        LAVASCRIPT_ASSERT(index < globalData.size());
        return &globalData[index];
    }

    template<typename TName>
    bool setGlobalImpl(TName name, const Variant & value)
    {
        const UInt32 index = findInternal(name, InvalidVarIndex);
        if (index == InvalidVarIndex)
        {
            return false;
        }
        LAVASCRIPT_ASSERT(index < globalData.size());
        globalData[index] = value;
        return true;
    }

    // Dummy value for invalid/unused keys.
    static constexpr UInt32 InvalidVarIndex = UInt32(~0);

    // Reference to the VM data array that owns the global variables.
    DataVector & globalData;
};

inline std::ostream & operator << (std::ostream & os, const LSGlobalsTable & globals)
{
    globals.print(os);
    return os;
}
#endif // LAVASCRIPT_GLOBALS_TABLE

// ========================================================
// class LSVM:
// ========================================================

// The Virtual Machine represents a whole script program.
class LSVM final
{
public:

    // Default program stack size in Variants.
    static constexpr int DefaultStackSize = 8192;

    //
    // Program context:
    //

    LSStack           stack;
    LSStack           locals;
    DataVector        data;
    CodeVector        code;
    LSGC              gc;
    LSTypeTable       types;
    LSFunctionTable   functions;
    LSStack::LSSlice  funcArgs;

    // Optional access to the DataVector by variable names.
    #if LAVASCRIPT_GLOBALS_TABLE
    LSGlobalsTable  globals;
    #endif // LAVASCRIPT_GLOBALS_TABLE

    // Optional call stack for debugging. Each entry is a Function struct pointer.
    #if LAVASCRIPT_SAVE_SCRIPT_CALLSTACK
    LSStack         callstack;
    #endif // LAVASCRIPT_SAVE_SCRIPT_CALLSTACK

    //
    // LSVM interface:
    //

    // loadBuiltIns: Load built-in native libraries (string, array, object helpers, print/panic, etc).
    // stackSize:    Size in Variants of the program stack. Stack size is fixed.
    LSVM(bool loadBuiltIns = true, int stackSize = DefaultStackSize);

    // Not copyable.
    LSVM(const LSVM &) = delete;
    LSVM & operator = (const LSVM &) = delete;

    // Get/set the PC, RVR and return address.
    // Setting the PC and return addr to invalid indexes will assert.
    void setProgramCounter(int target);
    int getProgramCounter() const noexcept { return pc; }

    void setReturnAddress(int target);
    int getReturnAddress() const noexcept { return retAddr; }

    void setReturnValue(Variant retVal) noexcept { rvr = retVal; }
    Variant getReturnValue() const noexcept      { return rvr;   }

    // Executes the whole program from the start.
    // This method will assert at the end that the program
    // stack is empty, to help catching stack mismatch bugs.
    void execute();

    // Executes just the passed instruction. This method is used internally
    // by the VM, you don't usually have to explicitly call it.
    void executeSingleInstruction(OpCode op, UInt32 operandIndex);

    // Executes the given number of instructions from the given starting index.
    // maxInstructionsToExec is clamped to the number of available instructions.
    // This method will NOT assert for an empty stack at the end.
    void execute(int firstInstruction, int maxInstructionsToExec);

    // Executes from the starting instruction to the end of program, conditional to
    // the return value of the callback. If it returns false, execution is interrupted.
    // The instruction passed to the callback is the one about to be executed. It will
    // only be executed if the callback returns true.
    void executeWithCallback(const std::function<bool(LSVM & vm, OpCode, UInt32)> & callback, int firstInstruction);

    // Call a function by its name. The current PC will be saved and restored before
    // this method returns. If the function has no return value, the returned variant
    // will be set to null. You are responsible for passing the expected number of
    // arguments and making sure the types match. Passing the wrong number of wrong
    // types will throw a runtime exception.
    Variant call(const char * funcName) { return call(funcName, {}); } // Call with zero arguments.
    Variant call(const char * funcName, const std::initializer_list<Variant> & arguments);

    // Prints the current Program Counter, data vector and stack.
    void print(std::ostream & os) const;

    // If this functionality is enabled, the call stack function names are
    // preserved for printing. This can be a useful debugging feature.
    #if LAVASCRIPT_SAVE_SCRIPT_CALLSTACK
    void printStackTrace(std::ostream & os) const;
    #endif // LAVASCRIPT_SAVE_SCRIPT_CALLSTACK

private:

    // The "Program Counter" (index of the next
    // instruction to be executed from the code vector).
    int pc = 0;

    // Saved by CALL instructions so that we can push it
    // into the stack when entering a script function.
    // Native functions don't require setting the return address.
    int retAddr = 0;

    // The "Return Value Register" (holds the latest return value from a func call).
    Variant rvr;
};

// ========================================================
// Debug printing helpers:
// ========================================================

void printDataVector(const DataVector & data, std::ostream & os);
void printCodeVector(const CodeVector & code, std::ostream & os);

inline std::ostream & operator << (std::ostream & os, const DataVector & data)
{
    printDataVector(data, os);
    return os;
}
inline std::ostream & operator << (std::ostream & os, const CodeVector & code)
{
    printCodeVector(code, os);
    return os;
}
inline std::ostream & operator << (std::ostream & os, const LSVM & vm)
{
    vm.print(os);
    return os;
}

} // namespace lavascript {}

#endif // LAVASCRIPT_VM_HPP
