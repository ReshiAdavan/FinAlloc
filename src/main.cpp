// main.cpp
#include <iostream>
#include "allocators/poolAllocator.hpp"
#include "allocators/sizeClassPool.hpp"

struct TestObject {
    int a;
    double b;
    TestObject(int x, double y) : a(x), b(y) {
        std::cout << "Constructed TestObject(" << a << ", " << b << ")\n";
    }
    ~TestObject() {
        std::cout << "Destroyed TestObject(" << a << ", " << b << ")\n";
    }
};

int main() {
    std::cout << "--- Basic PoolAllocator Test ---\n";
    PoolAllocator pool(sizeof(TestObject), 10);

    void* mem = pool.allocate();
    auto* obj = new (mem) TestObject(1, 3.14);
    obj->~TestObject();
    pool.deallocate(mem);

    std::cout << "--- construct<T>/destroy<T> Test ---\n";
    TestObject* obj2 = pool.construct<TestObject>(42, 6.28);
    pool.destroy(obj2);

    std::cout << "--- SizeClassPool Test ---\n";
    SizeClassPool<> sizePool(128);
    TestObject* obj3 = sizePool.construct<TestObject>(100, 99.99);
    sizePool.destroy(obj3);

    return 0;
}