#pragma once

#include <array>
#include <memory>

#define __KO_POOL_ITERATABLE_DEV__
//#define __KO_POOL_ITERATABLE_TEST__

#if defined(_MSC_VER)
#define __KO_POOL_UNREACHABLE__() do { __assume(0); } while(0)
#elif defined(__clang__)
#define __KO_POOL_UNREACHABLE__() do { __builtin_unreachable(); } while(0)
#elif defined(__GNUC__)
#define __KO_POOL_UNREACHABLE__() do { __builtin_unreachable(); } while(0)
#else
#define __KO_POOL_UNREACHABLE__()
#endif

#if defined(_MSC_VER)
#define __KO_POOL_FORCE_INLINE__ __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define __KO_POOL_FORCE_INLINE__ __attribute__((always_inline))
#else
#define __KO_POOL_FORCE_INLINE__ inline
#endif

#ifdef __KO_POOL_ITERATABLE_TEST__
#define __KO_POOL_ITERATABLE_ASSERT_TEST__(expression) \
do { \
    if (!(expression)) { \
        std::terminate(); \
    } \
} while (0)

#define __KO_POOL_ITERATABLE_ASSERT_TEST_UNREACHABLE__() \
do { \
    std::terminate(); \
    __KO_POOL_UNREACHABLE__(); \
} while (0)

#else

#define __KO_POOL_ITERATABLE_ASSERT_TEST__(expression)
#define __KO_POOL_ITERATABLE_ASSERT_TEST_UNREACHABLE__() __KO_POOL_UNREACHABLE__()

#endif // __KO_POOL_ITERATABLE_TEST__

#ifdef __KO_POOL_ITERATABLE_DEV__
#define __KO_POOL_ITERATABLE_ASSERT_DEV__(expression) \
do { \
    if (!(expression)) { \
        std::terminate(); \
    } \
} while (0)

#else

#define __KO_POOL_ITERATABLE_ASSERT_DEV__(expression)

#endif // __KO_POOL_ITERATABLE_TEST__

template <typename T>
class KoPoolIterator;

class KoPoolIteratable {
public:

    using USize = size_t;

    static constexpr USize SUBPOOLS_CNT = std::numeric_limits<USize>::digits;

    struct Opt {

        USize elementSizeInBytes = sizeof(USize);
        USize elementAlignment = alignof(USize);
    };

    KoPoolIteratable() = default;
    KoPoolIteratable(const Opt& opt);

    KoPoolIteratable(const KoPoolIteratable&) = delete;
    KoPoolIteratable& operator=(const KoPoolIteratable&) = delete;

    KoPoolIteratable(KoPoolIteratable&&) noexcept;
    KoPoolIteratable& operator=(KoPoolIteratable&&) noexcept;

    template <typename T, typename ...Args>
    T* Allocate(Args&&... args) {

        __KO_POOL_ITERATABLE_ASSERT_DEV__(sizeof(T) == _opt.elementSizeInBytes);
        __KO_POOL_ITERATABLE_ASSERT_DEV__(alignof(T) == _opt.elementAlignment);

        const AllocBytesResult alloc = AllocateBytes();
        if (!alloc.pMemory) {
            return nullptr;
        }

        T* pData = reinterpret_cast<T*>(alloc.pMemory);
        new (pData) T{ std::forward<Args>(args)... };

        return pData;
    }

    template <typename T>
    T* Allocate(T&& data) {

        __KO_POOL_ITERATABLE_ASSERT_DEV__(sizeof(T) == _opt.elementSizeInBytes);
        __KO_POOL_ITERATABLE_ASSERT_DEV__(alignof(T) == _opt.elementAlignment);

        const AllocBytesResult alloc = AllocateBytes();
        if (!alloc.pMemory) {
            return nullptr;
        }

        T* pData = reinterpret_cast<T*>(alloc.pMemory);
        new (pData) T{ std::forward<T>(data) };

        return pData;
    }

    template <typename T>
    void Deallocate(T* pMemory) noexcept {

        if (!pMemory) {
            return;
        }

        pMemory->~T();

        DeallocateBytesByPtr(pMemory);
    }

    template <typename T>
    void DeallocateByID(const USize id) noexcept {

        const PoolID poolID = IDToPtrImpl(id);
        DeallocateBySubPoolID(reinterpret_cast<T*>(poolID.pMemory), poolID.subPoolID);
    }

