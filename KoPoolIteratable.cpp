#include "KoPoolIteratable.h"

namespace {

    void* AlignedMalloc(const size_t sizeInBytes, const size_t alignment) {
#if defined(_MSC_VER)
        return _aligned_malloc(sizeInBytes, alignment);
#else
        return std::aligned_alloc(alignment, sizeInBytes);
#endif
    }

    void AlignedFree(void* ptr) {
#if defined(_MSC_VER)
        _aligned_free(ptr);
#else
        std::free(ptr);
#endif
    }

    inline size_t CeilDiv(const size_t x, const size_t y) {
        return x / y + (x % y != 0 ? 1 : 0);
    }

    template <typename Func>
    struct ScopeDefer {
        ScopeDefer(Func func_) : func(func_) {}
        ~ScopeDefer() { (func)(); }
        Func func;
    };

    struct ScopeDeferUnit {};
    template <typename Func>
    ScopeDefer<Func> operator+(ScopeDeferUnit, Func&& func) noexcept {
        return ScopeDefer<Func>(std::forward<Func>(func));
    }

#define __SCOPEDEFER_CONCAT_MACROS__0(x, y) x##y
#define __SCOPEDEFER_CONCAT_MACROS__(x, y) __SCOPEDEFER_CONCAT_MACROS__0(x, y)
#define defer auto __SCOPEDEFER_CONCAT_MACROS__(_scope_defer_, __COUNTER__) = ScopeDeferUnit{} + [&]()
}

void KoPoolIteratable::SubPoolsUniquePtrDeleter::operator()(SubPools* ptr) const noexcept {

    if (!ptr) {
        return;
    }

    for (USize i = 0; i < (USize)ptr->pointers.size(); ++i) {

#ifdef __KO_POOL_ITERATABLE_DEV__

        // Use 'DeallocateAll()' if you want to call all destructors and deallocate all memory in one call
        __KO_POOL_ITERATABLE_ASSERT_DEV__(ptr->pools[i].numUsed == 0);
#endif

        DeallocateSubPoolMemory(*ptr, i);
    }

    ptr->sortedPointersSize = 0;
    ptr->sortedPointers = { SortedPointer{} };

    AlignedFree(ptr);
}

KoPoolIteratable::KoPoolIteratable(const Opt& opt) {

    __KO_POOL_ITERATABLE_ASSERT_DEV__(IsPowerOf2(opt.elementAlignment));
    __KO_POOL_ITERATABLE_ASSERT_DEV__(opt.elementSizeInBytes >= sizeof(SkipNodeHead));

    _opt.elementSizeInBytes = opt.elementSizeInBytes;
    _opt.elementAlignment = std::max(opt.elementAlignment, alignof(SkipNodeHead));
}

KoPoolIteratable::KoPoolIteratable(KoPoolIteratable&& rhs) noexcept
    : _opt(std::exchange(rhs._opt, Opt{}))
    , _vacantSubPools(std::exchange(rhs._vacantSubPools, std::numeric_limits<USize>::max()))
    , _subPoolsWhichHaveAtLeastOneElement(std::exchange(rhs._subPoolsWhichHaveAtLeastOneElement, 0))
    , _subPoolToDeallocate(std::exchange(rhs._subPoolToDeallocate, SUB_POOL_ID_NONE))
    , _pSubPools(std::exchange(rhs._pSubPools, nullptr))
{}

KoPoolIteratable& KoPoolIteratable::operator=(KoPoolIteratable&& rhs) noexcept {

    if (this == &rhs) {
        return *this;
    }

    _opt = std::exchange(rhs._opt, Opt{});
    _vacantSubPools = std::exchange(rhs._vacantSubPools, std::numeric_limits<USize>::max());
    _subPoolsWhichHaveAtLeastOneElement = std::exchange(rhs._subPoolsWhichHaveAtLeastOneElement, 0);
    _subPoolToDeallocate = std::exchange(rhs._subPoolToDeallocate, SUB_POOL_ID_NONE);
    _pSubPools = std::exchange(rhs._pSubPools, nullptr);

    return *this;
}

bool KoPoolIteratable::IsEmpty() const noexcept {
    return _subPoolsWhichHaveAtLeastOneElement == 0;
}

