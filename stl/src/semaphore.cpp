#include <corecrt_terminate.h>
#include <internal_shared.h>
#include <intrin0.h>

// This file is almost an exact copy of parallel_algorithms minus any threadpools and with some loads for unsigned long
// and usigned long long values as well as adding a timed wait for both of those.
// The below comment is here from parallel_algorithms, in case it's the same here as there I am making sure that nothing
// that can't be allowed there is in here as well by keeping the same header set

// This must be as small as possible, because its contents are
// injected into the msvcprt.lib and msvcprtd.lib import libraries.
// Do not include or define anything else here.
// In particular, basic_string must not be included here.

#if _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8
#pragma comment(lib, "synchronization") // for WaitOnAddress family
#endif // _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8

#if _STL_WIN32_WINNT < _WIN32_WINNT_WIN8
namespace {
    struct _Parallel_init_info {
        unsigned int _Initialized;
#if _STL_WIN32_WINNT < _WIN32_WINNT_VISTA
        decltype(AcquireSRWLockExclusive)* _Pfn_AcquireSRWLockExclusive; // nullptr if _Pfn_WaitOnAddress is non-nullptr
        decltype(ReleaseSRWLockExclusive)* _Pfn_ReleaseSRWLockExclusive; // ditto
        decltype(SleepConditionVariableSRW)* _Pfn_SleepConditionVariableSRW; // ditto
        decltype(WakeConditionVariable)* _Pfn_WakeConditionVariable; // ditto
#endif // _STL_WIN32_WINNT < _WIN32_WINNT_VISTA
        decltype(WaitOnAddress)* _Pfn_WaitOnAddress;
        decltype(WakeByAddressSingle)* _Pfn_WakeByAddressSingle;
    };

    _Parallel_init_info _Parallel_info;

    struct _Wait_semaphore {
        SRWLOCK _Mtx;
        CONDITION_VARIABLE _Cv;
    };

    constexpr int _Wait_table_size      = 256; // one 4k page
    constexpr int _Wait_table_max_index = _Wait_table_size - 1;
    _Wait_semaphore _Wait_table[_Wait_table_size]{};
    size_t _Choose_wait_entry(const volatile void* _Target) noexcept {
        auto _Num = reinterpret_cast<uintptr_t>(_Target);
#ifdef _WIN64
        _Num = (_Num & ((1ull << 32) - 1ull)) ^ (_Num >> 32); // down to 32 bits
#endif // _WIN64
        _Num = (_Num & ((1u << 16) - 1u)) ^ (_Num >> 16); // to 16 bits
        _Num = (_Num & ((1u << 8) - 1u)) ^ (_Num >> 8); // to 8 bits
        static_assert(_Wait_table_max_index == (1 << 8) - 1, "Bad wait table size assumption");
        return _Num;
    }

    unsigned long _Atomic_load_ulong(const volatile unsigned long* _Ptr) noexcept {
        // atomic load of unsigned long, copied from <atomic> except ARM and ARM64 bits
        unsigned long _Value;
#if defined(_M_IX86) || defined(_M_X64)
        _Value = *_Ptr;
        _ReadWriteBarrier();
#else // architecture, ditto no ARM support
#error Unsupported architecture
#endif // architecture
        return _Value;
    }

    unsigned long long _Atomic_load_ullong(const volatile unsigned long long* _Ptr) noexcept {
        // atomic load of unsigned long long, copied from <atomic> except ARM and ARM64 bits
        unsigned long long _Value;
#if defined(_M_IX86) || defined(_M_X64)
        _Value = *_Ptr;
        _ReadWriteBarrier();
#else // architecture, ditto no ARM support
#error Unsupported architecture
#endif // architecture
        return _Value;
    }

    unsigned int _Atomic_load_uint(const volatile unsigned int* _Ptr) noexcept {
        // atomic load of unsigned int, copied from <atomic> except ARM and ARM64 bits
        unsigned int _Value;
#if defined(_M_IX86) || defined(_M_X64)
        _Value = *_Ptr;
        _ReadWriteBarrier();
#else // architecture, ditto no ARM support
#error Unsupported architecture
#endif // architecture
        return _Value;
    }