    template <typename T>
    void DeallocateBySubPoolID(T* pMemory, const USize subPoolID) noexcept {

        __KO_POOL_ITERATABLE_ASSERT_DEV__(subPoolID != SUB_POOL_ID_NONE);
        __KO_POOL_ITERATABLE_ASSERT_DEV__(IsPtrInsideSubPool(pMemory, subPoolID));

        if (!pMemory) {
            return;
        }

        pMemory->~T();

        DeallocateBytesBySubPoolID(pMemory, subPoolID);
    }

    struct AllocBytesResult {
        USize subPoolID = SUB_POOL_ID_NONE;
        uint8_t* pMemory = nullptr;
    };
    AllocBytesResult AllocateBytes() noexcept;

    void DeallocateBytesByPtr(void* pMemory) noexcept;
    void DeallocateBytesByID(const USize id) noexcept;
    void DeallocateBytesByPtrAndSubPoolID(void* pMemory, const USize subPoolID) noexcept;

    void DeallocateBytesAll() noexcept;

    uint8_t* IDToPtr(const USize id) const noexcept;
    USize IDToSubPoolID(const USize id) const noexcept;

    USize FindSubPoolIDByPtr(const void* pMemory) const noexcept;
    USize PtrToID(const void* pMemory, const USize subPoolID) const noexcept;

    template <typename T, std::enable_if_t<std::is_abstract<T>::value>* = nullptr>
    KoPoolIterator<T> GetIterator() const noexcept {

        __KO_POOL_ITERATABLE_ASSERT_DEV__(sizeof(T) <= _opt.elementSizeInBytes);
        __KO_POOL_ITERATABLE_ASSERT_DEV__(alignof(T) == _opt.elementAlignment);

        return KoPoolIterator<T>{ *this };
    }

    template <typename T, std::enable_if_t<!std::is_abstract<T>::value>* = nullptr>
    KoPoolIterator<T> GetIterator() const noexcept {

        __KO_POOL_ITERATABLE_ASSERT_DEV__(sizeof(T) == _opt.elementSizeInBytes);
        __KO_POOL_ITERATABLE_ASSERT_DEV__(alignof(T) == _opt.elementAlignment);

        return KoPoolIterator<T>{ *this };
    }

    bool IsEmpty() const noexcept;

private:

    struct SubPools;

    struct SkipNodeBase;
    struct SkipNodeHead;
    struct SkipNodeTail;

    struct SortedPointer;

    static USize GetSubPoolSize(const USize subPoolID) noexcept;
    static void DeallocateSubPoolMemory(SubPools& subPool, const USize subPoolID) noexcept;

    static __KO_POOL_FORCE_INLINE__ constexpr bool IsPowerOf2(const USize num) noexcept {
        return num != 0 && ((num & (num - 1)) == 0);
    }

    static __KO_POOL_FORCE_INLINE__ uint32_t Count0BitsLeft(const uint32_t num) noexcept;
    static __KO_POOL_FORCE_INLINE__ uint64_t Count0BitsLeft(const uint64_t num) noexcept;

    static __KO_POOL_FORCE_INLINE__ uint32_t Count0BitsRight(const uint32_t num) noexcept;
    static __KO_POOL_FORCE_INLINE__ uint64_t Count0BitsRight(const uint64_t num) noexcept;

    static __KO_POOL_FORCE_INLINE__ USize Log2(const USize num) noexcept;
    static __KO_POOL_FORCE_INLINE__ USize RoundUpToPowerOf2(const USize num) noexcept;

    struct PoolID {
        USize subPoolID = SUB_POOL_ID_NONE;
        USize id = 0;
        uint8_t* pMemory = nullptr;
    };

    PoolID IDToPtrImpl(const USize id) const noexcept;
    USize IDToSubPoolIDImpl(const USize id) const noexcept;
    PoolID PtrToIDImpl(const void* pMemory, const USize subPoolID) const noexcept;
    USize PtrToIDInSubPool(const void* pMemory, const USize subPoolID) const noexcept;

    USize FindSubPoolIDByPtrImpl(const void* pMemory) const noexcept;
    USize FindSortedPointerIDByPtr(const void* pMemory) const noexcept;

    bool IsPtrInsideSubPool(const void* pMemory, const USize subPoolID) const noexcept;
    bool IsSubPoolEmpty(const USize subPoolID) const noexcept;
    void ResetSubPool(const USize subPoolID) noexcept;