KoPoolIteratable::AllocBytesResult KoPoolIteratable::AllocateBytes() noexcept {

    if (!_pSubPools) {

        SubPools* pSubPools = reinterpret_cast<SubPools*>(AlignedMalloc(sizeof(SubPools), alignof(SubPools)));
        if (!pSubPools) {
            return AllocBytesResult{};
        }

        new (pSubPools) SubPools{};

        _pSubPools = SubPoolsUniquePtr{ pSubPools };
    }

    const USize subPoolID = Count0BitsRight(_vacantSubPools);

    // overflow (>= 2^DIGITS)
    __KO_POOL_ITERATABLE_ASSERT_TEST__(subPoolID < DIGITS);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

    const USize size = GetSubPoolSize(subPoolID);

    if (!_pSubPools->pointers[subPoolID]) {

        _pSubPools->pointers[subPoolID] = reinterpret_cast<uint8_t*>(
            AlignedMalloc(size * _opt.elementSizeInBytes, _opt.elementAlignment)
        );

        if (!_pSubPools->pointers[subPoolID]) {
            return AllocBytesResult{};
        }

        _pSubPools->pools[subPoolID].pPrevFreeSkipNodeTail = reinterpret_cast<SkipNodeTail*>(
            AlignedMalloc(CeilDiv(size, DIGITS) * sizeof(USize), alignof(USize))
        );

        if (!_pSubPools->pools[subPoolID].pPrevFreeSkipNodeTail) {

            AlignedFree(_pSubPools->pointers[subPoolID]);
            _pSubPools->pointers[subPoolID] = nullptr;

            return AllocBytesResult{};
        }

        ResetSubPool(subPoolID);
        InsertSortedPointer(subPoolID);
    }

    SubPools::Pool& subPool = _pSubPools->pools[subPoolID];

#ifdef __KO_POOL_ITERATABLE_DEV__
    subPool.numUsed += 1;
#endif

    if (_subPoolToDeallocate == subPoolID) {
        _subPoolToDeallocate = SUB_POOL_ID_NONE;
    }

    _subPoolsWhichHaveAtLeastOneElement |= ((USize)1 << subPoolID);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(subPool.pNextFreeSkipNodeHead);
    uint8_t* pMemory = reinterpret_cast<uint8_t*>(subPool.pNextFreeSkipNodeHead);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsSkipListNode(pMemory, subPoolID));
    defer{ SetIsSkipListNode(pMemory, subPoolID, false); };

    if (!IsRightSkipListNodeSafe(pMemory, subPoolID)) {

        const SkipNodeTail* pMemoryTail = reinterpret_cast<SkipNodeTail*>(pMemory);
        __KO_POOL_ITERATABLE_ASSERT_TEST__(pMemoryTail->pPrevFreeSkipNodeTail == &subPool);

        subPool.pNextFreeSkipNodeHead = pMemoryTail->pNextFreeSkipNodeHead;

        HeadNodeSetPrevFreeSkipNodeTail(pMemoryTail->pNextFreeSkipNodeHead, &subPool, subPoolID);

        if (!pMemoryTail->pNextFreeSkipNodeHead) {

            _vacantSubPools &= ~((USize)1 << subPoolID);

#ifdef __KO_POOL_ITERATABLE_DEV__
            __KO_POOL_ITERATABLE_ASSERT_DEV__(subPool.numUsed == size);
#endif
        }

        AllocBytesResult result{};
        result.subPoolID = subPoolID;
        result.pMemory = pMemory;

        return result;
    }

    const SkipNodeHead* pMemoryHead = reinterpret_cast<SkipNodeHead*>(pMemory);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(pMemoryHead->pPrevFreeSkipNodeTail == &subPool);

    SkipNodeHead* pHead = reinterpret_cast<SkipNodeHead*>(pMemory + _opt.elementSizeInBytes);
    if (pMemoryHead->numBytesToTail != _opt.elementSizeInBytes) {

        pHead->pPrevFreeSkipNodeTail = pMemoryHead->pPrevFreeSkipNodeTail;
        pHead->numBytesToTail = pMemoryHead->numBytesToTail - _opt.elementSizeInBytes;
    }
    else {

        __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsRightSkipListNodeSafe(pMemory + _opt.elementSizeInBytes, subPoolID));
    }

    subPool.pNextFreeSkipNodeHead = pHead;

    AllocBytesResult result{};
    result.subPoolID = subPoolID;
    result.pMemory = pMemory;

    return result;
}

