#pragma once
#include <unordered_map>
#include <memory>
#include <typeindex>
#include <tuple>
#include <algorithm>

#include "Entity.h"
#include "ComponentStorage.h"

class ECS {
public:
    EntityID CreateEntity() {
        EntityID id = nextID++;
        return id;
    }

    void DestroyEntity(EntityID e) {
        for (auto& [_, storage] : storages)
            storage->RemoveEntity(e);
    }

    template<typename T>
    void AddComponent(EntityID e, T component) {
        GetOrCreateStorage<T>().Add(e, std::move(component));
    }

    template<typename T>
    bool HasComponent(EntityID e) const {
        auto it = storages.find(std::type_index(typeid(T)));
        if (it == storages.end()) return false;
        return dynamic_cast<SparseSet<T>*>(it->second.get())->Has(e);
    }

    template<typename T>
    T& GetComponent(EntityID e) {
        return GetOrCreateStorage<T>().Get(e);
    }

    template<typename T>
    T* TryGetComponent(EntityID e) {
        return HasComponent<T>(e) ? &GetComponent<T>(e) : nullptr;
    }

    // Single-component iteration
    template<typename T, typename Func>
    void ForEach(Func func) {
        auto& storage = GetOrCreateStorage<T>();
        auto& entities = storage.Entities();
        auto& data = storage.Data();
        for (size_t i = 0; i < entities.size(); ++i)
            func(entities[i], data[i]);
    }

    // Multi-component iteration
    template<typename... Components, typename Func>
    void ForEachMulti(Func func) {
        // Create a tuple of pointers to the component storages.
        auto storagesTuple = std::make_tuple(&GetOrCreateStorage<Components>()...);

        // Find the index of the storage with the fewest entities. This is the correct way
        // to find the smallest set to optimize the iteration.
        auto smallestIndex = FindSmallestStorageIndex<Components...>(storagesTuple);

        // Invoke the implementation function with the smallest storage index.
        ForEachMultiImpl<0, Components...>(func, storagesTuple, smallestIndex);
    }

private:
    struct IStorage {
        virtual ~IStorage() = default;
        virtual void RemoveEntity(EntityID) = 0;
    };

    template<typename T>
    struct StorageImpl : IStorage, SparseSet<T> {
        void RemoveEntity(EntityID e) override { this->Remove(e); }
    };

    template<typename T>
    SparseSet<T>& GetOrCreateStorage() const {
        auto it = storages.find(std::type_index(typeid(T)));
        if (it == storages.end()) {
            auto storage = std::make_unique<StorageImpl<T>>();
            auto* ptr = storage.get();
            const_cast<ECS*>(this)->storages.emplace(std::type_index(typeid(T)), std::move(storage));
            return *ptr;
        }

        // Use dynamic_cast because IStorage and SparseSet<T> are sibling base classes.
        auto* ptr = dynamic_cast<SparseSet<T>*>(it->second.get());
        // It's good practice to check if the cast succeeded.
        // In this logic, it should never fail, but this prevents crashes.
        if (!ptr) {
            // This would indicate a serious logic error in the ECS.
            // For now, we'll throw an exception.
            throw std::runtime_error("dynamic_cast failed in GetOrCreateStorage");
        }
        return *ptr;
    }

    // --- Multi-component helpers ---
    template<typename... Ts>
    static size_t FindSmallestStorageIndex(const std::tuple<SparseSet<Ts>*...>& storages) {
        size_t sizes[] = { std::get<SparseSet<Ts>*>(storages)->Entities().size()... };
        size_t minIdx = 0;
        for (size_t i = 1; i < sizeof...(Ts); ++i)
            if (sizes[i] < sizes[minIdx]) minIdx = i;
        return minIdx;
    }

    template<size_t Index, typename... Ts, typename Func>
    void ForEachMultiImpl(Func& func, std::tuple<SparseSet<Ts>*...>& storages, size_t smallestIndex) {
        ApplyForEachMulti<Index, Ts...>(func, storages, smallestIndex, std::index_sequence_for<Ts...>{});
    }

    template<size_t Index, typename... Ts, typename Func, size_t... I>
    void ApplyForEachMulti(Func& func, std::tuple<SparseSet<Ts>*...>& storages, size_t smallestIndex, std::index_sequence<I...>) {
        auto& smallestSet = *std::get<Index>(storages);
        for (EntityID e : smallestSet.Entities()) {
            if ((std::get<SparseSet<Ts>*>(storages)->Has(e) && ...)) {
                func(e, std::get<SparseSet<Ts>*>(storages)->Get(e)...);
            }
        }
    }

    mutable std::unordered_map<std::type_index, std::unique_ptr<IStorage>> storages;
    EntityID nextID = 1;
};