    template <USize NUMBER>
    __KO_POOL_FORCE_INLINE__ USize BinarySearchSubPoolIDByPointerPow2Impl(const void* pMemory, const USize offset = 0) const noexcept {

        const USize sortedPointerID =
            BinarySearchSortedPointerIDByPointerPow2Impl<NUMBER>(pMemory, offset);

        __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
        return _pSubPools->sortedPointers[sortedPointerID].subPoolID;
    }

    template <USize NUMBER>
    __KO_POOL_FORCE_INLINE__ USize BinarySearchSortedPointerIDByPointerPow2Impl(const void* pMemory, const USize offset = 0) const noexcept {

        __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
        const KoPoolIteratable::SortedPointer* pSortedPointers = _pSubPools->sortedPointers.data() + offset;

        if (NUMBER == 0) {

            const bool isPtrInsideSubPool =
                pMemory >= pSortedPointers[NUMBER].pMemory &&
                pMemory < pSortedPointers[NUMBER].pMemory +
                    GetSubPoolSize(pSortedPointers[NUMBER].subPoolID) * _opt.elementSizeInBytes;

            __KO_POOL_ITERATABLE_ASSERT_TEST__(isPtrInsideSubPool);

            return offset;
        }

        __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPowerOf2(NUMBER));

        if (!pSortedPointers[NUMBER / 2].pMemory || pMemory < pSortedPointers[NUMBER / 2].pMemory) {
            return BinarySearchSortedPointerIDByPointerPow2Impl<NUMBER / 2>(pMemory, offset);
        }
        else {
            return BinarySearchSortedPointerIDByPointerPow2Impl<NUMBER / 2>(pMemory, offset + NUMBER / 2);
        }
    }

    void InsertSortedPointer(const USize subPoolID) noexcept;
    void RemoveSortedPointer(const USize subPoolID) noexcept;

    SkipNodeTail* HeadToTail(SkipNodeBase* pHeadNode) const noexcept;
    SkipNodeHead* TailToHead(SkipNodeBase* pTailNode) const noexcept;
    void HeadNodeSetPrevFreeSkipNodeTail(SkipNodeBase* pHeadNode, SkipNodeTail* pTailToSet, const USize subPoolID) const noexcept;

    bool IsSkipListNode(const void* pMemory, const USize subPoolID) const noexcept;
    bool IsSkipListNodeByIDInSubPool(const USize idInSubPool, const USize subPoolID) const noexcept;
    void SetIsSkipListNode(const void* pMemory, const USize subPoolID, const bool isSkipListNode) noexcept;

    bool IsRightSkipListNodeSafe(const void* pMemory, const USize subPoolID) const noexcept;
    bool IsLeftSkipListNodeSafe(const void* pMemory, const USize subPoolID) const noexcept;

    void DeallocateBytesImpl(void* pMemory, const USize subPoolID) noexcept;