void KoPoolIteratable::DeallocateBytesImpl(void* pMemory_, const USize subPoolID) noexcept {

    uint8_t* pMemory = reinterpret_cast<uint8_t*>(pMemory_);

    if (!pMemory) {
        return;
    }

    __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsEmpty());
    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

#ifdef __KO_POOL_ITERATABLE_DEV__
    _pSubPools->pools[subPoolID].numUsed -= 1;
#endif

    _vacantSubPools |= ((USize)1 << subPoolID);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsSkipListNode(pMemory, subPoolID));

    defer{

        SetIsSkipListNode(pMemory, subPoolID, true);

        if (IsSubPoolEmpty(subPoolID)) {

            _subPoolsWhichHaveAtLeastOneElement &= ~((USize)1 << subPoolID);

            if (_subPoolToDeallocate == SUB_POOL_ID_NONE) {
                _subPoolToDeallocate = subPoolID;
            }
            else {

                if (subPoolID < _subPoolToDeallocate) {

                    RemoveSortedPointer(_subPoolToDeallocate);
                    DeallocateSubPoolMemory(*_pSubPools, _subPoolToDeallocate);

                    _subPoolToDeallocate = subPoolID;
                }
                else {

                    RemoveSortedPointer(subPoolID);
                    DeallocateSubPoolMemory(*_pSubPools, subPoolID);
                }
            }
        }
    };

    const bool isLeftSkipListNode = IsLeftSkipListNodeSafe(pMemory, subPoolID);
    const bool isRightSkipListNode = IsRightSkipListNodeSafe(pMemory, subPoolID);

    if (isLeftSkipListNode && isRightSkipListNode) {

        SkipNodeTail* pTailLeft = reinterpret_cast<SkipNodeTail*>(pMemory - _opt.elementSizeInBytes);
        __KO_POOL_ITERATABLE_ASSERT_TEST__(pTailLeft->pPrevFreeSkipNodeTail);

        const bool isNextLeftSkipNode = IsLeftSkipListNodeSafe(pMemory - _opt.elementSizeInBytes, subPoolID);

        SkipNodeHead* pHeadLeft = isNextLeftSkipNode
            ? TailToHead(pTailLeft)
            : reinterpret_cast<SkipNodeHead*>(pTailLeft);

        __KO_POOL_ITERATABLE_ASSERT_TEST__(pHeadLeft->pPrevFreeSkipNodeTail);

        SkipNodeBase* pRightBase = reinterpret_cast<SkipNodeBase*>(pMemory + _opt.elementSizeInBytes);
        __KO_POOL_ITERATABLE_ASSERT_TEST__(pRightBase->pPrevFreeSkipNodeTail);

        const uintmax_t numBytesToTailRight = IsRightSkipListNodeSafe(pMemory + _opt.elementSizeInBytes, subPoolID)
            ? static_cast<SkipNodeHead*>(pRightBase)->numBytesToTail
            : 0;

        HeadNodeSetPrevFreeSkipNodeTail(pTailLeft->pNextFreeSkipNodeHead, pTailLeft->pPrevFreeSkipNodeTail, subPoolID);
        pTailLeft->pPrevFreeSkipNodeTail->pNextFreeSkipNodeHead = pTailLeft->pNextFreeSkipNodeHead;

        pTailLeft->pPrevFreeSkipNodeTail = pRightBase->pPrevFreeSkipNodeTail;
        pRightBase->pPrevFreeSkipNodeTail->pNextFreeSkipNodeHead = pHeadLeft;

        if (isNextLeftSkipNode) {

            pHeadLeft->pPrevFreeSkipNodeTail = pRightBase->pPrevFreeSkipNodeTail;
            pHeadLeft->numBytesToTail += _opt.elementSizeInBytes * 2 + numBytesToTailRight;
        }
        else {

            pHeadLeft->numBytesToTail = _opt.elementSizeInBytes * 2 + numBytesToTailRight;
        }

        return;
    }

    if (isLeftSkipListNode) {

        SkipNodeTail* pTailOld = reinterpret_cast<SkipNodeTail*>(pMemory - _opt.elementSizeInBytes);

        SkipNodeTail* pTailNew = reinterpret_cast<SkipNodeTail*>(pMemory);
        pTailNew->pPrevFreeSkipNodeTail = pTailOld->pPrevFreeSkipNodeTail;
        pTailNew->pNextFreeSkipNodeHead = pTailOld->pNextFreeSkipNodeHead;

        if (IsLeftSkipListNodeSafe(pMemory - _opt.elementSizeInBytes, subPoolID)) {

            SkipNodeHead* pHead = TailToHead(pTailOld);
            pHead->numBytesToTail += _opt.elementSizeInBytes;
        }
        else {

            SkipNodeHead* pHead = reinterpret_cast<SkipNodeHead*>(pTailOld);
            pHead->numBytesToTail = _opt.elementSizeInBytes;
        }

        HeadNodeSetPrevFreeSkipNodeTail(pTailNew->pNextFreeSkipNodeHead, pTailNew, subPoolID);

        return;
    }

    if (isRightSkipListNode) {

        const SkipNodeBase* pNodeOld = reinterpret_cast<SkipNodeBase*>(pMemory + _opt.elementSizeInBytes);

        SkipNodeHead* pHeadNew = reinterpret_cast<SkipNodeHead*>(pMemory);
        pHeadNew->pPrevFreeSkipNodeTail = pNodeOld->pPrevFreeSkipNodeTail;
        pHeadNew->numBytesToTail = _opt.elementSizeInBytes;

        if (IsRightSkipListNodeSafe(pMemory + _opt.elementSizeInBytes, subPoolID)) {

            const SkipNodeHead* pHeadOld = static_cast<const SkipNodeHead*>(pNodeOld);
            pHeadNew->numBytesToTail += pHeadOld->numBytesToTail;
        }

        pHeadNew->pPrevFreeSkipNodeTail->pNextFreeSkipNodeHead = pHeadNew;

        return;
    }

    SubPools::Pool& subPool = _pSubPools->pools[subPoolID];

    SkipNodeTail* pTail = reinterpret_cast<SkipNodeTail*>(pMemory);
    pTail->pPrevFreeSkipNodeTail = &subPool;
    pTail->pNextFreeSkipNodeHead = subPool.pNextFreeSkipNodeHead;

    subPool.pNextFreeSkipNodeHead = pTail;
    HeadNodeSetPrevFreeSkipNodeTail(pTail->pNextFreeSkipNodeHead, pTail, subPoolID);
}