    void _Atomic_store_uint(volatile unsigned int* _Tgt, unsigned int _Value) {
        // atomic store of unsigned int, copied from <atomic>
#if defined(_M_IX86) || defined(_M_X64)
        _InterlockedExchange((volatile long*) _Tgt, static_cast<long>(_Value));
#else // architecture, ditto no ARM support
#error Unsupported architecture
#endif // architecture
    }

    bool _Initialize_semaphore_init_info() { // try to fill in _Parallel_info
#if !(defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64))
#error Check hardware assumption: Assumes that write races of identical values to pointer-sized variables are benign
#endif // !(defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64))


        HMODULE _KernelBase = GetModuleHandleW(L"kernelbase.dll");
        if (_KernelBase) {
            _Parallel_info._Pfn_WaitOnAddress =
                reinterpret_cast<decltype(WaitOnAddress)*>(GetProcAddress(_KernelBase, "WaitOnAddress"));
            _Parallel_info._Pfn_WakeByAddressSingle =
                reinterpret_cast<decltype(WakeByAddressSingle)*>(GetProcAddress(_KernelBase, "WakeByAddressSingle"));
            if ((_Parallel_info._Pfn_WaitOnAddress == nullptr)
                != (_Parallel_info._Pfn_WakeByAddressSingle == nullptr)) {
                // if we don't have both we can use neither
                _Parallel_info._Pfn_WaitOnAddress       = nullptr;
                _Parallel_info._Pfn_WakeByAddressSingle = nullptr;
            }
        }
        HMODULE _Kernel32 = GetModuleHandleW(L"kernel32.dll");
#if _STL_WIN32_WINNT < _WIN32_WINNT_VISTA
        if (_Parallel_info._Pfn_WaitOnAddress) { // no need for SRWLOCK or CONDITION_VARIABLE if we have WaitOnAddress
            return true;
        }

        _Parallel_info._Pfn_AcquireSRWLockExclusive =
            reinterpret_cast<decltype(AcquireSRWLockExclusive)*>(GetProcAddress(_Kernel32, "AcquireSRWLockExclusive"));
        _Parallel_info._Pfn_ReleaseSRWLockExclusive =
            reinterpret_cast<decltype(ReleaseSRWLockExclusive)*>(GetProcAddress(_Kernel32, "ReleaseSRWLockExclusive"));
        _Parallel_info._Pfn_SleepConditionVariableSRW = reinterpret_cast<decltype(SleepConditionVariableSRW)*>(
            GetProcAddress(_Kernel32, "SleepConditionVariableSRW"));
        _Parallel_info._Pfn_WakeConditionVariable =
            reinterpret_cast<decltype(WakeConditionVariable)*>(GetProcAddress(_Kernel32, "WakeConditionVariable"));

        if (!_Parallel_info._Pfn_AcquireSRWLockExclusive || !_Parallel_info._Pfn_ReleaseSRWLockExclusive
            || !_Parallel_info._Pfn_SleepConditionVariableSRW || !_Parallel_info._Pfn_WakeConditionVariable) {
            // no fallback for WaitOnAddress; shouldn't be possible as these
            // APIs were added at the same time as the Windows Vista threadpool API
            return false;
        }
#endif // _STL_WIN32_WINNT < _WIN32_WINNT_VISTA

        return true;
    }
}
#endif
extern "C" {
void __stdcall __std_semaphore_init_values() {
#if _STL_WIN32_WINNT < _WIN32_WINNT_WIN8
    // _Atomic_load_uint enforces memory ordering in _Initialize_parallel_init_info:
    unsigned int _Result = _Atomic_load_uint(&_Parallel_info._Initialized);
    if (_Result == 0) {
        // Something else should be done here to notify that semaphore isn't availible on this platform
        // But I don't know what
        if (_Initialize_semaphore_init_info()) {
            _Result = 1;
        } else {
            _Result = 0;
        }

        // _Atomic_store_uint enforces memory ordering in _Initialize_parallel_init_info:
        _Atomic_store_uint(&_Parallel_info._Initialized, _Result);
    }
#endif // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 ^^^
}
// returns 1 on timeout, 0 on successful wait, and GetLastError on failure of any other kind
char __stdcall __std_execution_wait_on_ullong_timed(
    const volatile unsigned long long* _Address, unsigned long long _Compare, unsigned long _Time) {
#if _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8
    if (WaitOnAddress(const_cast<volatile unsigned long long*>(_Address), &_Compare, sizeof(unsigned long long), _Time)
        == FALSE) {
        unsigned long _LastError = GetLastError();
        // this API failing should only be possible with a timeout, and we asked for INFINITE
        if (_LastError == ERROR_TIMEOUT) {
            return 1;
        }
        return _LastError;
    }
    return 0;
#else // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8 ^^^ / vvv _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 vvv
    if (_Parallel_info._Pfn_WaitOnAddress) {
        if (_Parallel_info._Pfn_WaitOnAddress(
                const_cast<volatile unsigned long long*>(_Address), &_Compare, sizeof(unsigned long long), _Time)
            == FALSE) {
            unsigned long _LastError = GetLastError();
            // this API failing should only be possible with a timeout, and we asked for INFINITE
            if (_LastError == ERROR_TIMEOUT) {
                return 1;
            }
            return _LastError;
        }

        return 0;
    }

    // fake WaitOnAddress via SRWLOCK and CONDITION_VARIABLE
    for (int _Idx = 0; _Idx < 4096; ++_Idx) { // optimistic non-backoff spin
        // only return if it's no longer the same here (mimicking WaitOnAddress)
        if (_Atomic_load_ullong(_Address) != _Compare) {
            return 0;
        }
    }

    auto& _Wait_entry = _Wait_table[_Choose_wait_entry(_Address)];
#if _STL_WIN32_WINNT < _WIN32_WINNT_VISTA
    _Parallel_info._Pfn_AcquireSRWLockExclusive(&_Wait_entry._Mtx);
    while (_Atomic_load_ullong(_Address) == _Compare) {
        if (_Parallel_info._Pfn_SleepConditionVariableSRW(&_Wait_entry._Cv, &_Wait_entry._Mtx, _Time, 0) == 0) {
            unsigned long _LastError = GetLastError();
            // this API failing should only be possible with a timeout, and we asked for INFINITE
            if (_LastError == ERROR_TIMEOUT) {
                return 1;
            }
            return _LastError;
        }
    }

    _Parallel_info._Pfn_ReleaseSRWLockExclusive(&_Wait_entry._Mtx);
    return 0;
#else // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_VISTA ^^^ / vvv _STL_WIN32_WINNT >= _WIN32_WINNT_VISTA vvv
    AcquireSRWLockExclusive(&_Wait_entry._Mtx);
    while (_Atomic_load_ullong(_Address) == _Compare) {
        if (SleepConditionVariableSRW(&_Wait_entry._Cv, &_Wait_entry._Mtx, _Time, 0) == 0) {
            unsigned long _LastError = GetLastError();
            // this API failing should only be possible with a timeout, and we asked for INFINITE
            if (_LastError == ERROR_TIMEOUT) {
                return 1;
            }
            return _LastError;
        }
    }

    ReleaseSRWLockExclusive(&_Wait_entry._Mtx);
    return 0;
#endif // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_VISTA ^^^
#endif // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 ^^^
}
// returns 1 on timeout, 0 on successful wait, and GetLastError on failure of any other kind
char __stdcall __std_execution_wait_on_ulong_timed(
    const volatile unsigned long* _Address, unsigned long _Compare, unsigned long _Time) noexcept {
#if _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8
    if (WaitOnAddress(const_cast<volatile unsigned long*>(_Address), &_Compare, sizeof(unsigned long), _Time)
        == FALSE) {
        unsigned long _LastError = GetLastError();
        if (_LastError == ERROR_TIMEOUT) {
            return 1;
        }
        return _LastError;
    }
    return 0;
#else // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8 ^^^ / vvv _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 vvv
    if (_Parallel_info._Pfn_WaitOnAddress) {
        if (_Parallel_info._Pfn_WaitOnAddress(
                const_cast<volatile unsigned long*>(_Address), &_Compare, sizeof(unsigned long), _Time)
            == FALSE) {
            unsigned long _LastError = GetLastError();
            if (_LastError == ERROR_TIMEOUT) {
                return 1;
            }
            return _LastError;
        }

        return 0;
    }

    // fake WaitOnAddress via SRWLOCK and CONDITION_VARIABLE
    for (int _Idx = 0; _Idx < 4096; ++_Idx) { // optimistic non-backoff spin
        // only return if it's no longer the same here (mimicking WaitOnAddress)
        if (_Atomic_load_ulong(_Address) != _Compare) {
            return 0;
        }
    }

    auto& _Wait_entry = _Wait_table[_Choose_wait_entry(_Address)];
#if _STL_WIN32_WINNT < _WIN32_WINNT_VISTA
    _Parallel_info._Pfn_AcquireSRWLockExclusive(&_Wait_entry._Mtx);
    while (_Atomic_load_ulong(_Address) == _Compare) {
        if (_Parallel_info._Pfn_SleepConditionVariableSRW(&_Wait_entry._Cv, &_Wait_entry._Mtx, _Time, 0) == 0) {
            unsigned long _LastError = GetLastError();
            if (_LastError == ERROR_TIMEOUT) {
                return 1;
            }
            return _LastError;
        }
    }

    _Parallel_info._Pfn_ReleaseSRWLockExclusive(&_Wait_entry._Mtx);
    return 0;
#else // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_VISTA ^^^ / vvv _STL_WIN32_WINNT >= _WIN32_WINNT_VISTA vvv
    AcquireSRWLockExclusive(&_Wait_entry._Mtx);
    while (_Atomic_load_ulong(_Address) == _Compare) {
        if (SleepConditionVariableSRW(&_Wait_entry._Cv, &_Wait_entry._Mtx, _Time, 0) == 0) {
            unsigned long _LastError = GetLastError();
            if (_LastError == ERROR_TIMEOUT) {
                return 1;
            }
            return _LastError;
        }
    }

    ReleaseSRWLockExclusive(&_Wait_entry._Mtx);
    return 0;
#endif // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_VISTA ^^^
#endif // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 ^^^
}
void __stdcall __std_semaphore_wake_by_address(const volatile void* _Address) noexcept {
#if _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8
    WakeByAddressSingle(const_cast<void*>(_Address));
#else // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_WIN8 ^^^ / vvv _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 vvv
    if (_Parallel_info._Pfn_WakeByAddressSingle) {
        _Parallel_info._Pfn_WakeByAddressSingle(const_cast<void*>(_Address));
    } else {
        auto& _Wait_entry = _Wait_table[_Choose_wait_entry(_Address)];
#if _STL_WIN32_WINNT < _WIN32_WINNT_VISTA
        _Parallel_info._Pfn_AcquireSRWLockExclusive(&_Wait_entry._Mtx);
        _Parallel_info._Pfn_ReleaseSRWLockExclusive(&_Wait_entry._Mtx);
        _Parallel_info._Pfn_WakeConditionVariable(&_Wait_entry._Cv);
#else // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_VISTA ^^^ / vvv _STL_WIN32_WINNT >= _WIN32_WINNT_VISTA vvv
        AcquireSRWLockExclusive(&_Wait_entry._Mtx);
        ReleaseSRWLockExclusive(&_Wait_entry._Mtx);
        WakeConditionVariable(&_Wait_entry._Cv);
#endif // ^^^ _STL_WIN32_WINNT >= _WIN32_WINNT_VISTA ^^^
    }
#endif // ^^^ _STL_WIN32_WINNT < _WIN32_WINNT_WIN8 ^^^
}
}