private:

    class KoPoolIteratorCore {
    public:

        KoPoolIteratorCore(const KoPoolIteratable& pool)
            : _subPoolsWhichHaveAtLeastOneElement(pool._subPoolsWhichHaveAtLeastOneElement)
        {}

        template <typename T>
        __KO_POOL_FORCE_INLINE__ const T* NextAbstract(const KoPoolIteratable& pool) noexcept {

            __KO_POOL_ITERATABLE_ASSERT_TEST__(sizeof(T) <= pool._opt.elementSizeInBytes);
            __KO_POOL_ITERATABLE_ASSERT_TEST__(alignof(T) == pool._opt.elementAlignment);

            __KO_POOL_ITERATABLE_ASSERT_TEST__(pool._pSubPools);
            const SubPools& subPools = *pool._pSubPools;

            for (;;) {

                if (_idInSubPool >= GetSubPoolSize(_subPoolID)) {

                    if (_subPoolsWhichHaveAtLeastOneElement == 0) {
                        return nullptr;
                    }

                    _subPoolID = Count0BitsRight(_subPoolsWhichHaveAtLeastOneElement);
                    _subPoolsWhichHaveAtLeastOneElement &= ~((USize)1 << _subPoolID);

                    _idInSubPool = 0;
                }

                const uint8_t* pMemory = subPools.pointers[_subPoolID];
                __KO_POOL_ITERATABLE_ASSERT_TEST__(pMemory);

                const bool isSkipNode = pool.IsSkipListNodeByIDInSubPool(_idInSubPool, _subPoolID);

                if (isSkipNode) {

                    const USize size = GetSubPoolSize(_subPoolID);

                    if (_idInSubPool + 1 == size) {

                        _idInSubPool += 1;
                        continue;
                    }

                    __KO_POOL_ITERATABLE_ASSERT_TEST__(_idInSubPool + 1 < size);

                    if (pool.IsSkipListNodeByIDInSubPool(_idInSubPool + 1, _subPoolID)) {

                        const USize sizeToTailInBytes =
                            reinterpret_cast<const SkipNodeHead*>(
                                pMemory + _idInSubPool * pool._opt.elementSizeInBytes
                            )->numBytesToTail;

                        __KO_POOL_ITERATABLE_ASSERT_TEST__(sizeToTailInBytes % pool._opt.elementSizeInBytes == 0);

                        const USize sizeToSkip = sizeToTailInBytes / pool._opt.elementSizeInBytes + 1;
                        _idInSubPool += sizeToSkip;

                        if (_idInSubPool == size) {
                            continue;
                        }

                        __KO_POOL_ITERATABLE_ASSERT_TEST__(_idInSubPool < size);
                    }
                    else {

                        _idInSubPool += 1;
                    }
                }

                const T* pResult = reinterpret_cast<const T*>(pMemory + _idInSubPool * pool._opt.elementSizeInBytes);
                _idInSubPool += 1;

                return pResult;
            }
        }

        template <typename T>
        __KO_POOL_FORCE_INLINE__ const T* Next(const KoPoolIteratable& pool) noexcept {

            __KO_POOL_ITERATABLE_ASSERT_TEST__(sizeof(T) == pool._opt.elementSizeInBytes);
            __KO_POOL_ITERATABLE_ASSERT_TEST__(alignof(T) == pool._opt.elementAlignment);

            __KO_POOL_ITERATABLE_ASSERT_TEST__(pool._pSubPools);
            const SubPools& subPools = *pool._pSubPools;

            for (;;) {

                if (_idInSubPool >= GetSubPoolSize(_subPoolID)) {

                    if (_subPoolsWhichHaveAtLeastOneElement == 0) {
                        return nullptr;
                    }

                    _subPoolID = Count0BitsRight(_subPoolsWhichHaveAtLeastOneElement);
                    _subPoolsWhichHaveAtLeastOneElement &= ~((USize)1 << _subPoolID);

                    _idInSubPool = 0;
                }

                const T* pMemory = reinterpret_cast<const T*>(subPools.pointers[_subPoolID]);
                __KO_POOL_ITERATABLE_ASSERT_TEST__(pMemory);

                const bool isSkipNode = pool.IsSkipListNodeByIDInSubPool(_idInSubPool, _subPoolID);

                if (isSkipNode) {

                    const USize size = GetSubPoolSize(_subPoolID);

                    if (_idInSubPool + 1 == size) {

                        _idInSubPool += 1;
                        continue;
                    }

                    __KO_POOL_ITERATABLE_ASSERT_TEST__(_idInSubPool + 1 < size);

                    if (pool.IsSkipListNodeByIDInSubPool(_idInSubPool + 1, _subPoolID)) {

                        const USize sizeToTailInBytes =
                            reinterpret_cast<const SkipNodeHead*>(
                                pMemory + _idInSubPool
                            )->numBytesToTail;

                        __KO_POOL_ITERATABLE_ASSERT_TEST__(sizeToTailInBytes % pool._opt.elementSizeInBytes == 0);

                        const USize sizeToSkip = sizeToTailInBytes / pool._opt.elementSizeInBytes + 1;
                        _idInSubPool += sizeToSkip;

                        if (_idInSubPool == size) {
                            continue;
                        }

                        __KO_POOL_ITERATABLE_ASSERT_TEST__(_idInSubPool < size);
                    }
                    else {

                        _idInSubPool += 1;
                    }
                }

                const T* pResult = pMemory + _idInSubPool;
                _idInSubPool += 1;

                return pResult;
            }
        }

        // Must be called after Allocate...
        __KO_POOL_FORCE_INLINE__ KoPoolIteratorCore GetFixedIteratorAfterAllocate(
            const KoPoolIteratable& pool
        ) const noexcept {

            KoPoolIteratorCore iterator = *this;

            __KO_POOL_ITERATABLE_ASSERT_TEST__(_subPoolID < KoPoolIteratable::DIGITS);
            __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPowerOf2(KoPoolIteratable::DIGITS));

            const USize mask = ~(((USize)1 << _subPoolID) - 1);
            iterator._subPoolsWhichHaveAtLeastOneElement =
                (pool._subPoolsWhichHaveAtLeastOneElement & mask) |
                (_subPoolsWhichHaveAtLeastOneElement & pool._subPoolsWhichHaveAtLeastOneElement);

            return iterator;
        }

        // Must be called immediately after Deallocate...
        __KO_POOL_FORCE_INLINE__ KoPoolIteratorCore GetFixedIteratorAfterDeallocate(
            const KoPoolIteratable& pool, const uint8_t* pDeallocatedMemory
        ) const noexcept {

            KoPoolIteratorCore iterator = *this;

            __KO_POOL_ITERATABLE_ASSERT_TEST__(_subPoolID < KoPoolIteratable::DIGITS);
            __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPowerOf2(KoPoolIteratable::DIGITS));

            const USize mask = ~(((USize)1 << _subPoolID) - 1);
            iterator._subPoolsWhichHaveAtLeastOneElement =
                (pool._subPoolsWhichHaveAtLeastOneElement & mask) |
                (_subPoolsWhichHaveAtLeastOneElement & pool._subPoolsWhichHaveAtLeastOneElement);

            __KO_POOL_ITERATABLE_ASSERT_TEST__(pool._pSubPools);
            if (!pool._pSubPools->pointers[_subPoolID]) {

                iterator._idInSubPool = GetSubPoolSize(_subPoolID);
                return iterator;
            }

            if (!pool.IsPtrInsideSubPool(pDeallocatedMemory, _subPoolID)) {
                return iterator;
            }

            __KO_POOL_ITERATABLE_ASSERT_DEV__(pool.IsSkipListNode(pDeallocatedMemory, _subPoolID));

            const USize idInSubPool = pool.PtrToIDInSubPool(pDeallocatedMemory, _subPoolID);
            if (idInSubPool == iterator._idInSubPool) {

                const bool isLeftSkipListNode = pool.IsLeftSkipListNodeSafe(pDeallocatedMemory, _subPoolID);
                const bool isRightSkipListNode = pool.IsRightSkipListNodeSafe(pDeallocatedMemory, _subPoolID);

                if (isLeftSkipListNode && isRightSkipListNode) {

                    if (pool.IsRightSkipListNodeSafe(pDeallocatedMemory + pool._opt.elementSizeInBytes, _subPoolID)) {

                        // When we Deallocate... and merge 2 blocks we don't change 'numBytesToTail'
                        const USize sizeToTailInBytes =
                            reinterpret_cast<const SkipNodeHead*>(
                                pDeallocatedMemory + pool._opt.elementSizeInBytes
                            )->numBytesToTail;

                        __KO_POOL_ITERATABLE_ASSERT_TEST__(sizeToTailInBytes % pool._opt.elementSizeInBytes == 0);

                        const USize sizeToSkip = sizeToTailInBytes / pool._opt.elementSizeInBytes + 2;
                        iterator._idInSubPool += sizeToSkip;
                    }
                    else {

                        iterator._idInSubPool += 2;
                    }

                    return iterator;
                }

                if (isRightSkipListNode) {

                    return iterator;
                }

                if (isLeftSkipListNode) {

                    iterator._idInSubPool += 1;
                    return iterator;
                }

                iterator._idInSubPool += 1;
                return iterator;
            }

            if (idInSubPool + 1 == iterator._idInSubPool && pool.IsRightSkipListNodeSafe(pDeallocatedMemory, _subPoolID)) {

                if (pool.IsRightSkipListNodeSafe(pDeallocatedMemory + pool._opt.elementSizeInBytes, _subPoolID)) {

                    // When we Deallocate... and merge 2 blocks we don't change 'numBytesToTail'
                    const USize sizeToTailInBytes =
                        reinterpret_cast<const SkipNodeHead*>(
                            pDeallocatedMemory + pool._opt.elementSizeInBytes
                        )->numBytesToTail;

                    __KO_POOL_ITERATABLE_ASSERT_TEST__(sizeToTailInBytes % pool._opt.elementSizeInBytes == 0);

                    const USize sizeToSkip = sizeToTailInBytes / pool._opt.elementSizeInBytes + 1;
                    iterator._idInSubPool += sizeToSkip;
                }
                else {

                    iterator._idInSubPool += 1;
                }
            }

            return iterator;
        }

    private:

        USize _subPoolID = 0;
        USize _idInSubPool = std::numeric_limits<USize>::max();
        USize _subPoolsWhichHaveAtLeastOneElement = 0;
    };