void KoPoolIteratable::DeallocateBytesByPtr(void* pMemory_) noexcept {

    uint8_t* pMemory = reinterpret_cast<uint8_t*>(pMemory_);

    if (!pMemory) {
        return;
    }

    const USize subPoolID = FindSubPoolIDByPtrImpl(pMemory);
    return DeallocateBytesImpl(pMemory, subPoolID);
}

void KoPoolIteratable::DeallocateBytesByID(const USize id) noexcept {

    const PoolID poolID = IDToPtrImpl(id);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(poolID.subPoolID != SUB_POOL_ID_NONE);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPtrInsideSubPool(poolID.pMemory, poolID.subPoolID));

    DeallocateBytesByPtrAndSubPoolID(poolID.pMemory, poolID.subPoolID);
}

void KoPoolIteratable::DeallocateBytesByPtrAndSubPoolID(void* pMemory, const USize subPoolID) noexcept {

    DeallocateBytesImpl(pMemory, subPoolID);
}

void KoPoolIteratable::DeallocateBytesAll() noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

    for (USize i = 0; i < (USize)_pSubPools->pointers.size(); ++i) {

        DeallocateSubPoolMemory(*_pSubPools, i);
    }

    _vacantSubPools = std::numeric_limits<USize>::max();
    _subPoolsWhichHaveAtLeastOneElement = 0;
    _subPoolToDeallocate = SUB_POOL_ID_NONE;

    _pSubPools->sortedPointersSize = 0;
    _pSubPools->sortedPointers = { SortedPointer{} };
}

uint8_t* KoPoolIteratable::IDToPtr(const USize id) const noexcept {

    const PoolID poolID = IDToPtrImpl(id);
    return poolID.pMemory;
}

KoPoolIteratable::USize KoPoolIteratable::IDToSubPoolID(const USize id) const noexcept {

    return IDToSubPoolIDImpl(id);
}

