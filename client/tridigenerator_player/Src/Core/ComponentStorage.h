#pragma once
#include <vector>
#include <unordered_map>
#include <cassert>

// Required for UINT32_MAX
#include <cstdint>

// Required for the using-declaration for std::swap
#include <utility>

// Forward declaration of EntityID
#include "Entity.h"

// Sparse–dense component storage
template<typename T>
class SparseSet {
public:
    // Overload Add for movable types.
    void Add(EntityID e, T&& component) { // Note the '&&'
        if (Has(e)) return;
        if (e >= sparse.size()) sparse.resize(e + 1, INVALID_INDEX);

        sparse[e] = static_cast<uint32_t>(dense.size());
        dense.push_back(e);
        data.push_back(std::move(component)); // FIX: Use std::move
    }

    // Add method for copyable types (optional but good practice)
    void Add(EntityID e, const T& component) {
        if (Has(e)) return;
        if (e >= sparse.size()) sparse.resize(e + 1, INVALID_INDEX);

        sparse[e] = static_cast<uint32_t>(dense.size());
        dense.push_back(e);
        data.push_back(component);
    }

    void Remove(EntityID e) {
        assert(Has(e));
        uint32_t denseIndex = sparse[e];
        uint32_t lastIndex = static_cast<uint32_t>(dense.size() - 1);
        EntityID lastEntity = dense[lastIndex];

        using std::swap;
        swap(dense[denseIndex], dense[lastIndex]);
        swap(data[denseIndex], data[lastIndex]);

        sparse[lastEntity] = denseIndex;

        dense.pop_back();
        data.pop_back();
        sparse[e] = INVALID_INDEX;
    }

    bool Has(EntityID e) const {
        return e < sparse.size() && sparse[e] != INVALID_INDEX;
    }

    T& Get(EntityID e) {
        assert(Has(e));
        return data[sparse[e]];
    }

    const std::vector<EntityID>& Entities() const { return dense; }
    std::vector<T>& Data() { return data; }

private:
    static const uint32_t INVALID_INDEX = UINT32_MAX;
    std::vector<uint32_t> sparse;
    std::vector<EntityID> dense;
    std::vector<T> data;
};

template<typename T>
const uint32_t SparseSet<T>::INVALID_INDEX;