private:

    template <typename T>
    friend class KoPoolIterator;

    static constexpr USize DIGITS = SUBPOOLS_CNT;
    static constexpr USize SUB_POOL_ID_NONE = SUBPOOLS_CNT;

    struct SubPools;

    struct SubPoolsUniquePtrDeleter {
        void operator()(SubPools* ptr) const noexcept;
    };

    struct SkipNodeBase {
        SkipNodeTail* pPrevFreeSkipNodeTail = nullptr;
    };

    struct SkipNodeHead : public SkipNodeBase {
        uintptr_t numBytesToTail = 0;
    };

    struct SkipNodeTail : public SkipNodeBase {
        SkipNodeBase* pNextFreeSkipNodeHead = nullptr;
    };

    struct SortedPointer {

        uint8_t* pMemory = nullptr;
        USize subPoolID = SUB_POOL_ID_NONE;
    };

    struct SubPools {

        // 'USize* pIsSkipListNode' is stored in 'pPrevFreeSkipNodeTail'
        struct Pool : public SkipNodeTail {

#ifdef __KO_POOL_ITERATABLE_DEV__
            USize numUsed = 0;
#endif
        };

        // sum(2^0...2^(DIGITS - 1)) == 2^DIGITS - 1, in 2^0 we store 2 elements see. 'GetSubPoolSize(...)'
        std::array<Pool, DIGITS - 1> pools;
        std::array<uint8_t*, DIGITS - 1> pointers{ nullptr };

        std::array<SortedPointer, DIGITS - 1> sortedPointers{ SortedPointer{} };
        USize sortedPointersSize = 0;
    };

    using SubPoolsUniquePtr = std::unique_ptr<SubPools, SubPoolsUniquePtrDeleter>;

    //static_assert(IsPowerOf2(DIGITS), "");
    static_assert(DIGITS != 0 && ((DIGITS & (DIGITS - 1)) == 0), "");
    static_assert(sizeof(SkipNodeHead) == sizeof(SkipNodeTail), "");
    static_assert(alignof(SkipNodeHead) == alignof(SkipNodeTail), "");

    USize _vacantSubPools = std::numeric_limits<USize>::max();
    USize _subPoolsWhichHaveAtLeastOneElement = 0;
    USize _subPoolToDeallocate = SUB_POOL_ID_NONE;

    SubPoolsUniquePtr _pSubPools = nullptr;

    Opt _opt;
};