KoPoolIteratable::USize KoPoolIteratable::FindSubPoolIDByPtr(const void* pMemory) const noexcept {

    const USize subPoolID = FindSubPoolIDByPtrImpl(pMemory);
    __KO_POOL_ITERATABLE_ASSERT_DEV__(subPoolID != SUB_POOL_ID_NONE);
    __KO_POOL_ITERATABLE_ASSERT_DEV__(IsPtrInsideSubPool(pMemory, subPoolID));

    return subPoolID;
}

KoPoolIteratable::USize KoPoolIteratable::PtrToID(const void* pMemory, const USize subPoolID) const noexcept {

    const PoolID poolID = PtrToIDImpl(pMemory, subPoolID);
    return poolID.id;
}

KoPoolIteratable::USize KoPoolIteratable::GetSubPoolSize(const USize subPoolID) noexcept {
    return subPoolID == 0 ? 2 : (USize)1 << subPoolID;
}

void KoPoolIteratable::InsertSortedPointer(const USize subPoolID) noexcept {

    SortedPointer sortedPointer{};
    sortedPointer.pMemory = _pSubPools->pointers[subPoolID];
    sortedPointer.subPoolID = subPoolID;

    _pSubPools->sortedPointers[_pSubPools->sortedPointersSize] = sortedPointer;

    USize sortedInsertIdx = _pSubPools->sortedPointersSize;
    while (sortedInsertIdx > 0 && _pSubPools->sortedPointers[sortedInsertIdx - 1].pMemory > _pSubPools->sortedPointers[sortedInsertIdx].pMemory) {

        std::swap(_pSubPools->sortedPointers[sortedInsertIdx - 1], _pSubPools->sortedPointers[sortedInsertIdx]);
        sortedInsertIdx -= 1;
    }

    _pSubPools->sortedPointersSize += 1;
}

void KoPoolIteratable::RemoveSortedPointer(const USize subPoolID) noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

    USize idxToRemove = FindSortedPointerIDByPtr(_pSubPools->pointers[subPoolID]);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(idxToRemove != SUB_POOL_ID_NONE);

    while (idxToRemove + 1 < _pSubPools->sortedPointersSize) {

        std::swap(_pSubPools->sortedPointers[idxToRemove], _pSubPools->sortedPointers[idxToRemove + 1]);
        idxToRemove += 1;
    }

    _pSubPools->sortedPointers[idxToRemove] = SortedPointer{};
    _pSubPools->sortedPointersSize -= 1;
}

void KoPoolIteratable::DeallocateSubPoolMemory(SubPools& subPool, const USize subPoolID) noexcept {

    AlignedFree(subPool.pointers[subPoolID]);
    subPool.pointers[subPoolID] = nullptr;

    AlignedFree(subPool.pools[subPoolID].pPrevFreeSkipNodeTail);
    subPool.pools[subPoolID].pPrevFreeSkipNodeTail = nullptr;

    subPool.pools[subPoolID].pNextFreeSkipNodeHead = nullptr;

#ifdef __KO_POOL_ITERATABLE_DEV__
    __KO_POOL_ITERATABLE_ASSERT_DEV__(subPool.pools[subPoolID].numUsed == 0);
    subPool.pools[subPoolID].numUsed = 0;
#endif
}

uint32_t KoPoolIteratable::Count0BitsLeft(const uint32_t num) noexcept {

#ifdef _MSC_VER

    unsigned long result;
    const uint8_t isNonZero = _BitScanReverse(&result, num);
    return isNonZero ? 31 - static_cast<uint32_t>(result) : 32;
#else

    return num != 0
        ? __builtin_clzl(num)
        : 32;
#endif
}

uint64_t KoPoolIteratable::Count0BitsLeft(const uint64_t num) noexcept {

#ifdef _MSC_VER

    unsigned long result;
    const uint8_t isNonZero = _BitScanReverse64(&result, num);
    return isNonZero ? 63 - static_cast<uint64_t>(result) : 64;
#else
    return num != 0
        ? __builtin_clzll(num)
        : 64;
#endif
}

uint32_t KoPoolIteratable::Count0BitsRight(const uint32_t num) noexcept {

#ifdef _MSC_VER

    unsigned long result;
    const uint8_t isNonZero = _BitScanForward(&result, num);
    return isNonZero ? static_cast<uint32_t>(result) : 32;
#else

    return num != 0
        ? __builtin_ctzl(num)
        : 32;
#endif
}

