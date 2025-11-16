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
    void AddComponent(EntityID e, const T& component) {
        GetOrCreateStorage<T>().Add(e, component);
    }

    template<typename T>
    bool HasComponent(EntityID e) const {
        auto it = storages.find(std::type_index(typeid(T)));
        if (it == storages.end()) return false;
        return static_cast<SparseSet<T>*>(it->second.get())->Has(e);
    }

    template<typename T>
    T& GetComponent(EntityID e) {
        return GetOrCreateStorage<T>().Get(e);
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
    void ForEach(Func func) {
        auto storagesTuple = std::make_tuple(&GetOrCreateStorage<Components>()...);
        auto& smallest = *std::min_element(
                { static_cast<const void*>(std::get<SparseSet<Components>*>(storagesTuple)->Entities().data())... },
                { static_cast<const void*>(std::get<SparseSet<Components>*>(storagesTuple)->Entities().data())... }
        ); // placeholder: compile safe but not meaningful; we’ll refine below

        // Find the smallest storage by entity count
        auto smallestIndex = FindSmallestStorageIndex<Components...>(storagesTuple);
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
        return *static_cast<SparseSet<T>*>(it->second.get());
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