// Iterator can be invalidated, so use 'GetFixedIteratorAfterAllocate()' and 'GetFixedIteratorAfterDeallocate(...)'
template <typename T>
class KoPoolIterator {
public:

    KoPoolIterator(const KoPoolIteratable& pool)
        : _core(pool)
        , _pPool(&pool)
    {}

    template <typename U = T, std::enable_if_t<std::is_abstract<U>::value>* = nullptr>
    __KO_POOL_FORCE_INLINE__ T* Next() noexcept {

        return const_cast<T*>(_core.NextAbstract<T>(*_pPool));
    }

    template <typename U = T, std::enable_if_t<std::is_abstract<U>::value>* = nullptr>
    __KO_POOL_FORCE_INLINE__ const T* Next() const noexcept {

        return _core.NextAbstract<T>(*_pPool);
    }

    template <typename U = T, std::enable_if_t<!std::is_abstract<U>::value>* = nullptr>
    __KO_POOL_FORCE_INLINE__ T* Next() noexcept {

        return const_cast<T*>(_core.Next<T>(*_pPool));
    }

    template <typename U = T, std::enable_if_t<!std::is_abstract<U>::value>* = nullptr>
    __KO_POOL_FORCE_INLINE__ T* Next() const noexcept {

        return _core.Next<T>(*_pPool);
    }

    // Must be called after Allocate...
    __KO_POOL_FORCE_INLINE__ KoPoolIterator GetFixedIteratorAfterAllocate() const noexcept {

        KoPoolIterator<T> iterator = *this;
        iterator._core = iterator._core.GetFixedIteratorAfterAllocate(*_pPool);

        return iterator;
    }

    // Must be called immediately after Deallocate...
    __KO_POOL_FORCE_INLINE__ KoPoolIterator GetFixedIteratorAfterDeallocate(
        const void* pDeallocatedMemory
    ) const noexcept {

        KoPoolIterator<T> iterator = *this;
        iterator._core = iterator._core.GetFixedIteratorAfterDeallocate(
            *_pPool, reinterpret_cast<const uint8_t*>(pDeallocatedMemory)
        );

        return iterator;
    }

private:

    const KoPoolIteratable* _pPool = nullptr;
    KoPoolIteratable::KoPoolIteratorCore _core;
};