uint64_t KoPoolIteratable::Count0BitsRight(const uint64_t num) noexcept {

#ifdef _MSC_VER

    unsigned long result;
    const uint8_t isNonZero = _BitScanForward64(&result, num);
    return isNonZero ? static_cast<uint64_t>(result) : 64;
#else

    return num != 0
        ? __builtin_ctzll(num)
        : 64;
#endif
}

KoPoolIteratable::USize KoPoolIteratable::Log2(const USize num) noexcept {

    if (num == 0) {
        return 0;
    }

    return (DIGITS - 1) - Count0BitsLeft(num);
}

KoPoolIteratable::USize KoPoolIteratable::RoundUpToPowerOf2(const USize num) noexcept {

    if (num == std::numeric_limits<USize>::max()) {
        return num;
    }

    if (IsPowerOf2(num)) {
        return num;
    }

    return (USize)1 << (DIGITS - Count0BitsLeft(num));
}

KoPoolIteratable::USize KoPoolIteratable::FindSubPoolIDByPtrImpl(const void* pMemory) const noexcept {

    const USize sortedPointerID = FindSortedPointerIDByPtr(pMemory);
    return _pSubPools->sortedPointers[sortedPointerID].subPoolID;
}

KoPoolIteratable::USize KoPoolIteratable::FindSortedPointerIDByPtr(const void* pMemory) const noexcept {

    const USize sortedPointersSizePow2 = RoundUpToPowerOf2(_pSubPools->sortedPointersSize);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(sortedPointersSizePow2 > 0 && sortedPointersSizePow2 <= DIGITS);

    switch (sortedPointersSizePow2) {
    case 1: {
        return 0;
    }
    case 2: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<2>(pMemory);
    }
    case 4: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<4>(pMemory);
    }
    case 8: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<8>(pMemory);
    }
    case 16: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<16>(pMemory);
    }
    case 32: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<32>(pMemory);
    }
    case 64: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<64>(pMemory);
    }
    default: {
        return BinarySearchSortedPointerIDByPointerPow2Impl<DIGITS>(pMemory);
    }
    }

    //for (USize i = 0; i < (USize)_pSubPools->sortedPointers.size(); ++i) {

    //	if (IsPtrInsideSubPool(pMemory, _pSubPools->sortedPointers[i].subPoolID)) {
    //		return i;
    //	}
    //}

    //// Allocated outside of the pool
    //__KO_POOL_ITERATABLE_ASSERT_TEST_UNREACHABLE__();
    //return SUB_POOL_ID_NONE;
}

KoPoolIteratable::PoolID KoPoolIteratable::IDToPtrImpl(const USize id) const noexcept {

    const USize subPoolID = IDToSubPoolIDImpl(id);
    const USize baseID = subPoolID == 0
        ? 0
        // in 2^0 we store 2 elements
        : ((USize)1 << subPoolID);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools->pointers[subPoolID]);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(id >= baseID);
    uint8_t* pMemory = _pSubPools->pointers[subPoolID] + (id - baseID) * _opt.elementSizeInBytes;

    PoolID result{};
    result.subPoolID = subPoolID;
    result.id = id;
    result.pMemory = pMemory;

    return result;
}

KoPoolIteratable::USize KoPoolIteratable::IDToSubPoolIDImpl(const USize id) const noexcept {

    const USize subPoolID = Log2(id);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(subPoolID < SUBPOOLS_CNT);

    return subPoolID;
}

KoPoolIteratable::PoolID KoPoolIteratable::PtrToIDImpl(const void* pMemory, const USize subPoolID) const noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPtrInsideSubPool(pMemory, subPoolID));

    const USize baseID = subPoolID == 0
        ? 0
        // in 2^0 we store 2 elements
        : ((USize)1 << subPoolID);

    const USize id = baseID + PtrToIDInSubPool(pMemory, subPoolID);

    PoolID result{};
    result.subPoolID = subPoolID;
    result.id = id;
    result.pMemory = nullptr;

    return result;
}

