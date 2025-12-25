#include <vector>
#include <string>
#include <random>
#include <chrono>
#include <iostream>

#include "unordered_dense.h"
#include "KoPoolIteratable.h"

#define DevAssert(expression, message) \
do { \
    if (!(expression)) { \
        std::cerr << message; \
        __debugbreak(); \
        std::terminate(); \
    } \
} while (0)

class TestCtx {
public:

    struct Data {

        float x;
        float y;
        float z;

        std::string name = "Data";

        size_t cnt = 1;
    };

    TestCtx(KoPoolIteratable& pool)
        : _pPool(&pool)
    {}

    void RunFuzzing() {

        for (size_t iter = 0;; ++iter) {

            printf("Fuzzing Iter: %zu\n", iter);

            printf("Test_FixedIterator:\n");
            Test_FixedIterator();
            DevAssert(_datas.empty(), "");
            DevAssert(_set.empty(), "");

            printf("TestAndBench_Allocate_Deallocate_Iterate:\n");
            TestAndBench_Allocate_Deallocate_Iterate();
            DevAssert(_datas.empty(), "");
            DevAssert(_set.empty(), "");
        }
    }

private:

    void Test_FixedIterator() {

        {
            for (size_t i = 0; i < SIZE; ++i) {

                Data* pData = _pPool->Allocate<Data>();
            }

            size_t cnt = 0;

            KoPoolIterator<Data> iterator = _pPool->GetIterator<Data>();
            while (Data* pData = iterator.Next()) {

                cnt += pData->cnt;
                DevAssert(cnt <= SIZE, "");

                _pPool->Deallocate(pData);
                iterator = iterator.GetFixedIteratorAfterDeallocate(pData);
            }

            DevAssert(cnt == SIZE, "");
            _pPool->DeallocateBytesAll();

            printf("%zu\n", cnt);
        }

        {
            for (size_t i = 0; i < SIZE; ++i) {

                Data* pData = _pPool->Allocate<Data>();
                _datas.push_back(pData);
            }

            std::shuffle(_datas.begin(), _datas.end(), _rng);

            size_t cnt = 0;
            size_t totalCnt = SIZE;
            size_t numFixed = 0;

            KoPoolIterator<Data> iterator = _pPool->GetIterator<Data>();
            while (Data* pData = iterator.Next()) {

                cnt += pData->cnt;
                DevAssert(cnt <= SIZE, "");

                Data* pDataToRemove = _datas.back();
                _datas.pop_back();

                _set.insert(pData);

                if (_set.find(pDataToRemove) == _set.end()) {
                    totalCnt -= 1;
                }

                _pPool->Deallocate(pDataToRemove);
                iterator = iterator.GetFixedIteratorAfterDeallocate(pDataToRemove);

                if (pDataToRemove == pData || pDataToRemove == pData + sizeof(Data)) {
                    numFixed += 1;
                }
            }

            printf("%zu\n", _datas.size());

            _set.clear();
            _datas.clear();

            DevAssert(cnt == totalCnt, "");

            size_t danglingCnt = 0;

            iterator = _pPool->GetIterator<Data>();
            while (Data* pData = iterator.Next()) {

                danglingCnt += pData->cnt;

                _pPool->Deallocate(pData);
                iterator = iterator.GetFixedIteratorAfterDeallocate(pData);
            }

            DevAssert(totalCnt + danglingCnt == SIZE, "");

            _pPool->DeallocateBytesAll();

            printf("%zu\n", cnt);
            printf("Num Fixed Iterators on Deallocate: %zu\n", numFixed);
        }
    }