KoPoolIteratable::USize KoPoolIteratable::PtrToIDInSubPool(const void* pMemory, const USize subPoolID) const noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPtrInsideSubPool(pMemory, subPoolID));

    const USize offsetInBytes = static_cast<USize>(
        reinterpret_cast<uintptr_t>(pMemory) - reinterpret_cast<uintptr_t>(_pSubPools->pointers[subPoolID])
    );

    __KO_POOL_ITERATABLE_ASSERT_TEST__(offsetInBytes % _opt.elementSizeInBytes == 0);
    return offsetInBytes / _opt.elementSizeInBytes;
}

bool KoPoolIteratable::IsPtrInsideSubPool(const void* pMemory, const size_t subPoolID) const noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

    return
        _pSubPools->pointers[subPoolID] &&
        pMemory >= _pSubPools->pointers[subPoolID] &&
        pMemory < _pSubPools->pointers[subPoolID] + GetSubPoolSize(subPoolID) * _opt.elementSizeInBytes;
}

bool KoPoolIteratable::IsSubPoolEmpty(const USize subPoolID) const noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

    const SubPools::Pool& subPool = _pSubPools->pools[subPoolID];

    const bool isEmpty =
        IsRightSkipListNodeSafe(subPool.pNextFreeSkipNodeHead, subPoolID) &&
        static_cast<SkipNodeHead*>(subPool.pNextFreeSkipNodeHead)
            ->numBytesToTail == (GetSubPoolSize(subPoolID) - 1) * _opt.elementSizeInBytes;

#ifdef __KO_POOL_ITERATABLE_DEV__
    if (isEmpty) {
        __KO_POOL_ITERATABLE_ASSERT_DEV__(subPool.numUsed == 0);
    }
#endif

    return isEmpty;
}

void KoPoolIteratable::ResetSubPool(const USize subPoolID) noexcept {

    const USize size = GetSubPoolSize(subPoolID);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(size > 1);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
    SubPools::Pool& subPool = _pSubPools->pools[subPoolID];

    std::memset(subPool.pPrevFreeSkipNodeTail, std::numeric_limits<int>::max(), CeilDiv(size, DIGITS) * sizeof(USize));

    SkipNodeHead* pHead = reinterpret_cast<SkipNodeHead*>(_pSubPools->pointers[subPoolID]);
    SkipNodeTail* pTail = reinterpret_cast<SkipNodeTail*>(_pSubPools->pointers[subPoolID] + (size - 1) * _opt.elementSizeInBytes);

    pHead->pPrevFreeSkipNodeTail = &subPool;
    pHead->numBytesToTail = (size - 1) * _opt.elementSizeInBytes;

    pTail->pPrevFreeSkipNodeTail = &subPool;
    pTail->pNextFreeSkipNodeHead = nullptr;

    subPool.pNextFreeSkipNodeHead = pHead;
    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsPtrInsideSubPool((uint8_t*)pHead, subPoolID));
}

KoPoolIteratable::SkipNodeTail* KoPoolIteratable::HeadToTail(SkipNodeBase* pHeadNode) const noexcept {

    uint8_t* pHeadNodeBytes = reinterpret_cast<uint8_t*>(pHeadNode);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(pHeadNode);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(pHeadNode->pPrevFreeSkipNodeTail);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsLeftSkipListNodeSafe(pHeadNodeBytes, FindSubPoolIDByPtrImpl(pHeadNodeBytes)));
    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsRightSkipListNodeSafe(pHeadNodeBytes, FindSubPoolIDByPtrImpl(pHeadNodeBytes)));

    return reinterpret_cast<SkipNodeTail*>(
        pHeadNodeBytes + reinterpret_cast<SkipNodeHead*>(pHeadNode)->numBytesToTail
    );
}

KoPoolIteratable::SkipNodeHead* KoPoolIteratable::TailToHead(SkipNodeBase* pTailNode) const noexcept {

    const uint8_t* pTailNodeBytes = reinterpret_cast<uint8_t*>(pTailNode);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(pTailNode);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(pTailNode->pPrevFreeSkipNodeTail);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(IsLeftSkipListNodeSafe(pTailNodeBytes, FindSubPoolIDByPtrImpl(pTailNodeBytes)));
    __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsRightSkipListNodeSafe(pTailNodeBytes, FindSubPoolIDByPtrImpl(pTailNodeBytes)));

    return reinterpret_cast<SkipNodeHead*>(pTailNode->pPrevFreeSkipNodeTail->pNextFreeSkipNodeHead);
}

void KoPoolIteratable::HeadNodeSetPrevFreeSkipNodeTail(
    SkipNodeBase* pHeadNode, SkipNodeTail* pTailToSet, const USize subPoolID
) const noexcept {

    if (!pHeadNode) {
        return;
    }

    uint8_t* pHeadNodeBytes = reinterpret_cast<uint8_t*>(pHeadNode);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(FindSubPoolIDByPtrImpl(pHeadNodeBytes) == subPoolID);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsLeftSkipListNodeSafe(pHeadNodeBytes, subPoolID));

    const uint8_t* pTailNodeBytes = reinterpret_cast<uint8_t*>(pTailToSet);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(pTailToSet);

#ifdef __KO_POOL_ITERATABLE_TEST__

    if (pTailToSet != &_pSubPools->pools[subPoolID]) {

        __KO_POOL_ITERATABLE_ASSERT_TEST__(FindSubPoolIDByPtrImpl(pTailNodeBytes) == subPoolID);
        __KO_POOL_ITERATABLE_ASSERT_TEST__(!IsRightSkipListNodeSafe(pTailNodeBytes, subPoolID));
    }
#endif

    pHeadNode->pPrevFreeSkipNodeTail = pTailToSet;

    if (IsRightSkipListNodeSafe(pHeadNode, subPoolID)) {

        SkipNodeTail* pTail = HeadToTail(pHeadNode);
        pTail->pPrevFreeSkipNodeTail = pTailToSet;
    }
}

bool KoPoolIteratable::IsSkipListNode(const void* pMemory, const USize subPoolID) const noexcept {

    return IsSkipListNodeByIDInSubPool(PtrToIDInSubPool(pMemory, subPoolID), subPoolID);
}

bool KoPoolIteratable::IsSkipListNodeByIDInSubPool(const USize idInSubPool, const USize subPoolID) const noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
    const SubPools::Pool& subPool = _pSubPools->pools[subPoolID];

    __KO_POOL_ITERATABLE_ASSERT_TEST__(subPool.pPrevFreeSkipNodeTail);

    const USize bitID = (idInSubPool & (DIGITS - 1));

    return ((reinterpret_cast<USize*>(subPool.pPrevFreeSkipNodeTail)[idInSubPool / DIGITS] >> bitID) & 0b1) == 1;
}

void KoPoolIteratable::SetIsSkipListNode(const void* pMemory, const USize subPoolID, const bool isSkipListNode) noexcept {

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
    const SubPools::Pool& subPool = _pSubPools->pools[subPoolID];

    const USize id = PtrToIDInSubPool(pMemory, subPoolID);
    const USize bitID = (id & (DIGITS - 1));

    if (isSkipListNode) {

        reinterpret_cast<USize*>(subPool.pPrevFreeSkipNodeTail)[id / DIGITS] |= ((USize)1 << bitID);
    }
    else {

        reinterpret_cast<USize*>(subPool.pPrevFreeSkipNodeTail)[id / DIGITS] &= ~((USize)1 << bitID);
    }
}

bool KoPoolIteratable::IsRightSkipListNodeSafe(const void* pMemory_, const USize subPoolID) const noexcept {

    const uint8_t* pMemory = reinterpret_cast<const uint8_t*>(pMemory_);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);
    __KO_POOL_ITERATABLE_ASSERT_TEST__(
        pMemory + _opt.elementSizeInBytes <=
            _pSubPools->pointers[subPoolID] + GetSubPoolSize(subPoolID) * _opt.elementSizeInBytes
    );

    const bool isEnd = pMemory + _opt.elementSizeInBytes ==
        _pSubPools->pointers[subPoolID] + GetSubPoolSize(subPoolID) * _opt.elementSizeInBytes;

    return !isEnd && IsSkipListNode(pMemory + _opt.elementSizeInBytes, subPoolID);
}

bool KoPoolIteratable::IsLeftSkipListNodeSafe(const void* pMemory_, const USize subPoolID) const noexcept {

    const uint8_t* pMemory = reinterpret_cast<const uint8_t*>(pMemory_);

    __KO_POOL_ITERATABLE_ASSERT_TEST__(_pSubPools);

    const bool isBegin = pMemory == _pSubPools->pointers[subPoolID];
    return !isBegin && IsSkipListNode(pMemory - _opt.elementSizeInBytes, subPoolID);
}