    void TestAndBench_Allocate_Deallocate_Iterate() {

        Bench bench{};

        size_t id = 0;
        for (size_t i = 0; i < SIZE; ++i) {

            Data* pData = nullptr;

            KoPoolIteratable::AllocBytesResult alloc{};
            bench.TimeScope(Bench::KoPoolAllocate, [&]() {

                alloc = _pPool->AllocateBytes();
                pData = reinterpret_cast<Data*>(alloc.pMemory);
            });

            DevAssert(id == _pPool->PtrToID(alloc.pMemory, alloc.subPoolID), "");
            DevAssert(_pPool->IDToPtr(id) == alloc.pMemory, "");
            DevAssert(_pPool->IDToSubPoolID(id) == alloc.subPoolID, "");
            id += 1;

            // Don't bench constructor call
            new (pData) Data{};

            bench.TimeScope(Bench::STDVectorPush, [&]() {
                _datas.push_back(pData);
            });

            bool isInserted = false;
            bench.TimeScope(Bench::UnorderedSetInsert, [&]() {
                isInserted = _set.insert(pData).second;
            });

            DevAssert(isInserted, "");
        }

        std::shuffle(_datas.begin(), _datas.end(), _rng);

        bench.TimeScope(Bench::KoPoolIterate, [&]() {

            size_t cnt = 0;
            KoPoolIterator<Data> iterator = _pPool->GetIterator<Data>();
            while (const Data* pData = iterator.Next()) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        bench.TimeScope(Bench::STDVectorIterate, [&]() {

            size_t cnt = 0;
            for (const Data* pData : _datas) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        bench.TimeScope(Bench::UnorderedSetIterate, [&]() {

            size_t cnt = 0;
            for (const Data* pData : _set) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        const size_t numToRemove = _distribution(_rng);
        for (size_t i = 0; i < numToRemove; ++i) {

            // Don't bench destructor call
            _datas.back()->~Data();

            const KoPoolIteratable::USize subPoolID = _pPool->FindSubPoolIDByPtr(_datas.back());
            const KoPoolIteratable::USize id = _pPool->PtrToID(_datas.back(), subPoolID);
            DevAssert(reinterpret_cast<uint8_t*>(_datas.back()) == _pPool->IDToPtr(id), "");
            DevAssert(subPoolID == _pPool->IDToSubPoolID(id), "");

            bench.TimeScope(Bench::KoPoolDeallocate, [&]() {

                //_pPool->DeallocateBytesByID(id);
                //_pPool->DeallocateBytesByPtrAndSubPoolID(_datas.back(), subPoolID);
                _pPool->DeallocateBytesByPtr(_datas.back());
            });

            bench.TimeScope(Bench::UnorderedSetErase, [&]() {
                _set.erase(_datas.back());
            });

            bench.TimeScope(Bench::STDVectorPop, [&]() {
                _datas.pop_back();
            });
        }

        bench.TimeScope(Bench::KoPoolIterate, [&]() {

            size_t cnt = 0;
            KoPoolIterator<Data> iterator = _pPool->GetIterator<Data>();
            while (const Data* pData = iterator.Next()) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        bench.TimeScope(Bench::STDVectorIterate, [&]() {

            size_t cnt = 0;
            for (const Data* pData : _datas) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        bench.TimeScope(Bench::UnorderedSetIterate, [&]() {

            size_t cnt = 0;
            for (const Data* pData : _set) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        for (size_t i = 0; i < SIZE; ++i) {

            Data* pData = nullptr;

            bench.TimeScope(Bench::KoPoolAllocate, [&]() {

                KoPoolIteratable::AllocBytesResult alloc = _pPool->AllocateBytes();
                pData = reinterpret_cast<Data*>(alloc.pMemory);
            });

            // Don't bench constructor call
            new (pData) Data{};

            bench.TimeScope(Bench::STDVectorPush, [&]() {
                _datas.push_back(pData);
            });

            bool isInserted = false;
            bench.TimeScope(Bench::UnorderedSetInsert, [&]() {
                isInserted = _set.insert(pData).second;
            });

            DevAssert(isInserted, "");
        }

        bench.TimeScope(Bench::KoPoolIterate, [&]() {

            size_t cnt = 0;
            KoPoolIterator<Data> iterator = _pPool->GetIterator<Data>();
            while (const Data* pData = iterator.Next()) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        bench.TimeScope(Bench::STDVectorIterate, [&]() {

            size_t cnt = 0;
            for (const Data* pData : _datas) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        bench.TimeScope(Bench::UnorderedSetIterate, [&]() {

            size_t cnt = 0;
            for (const Data* pData : _set) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        });

        for (size_t i = 0; i < _datas.size(); ++i) {

            _pPool->Deallocate(_datas[i]);
        }

        _datas.clear();
        _set.clear();

        {
            size_t cnt = 0;
            KoPoolIterator<Data> iterator = _pPool->GetIterator<Data>();
            while (const Data* pData = iterator.Next()) {
                cnt += pData->cnt;
            }

            DevAssert(cnt == _datas.size(), "");
        }

        bench.Print();
    }

private:

    class Bench {
    public:

        enum Section : size_t {

            KoPoolAllocate,
            KoPoolDeallocate,
            KoPoolIterate,

            STDVectorPush,
            STDVectorPop,
            STDVectorIterate,

            UnorderedSetInsert,
            UnorderedSetErase,
            UnorderedSetIterate,

            COUNT
        };

        template <typename Func>
        void TimeScope(const Section section, Func&& func) {

            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

            (func)();

            const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
            const std::chrono::duration<float> duration = end - start;

            _timings[section].accum += duration.count();
            _timings[section].cnt += 1;
        }

        void Print() {

            size_t maxSize = 0;
            for (const std::string_view& str : SECTION_TO_STR) {
                maxSize = std::max(str.size(), maxSize);
            }

            for (size_t i = 0; i < Section::COUNT; ++i) {

                printf("%s: ", SECTION_TO_STR[i].data());

                for (size_t j = 0; SECTION_TO_STR[i].size() + j < maxSize; ++j) {
                    printf(" ");
                }

                printf("%fms\n", _timings[i].accum / (double)_timings[i].cnt * 1'000);
            }

            printf("--------------------------\n");
        }

    public:

        static constexpr std::array<std::string_view, Section::COUNT> SECTION_TO_STR{

            "[KoPool] Allocate",
            "[KoPool] Deallocate",
            "[KoPool] Iterate",

            "[STDVector] Push",
            "[STDVector] Pop",
            "[STDVector] Iterate",

            "[UnorderedSet] Insert",
            "[UnorderedSet] Erase",
            "[UnorderedSet] Iterate",
        };

        struct Time {
            double accum = 0.0;
            size_t cnt = 0;
        };

    private:

        std::array<Time, Section::COUNT> _timings{};
    };

private:

    //using UnorderedSet = std::unordered_set<Data*>;
    using UnorderedSet = ankerl::unordered_dense::set<Data*>;

    static constexpr size_t SIZE = 1'000'000;

    std::mt19937_64 _rng{ std::random_device{}() };
    std::uniform_int_distribution<uint64_t> _distribution{ 0, SIZE };

    UnorderedSet _set;
    std::vector<Data*> _datas;

    KoPoolIteratable* _pPool = nullptr;
};

int main() {

    KoPoolIteratable::Opt opt{};
    opt.elementAlignment = alignof(TestCtx::Data);
    opt.elementSizeInBytes = sizeof(TestCtx::Data);

    KoPoolIteratable pool{ opt };

    TestCtx test{ pool };
    test.RunFuzzing();

    return 1;
}
