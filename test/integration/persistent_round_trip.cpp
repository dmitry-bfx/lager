// lager - library for functional interactive C++ programs
// Copyright (C) 2026 OpenAI
//
// This file is part of lager and is distributed under the terms of the MIT
// License. See the LICENSE file at the root of this source code distribution.

#include <lager/extra/persistent_state.hpp>
#include <lager/extra/struct.hpp>
#include <lager/garbage_collector.hpp>
#include <lager/mapped_storage.hpp>

#include <immer/vector.hpp>
#include <immer/vector_transient.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace persistent_round_trip {

using values_type = immer::vector<int, immer::storage_memory_policy>;

struct root_state
{
    values_type values;
};

} // namespace persistent_round_trip

LAGER_STRUCT(persistent_round_trip, root_state, values);

namespace {

using namespace persistent_round_trip;
constexpr std::uint64_t store_id = 0x4c41474552505431ULL;
constexpr std::uint64_t dependency_store_id = store_id + 1;
constexpr std::size_t value_count = 100000;
constexpr std::size_t changed_index = 5137;
constexpr int changed_value = -5137;
constexpr std::size_t slot_bytes = 1024;
constexpr std::size_t slot_alignment = 16;
constexpr std::uint64_t extent_layout_id = 0x455854454e543031ULL;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error{message};
}

std::string metadata_path(const std::string& path)
{
    return path + ".mapping";
}

#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
std::pair<void*, std::size_t>& reserved_mapping();
#endif

void write_mapping_metadata(const std::string& path,
                            const lager::storage_extent& extent)
{
#if !defined(_WIN32)
    const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    const auto mapping_begin =
        reinterpret_cast<std::uintptr_t>(extent.base) - page;
    const auto mapping_bytes = page + extent.size_bytes;
    std::ofstream metadata{metadata_path(path), std::ios::trunc};
    require(static_cast<bool>(metadata), "cannot write mapping metadata");
    metadata << mapping_begin << ' ' << mapping_bytes << '\n';
#else
    (void) path;
    (void) extent;
    throw std::runtime_error{"persistent round trip requires POSIX mmap"};
#endif
}

void reserve_writer_address(const std::string& path)
{
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    std::uintptr_t address = 0;
    std::size_t bytes = 0;
    std::ifstream metadata{metadata_path(path)};
    require(static_cast<bool>(metadata), "mapping metadata is missing");
    metadata >> address >> bytes;
    require(address != 0 && bytes != 0, "mapping metadata is invalid");
    auto* reservation = ::mmap(reinterpret_cast<void*>(address),
                               bytes,
                               PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS |
                                   MAP_FIXED_NOREPLACE,
                               -1,
                               0);
    require(reservation != MAP_FAILED,
            "could not reserve the writer's mapping address");
    // Keep the reservation until all extents have been mapped by this reader.
    reserved_mapping() = {reservation, bytes};
#else
    (void) path;
    throw std::runtime_error{
        "this platform does not provide MAP_FIXED_NOREPLACE"};
#endif
}

#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
std::pair<void*, std::size_t>& reserved_mapping()
{
    static std::pair<void*, std::size_t> value{MAP_FAILED, 0};
    return value;
}

void release_reserved_address()
{
    auto& reservation = reserved_mapping();
    if (reservation.first != MAP_FAILED) {
        ::munmap(reservation.first, reservation.second);
        reservation = {MAP_FAILED, 0};
    }
}
#endif

std::vector<lager::mapped_extent> map_existing(
    const std::string& path,
    lager::persistent_storage_file& file,
    lager::shared_store_runtime& runtime,
    bool force_relocation)
{
    if (force_relocation)
        reserve_writer_address(path);
    auto mappings = file.map_existing_extents();
    require(!mappings.empty(), "persistent store contains no extents");
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    if (force_relocation) {
        const auto mapped_data = reinterpret_cast<std::uintptr_t>(
            mappings.front().view().base);
        require(mapped_data != reinterpret_cast<std::uintptr_t>(
                                   reserved_mapping().first) +
                                   static_cast<std::size_t>(
                                       ::sysconf(_SC_PAGESIZE)),
                "reader reused the writer's virtual mapping address");
    }
#endif
    return mappings;
}

struct traced_nodes
{
    std::set<std::uint64_t> bits;

    template <typename T>
    void visit_node(immer::storage_ptr<T> handle)
    {
        bits.insert(handle.bits());
    }

    template <typename T>
    void visit_auxiliary(immer::storage_ptr<T> handle)
    {
        bits.insert(handle.bits());
    }

    template <typename T>
    void visit_value(const T&)
    {}
};

void create_store(const std::string& path)
{
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".roots");
    std::filesystem::remove(metadata_path(path));

    auto file = lager::persistent_storage_file::open(path);
    auto mapping = file.append_extent(slot_bytes,
                                      slot_alignment,
                                      extent_layout_id,
                                      static_cast<std::uint32_t>(
                                          value_count + 40000));
    lager::shared_store_runtime runtime;
    runtime.publish_extent(mapping.view());
    write_mapping_metadata(path, mapping.view());

    {
        auto storage = runtime.activate_storage();
        auto builder = values_type{}.transient();
        for (std::size_t i = 0; i < value_count; ++i)
            builder.push_back(static_cast<int>(i));
        auto values = std::move(builder).persistent();
        const auto persisted_root = lager::persistent_layout(
            root_state{values}, 1);
        auto root = immer::make_storage<root_state>(root_state{values});
        auto journal = file.roots();
        auto store = lager::create_persistent_substore(
            runtime, {store_id}, root, journal);
        auto dependency_values = values_type{}.push_back(8675309);
        auto dependency_root = immer::make_storage<root_state>(
            root_state{std::move(dependency_values)});
        (void) lager::create_persistent_substore(
            runtime, {dependency_store_id}, dependency_root, journal);
        const auto snapshot = runtime.read(store);
        snapshot.with_root<root_state>(
            persisted_root.fingerprint,
            [](const root_state& saved) {
                require(saved.values.size() == value_count,
                        "writer root has the wrong element count");
                require(saved.values[0] == 0 &&
                            saved.values[value_count - 1] ==
                                static_cast<int>(value_count - 1),
                        "writer indexed reads returned incorrect values");
            });
    }
    std::cout << "writer committed " << value_count
              << " values and exited with a populated persistent root\n";
}

void read_store(const std::string& path, bool updated)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = map_existing(path, file, runtime, true);
    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "persistent root was not recovered");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    const auto snapshot = runtime.read(store);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    snapshot.with_root<root_state>(layout.fingerprint, [&](const auto& root) {
        require(root.values.size() == value_count,
                "reader found the wrong number of values");
        for (std::size_t i = 0; i < value_count; ++i) {
            const auto expected = updated && i == changed_index
                                      ? changed_value
                                      : static_cast<int>(i);
            require(root.values[i] == expected,
                    "reader found an incorrect value at index " +
                        std::to_string(i));
        }
    });
    std::cout << "separate reader recovered generation "
              << snapshot.generation() << " at relocated address "
              << static_cast<const void*>(mappings.front().view().base)
              << " and verified all values"
              << (updated ? " after update" : " before update") << '\n';
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    release_reserved_address();
#endif
}

void update_store(const std::string& path)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = map_existing(path, file, runtime, true);
    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "persistent root was not recovered");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    auto transaction = runtime.begin_transaction(store);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    const auto new_generation = lager::update_persistent_root<root_state>(
        transaction,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(changed_index, changed_value);
            return updated;
        },
        journal);
    require(new_generation > found->generation,
            "persistent update did not advance generation");
    const auto updated_snapshot = runtime.read(store);
    std::set<std::uint64_t> old_nodes;
    std::set<std::uint64_t> new_nodes;
    transaction.base().with_root<root_state>(layout.fingerprint,
        [&](const auto& old_root) {
            require(old_root.values[changed_index] ==
                        static_cast<int>(changed_index),
                    "pinned old snapshot changed after commit");
            traced_nodes visitor;
            old_root.values.trace_children(visitor);
            old_nodes = std::move(visitor.bits);
        });
    updated_snapshot.with_root<root_state>(layout.fingerprint,
        [&](const auto& new_root) {
            require(new_root.values[changed_index] == changed_value,
                    "updated root does not contain the new value");
            traced_nodes visitor;
            new_root.values.trace_children(visitor);
            new_nodes = std::move(visitor.bits);
        });
    std::vector<std::uint64_t> shared_nodes;
    std::set_intersection(old_nodes.begin(),
                          old_nodes.end(),
                          new_nodes.begin(),
                          new_nodes.end(),
                          std::back_inserter(shared_nodes));
    require(!shared_nodes.empty(),
            "persistent vector update did not preserve shared nodes");
    std::cout << "updated generation " << new_generation << "; "
              << shared_nodes.size()
              << " Immer nodes remain structurally shared\n";
#if !defined(_WIN32) && defined(MAP_FIXED_NOREPLACE)
    release_reserved_address();
#endif
}

#if !defined(_WIN32)
int shared_ephemeral_writer_child(const std::string& name,
                                  std::uint64_t store_id,
                                  std::uint64_t layout_id,
                                  int value)
{
    auto shared = lager::shared_ephemeral_storage::open(
        name, 73, slot_bytes, slot_alignment, extent_layout_id);
    lager::shared_store_runtime runtime;
    runtime.publish_extent(shared.view());
    auto store = runtime.attach_shared_ephemeral_root(
        {store_id}, layout_id, shared);
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    require(lager::persistent_layout(layout_sample, 1).fingerprint ==
                layout_id,
            "shared ephemeral layout did not match the attached root");
    auto transaction = runtime.begin_transaction(store);
    auto candidate = transaction.base().with_root<root_state>(
        layout_id,
        [value](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, value);
            return updated;
        });
    auto handle = immer::make_storage<root_state>(std::move(candidate));
    (void) transaction.commit(handle.template rebind<void>());
    return 0;
}

void shared_ephemeral_process_round_trip()
{
    const auto name = "/lager_ephemeral_" + std::to_string(::getpid());
    constexpr std::uint64_t id = 0x4c41474552455031ULL;
    auto shared = lager::shared_ephemeral_storage::create(
        name, 73, slot_bytes, slot_alignment, extent_layout_id, 4096);
    lager::shared_store_runtime runtime;
    runtime.publish_extent(shared.view());
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout_id = lager::persistent_layout(layout_sample, 1).fingerprint;
    auto initial_values = values_type{}.push_back(1);
    auto initial = immer::make_storage<root_state>(
        root_state{std::move(initial_values)});
    auto store = runtime.create_shared_ephemeral_root(
        {id}, initial.template rebind<void>(), layout_id, shared);
    auto dependency_values = values_type{}.push_back(2);
    auto dependency_root = immer::make_storage<root_state>(
        root_state{std::move(dependency_values)});
    auto dependency = runtime.create_shared_ephemeral_root(
        {id + 1}, dependency_root.template rebind<void>(), layout_id, shared);

    auto stale = runtime.begin_transaction(store);
    auto stale_candidate = stale.base().with_root<root_state>(
        layout_id,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 99);
            return updated;
        });
    auto stale_handle = immer::make_storage<root_state>(
        std::move(stale_candidate));

    const auto child = ::fork();
    require(child >= 0, "fork failed for shared ephemeral store test");
    if (child == 0) {
        try {
            ::_exit(shared_ephemeral_writer_child(name, id, layout_id, 42));
        } catch (...) {
            ::_exit(3);
        }
    }
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "separate shared ephemeral writer failed");
    bool conflicted = false;
    try {
        (void) stale.commit(stale_handle.template rebind<void>());
    } catch (const lager::shared_write_conflict&) {
        conflicted = true;
    }
    require(conflicted,
            "shared ephemeral writer accepted a stale process-local root");
    auto latest = runtime.read(store);
    latest.with_root<root_state>(layout_id, [](const auto& root) {
        require(root.values[0] == 42,
                "shared ephemeral read did not observe the other process commit");
    });

    auto stale_dependency = runtime.begin_transaction(store);
    (void) stale_dependency.read(dependency);
    auto stale_dependency_candidate = stale_dependency.base().with_root<root_state>(
        layout_id,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 99);
            return updated;
        });
    auto stale_dependency_handle = immer::make_storage<root_state>(
        std::move(stale_dependency_candidate));
    const auto dependency_child = ::fork();
    require(dependency_child >= 0,
            "fork failed for shared ephemeral dependency test");
    if (dependency_child == 0) {
        try {
            ::_exit(shared_ephemeral_writer_child(
                name, id + 1, layout_id, 24));
        } catch (...) {
            ::_exit(3);
        }
    }
    status = 0;
    require(::waitpid(dependency_child, &status, 0) == dependency_child &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "shared ephemeral dependency writer failed");
    bool dependency_conflicted = false;
    try {
        (void) stale_dependency.commit(
            stale_dependency_handle.template rebind<void>());
    } catch (const lager::shared_write_conflict&) {
        dependency_conflicted = true;
    }
    require(dependency_conflicted,
            "shared ephemeral exact dependency accepted a stale generation");
    latest = runtime.read(store);
    latest.with_root<root_state>(layout_id, [](const auto& root) {
        require(root.values[0] == 42,
                "failed dependency commit changed the target root");
    });
    std::cout << "shared ephemeral reader observed a process-shared commit and "
                 "rejected stale root and dependency candidates\n";
}

void crash_update_child(const std::string& path, const std::string& stage)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings) {
        auto extent = mapping.view();
        if (stage == "data-flush") {
            const auto flush = extent.flush_data;
            extent.flush_data = [flush] {
                flush();
                ::kill(::getpid(), SIGKILL);
                ::_exit(137);
            };
        }
        runtime.publish_extent(std::move(extent));
    }

    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "crash writer could not recover root");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    auto transaction = runtime.begin_transaction(store);
    if (stage == "allocation") {
        const auto layout = lager::persistent_layout(layout_sample, 1);
        auto candidate = transaction.base().with_root<root_state>(
            layout.fingerprint,
            [](const root_state& old_root) {
                auto updated = old_root;
                updated.values =
                    updated.values.set(changed_index, changed_value);
                return updated;
            });
        (void) immer::make_storage<root_state>(std::move(candidate));
        ::kill(::getpid(), SIGKILL);
        ::_exit(137);
    }
    if (stage == "root-record-write" || stage == "publication")
        ::setenv("LAGER_PERSISTENT_FAILPOINT", stage.c_str(), 1);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    (void) lager::update_persistent_root<root_state>(
        transaction,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(changed_index, changed_value);
            return updated;
        },
        journal);
    ::_exit(0);
}

void crash_case(const std::string& path, const std::string& stage)
{
    create_store(path);
    const auto child = ::fork();
    require(child >= 0, "fork failed for crash-boundary test");
    if (child == 0)
        crash_update_child(path, stage);
    int status = 0;
    require(::waitpid(child, &status, 0) == child,
            "waitpid failed for crash-boundary test");
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "crash writer did not terminate at requested boundary");

    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [](const auto& root) {
                                        return root.store_id == store_id;
                                    });
    require(found != roots.end(), "recovery lost every committed root");
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto store = lager::restore_persistent_substore(
        runtime, *found, layout_sample, journal);
    const auto snapshot = runtime.read(store);
    const auto layout = lager::persistent_layout(layout_sample, 1);
    const bool committed = stage == "publication";
    snapshot.with_root<root_state>(layout.fingerprint, [&](const auto& root) {
        require(root.values.size() == value_count,
                "crash recovery exposed an incomplete vector");
        require(root.values[changed_index] ==
                    (committed ? changed_value
                               : static_cast<int>(changed_index)),
                "crash recovery did not select an atomic root version");
    });
    std::cout << "termination at " << stage << " recovered the "
              << (committed ? "new" : "previous") << " complete root\n";
}

const lager::durable_root_descriptor& root_for(
    const std::vector<lager::durable_root_descriptor>& roots,
    std::uint64_t id)
{
    const auto found = std::find_if(roots.begin(),
                                    roots.end(),
                                    [id](const auto& root) {
                                        return root.store_id == id;
                                    });
    if (found == roots.end())
        throw std::runtime_error{"requested persistent substore was not recovered"};
    return *found;
}

void transaction_semantics(const std::string& path)
{
    create_store(path);
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    auto recovered = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto first = lager::restore_persistent_substore(
        runtime, root_for(recovered, store_id), layout_sample, journal);
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(recovered, dependency_store_id),
        layout_sample,
        journal);

    auto reads_dependency = runtime.begin_transaction(first);
    const auto dependency_snapshot = reads_dependency.read(dependency);
    const auto dependency_generation = dependency_snapshot.generation();
    (void) lager::update_persistent_root<root_state>(
        reads_dependency,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, -1);
            return updated;
        },
        journal);
    recovered = journal.recover();
    const auto& committed = root_for(recovered, store_id);
    require(committed.read_watermark == dependency_generation,
            "commit did not persist the maximum read generation");
    require(committed.generation > committed.read_watermark,
            "commit generation did not advance beyond its read watermark");

    auto stale_reader = runtime.begin_transaction(first);
    (void) stale_reader.read(dependency);
    auto dependency_writer = runtime.begin_transaction(dependency);
    (void) lager::update_persistent_root<root_state>(
        dependency_writer,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 8675310);
            return updated;
        },
        journal);
    bool stale_rejected = false;
    try {
        (void) lager::update_persistent_root<root_state>(
            stale_reader,
            layout_sample,
            [](const root_state& old_root) {
                auto updated = old_root;
                updated.values = updated.values.set(1, -2);
                return updated;
            },
            journal);
    } catch (const lager::shared_write_conflict&) {
        stale_rejected = true;
    }
    require(stale_rejected,
            "exact cross-domain dependency accepted a stale generation");

    lager::shared_transaction_scope single_writer;
    single_writer.record_write({store_id});
    bool second_write_rejected = false;
    try {
        single_writer.record_write({dependency_store_id});
    } catch (const std::logic_error&) {
        second_write_rejected = true;
    }
    require(second_write_rejected,
            "transaction scope accepted two write domains");
    std::cout << "transaction tests confirmed single-writer domains, exact "
                 "cross-domain validation, and durable read watermarks\n";
}

void race_writer_child(const std::string& path,
                       std::size_t index,
                       int ready_fd,
                       int release_fd)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto store = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    auto transaction = runtime.begin_transaction(store);
    const auto candidate = transaction.base().with_root<root_state>(
        layout.fingerprint,
        [index](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(index, -static_cast<int>(index));
            return updated;
        });
    auto candidate_handle = immer::make_storage<root_state>(candidate);
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1)
        ::_exit(4);
    char release = 0;
    if (::read(release_fd, &release, 1) != 1)
        ::_exit(5);
    try {
        (void) transaction.commit_persistent_root(
            candidate_handle.template rebind<void>(),
            layout.fingerprint,
            journal);
        ::_exit(0);
    } catch (const lager::shared_write_conflict&) {
        ::_exit(2);
    } catch (...) {
        ::_exit(3);
    }
}

void competing_writers(const std::string& path)
{
    create_store(path);
    int ready_pipe[2];
    int release_pipe[2];
    require(::pipe(ready_pipe) == 0 && ::pipe(release_pipe) == 0,
            "could not create writer synchronization pipes");
    const std::size_t indexes[] = {101, 202};
    pid_t children[2] = {-1, -1};
    for (std::size_t i = 0; i < 2; ++i) {
        children[i] = ::fork();
        require(children[i] >= 0, "fork failed for competing writers");
        if (children[i] == 0) {
            ::close(ready_pipe[0]);
            ::close(release_pipe[1]);
            race_writer_child(path,
                              indexes[i],
                              ready_pipe[1],
                              release_pipe[0]);
        }
    }
    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);
    char ready = 0;
    require(::read(ready_pipe[0], &ready, 1) == 1 &&
                ::read(ready_pipe[0], &ready, 1) == 1,
            "competing writers did not prepare both candidates");
    const char release[2] = {'g', 'g'};
    require(::write(release_pipe[1], release, sizeof(release)) == 2,
            "could not release competing writers");
    int statuses[2]{};
    for (std::size_t i = 0; i < 2; ++i)
        require(::waitpid(children[i], &statuses[i], 0) == children[i],
                "waitpid failed for competing writers");
    const auto success_count =
        static_cast<int>(WIFEXITED(statuses[0]) && WEXITSTATUS(statuses[0]) == 0) +
        static_cast<int>(WIFEXITED(statuses[1]) && WEXITSTATUS(statuses[1]) == 0);
    const auto conflict_count =
        static_cast<int>(WIFEXITED(statuses[0]) && WEXITSTATUS(statuses[0]) == 2) +
        static_cast<int>(WIFEXITED(statuses[1]) && WEXITSTATUS(statuses[1]) == 2);
    require(success_count == 1 && conflict_count == 1,
            "competing writers did not produce one commit and one conflict");
    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);

    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto store = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    runtime.read(store).with_root<root_state>(layout.fingerprint,
        [](const auto& root) {
            const bool first_won = root.values[101] == -101;
            const bool second_won = root.values[202] == -202;
            require(first_won != second_won,
                    "stale candidate lost a committed update");
        });
    std::cout << "competing processes produced one durable commit and one "
                 "stale-candidate conflict\n";
}

void cross_dependency_writer_child(const std::string& path,
                                   int ready_fd,
                                   int release_fd)
{
    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    const auto layout = lager::persistent_layout(layout_sample, 1);
    auto first = lager::restore_persistent_substore(
        runtime, root_for(roots, store_id), layout_sample, journal);
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(roots, dependency_store_id),
        layout_sample,
        journal);
    auto transaction = runtime.begin_transaction(first);
    (void) transaction.read(dependency);
    const auto candidate = transaction.base().with_root<root_state>(
        layout.fingerprint,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, -10);
            return updated;
        });
    auto candidate_handle = immer::make_storage<root_state>(candidate);
    const char ready = 'r';
    if (::write(ready_fd, &ready, 1) != 1)
        ::_exit(4);
    char release = 0;
    if (::read(release_fd, &release, 1) != 1)
        ::_exit(5);
    try {
        (void) transaction.commit_persistent_root(
            candidate_handle.template rebind<void>(),
            layout.fingerprint,
            journal);
        ::_exit(0);
    } catch (const lager::shared_write_conflict&) {
        ::_exit(2);
    } catch (...) {
        ::_exit(3);
    }
}

void cross_process_dependency(const std::string& path)
{
    create_store(path);
    int ready_pipe[2];
    int release_pipe[2];
    require(::pipe(ready_pipe) == 0 && ::pipe(release_pipe) == 0,
            "could not create dependency synchronization pipes");
    const auto child = ::fork();
    require(child >= 0, "fork failed for cross-domain dependency test");
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(release_pipe[1]);
        cross_dependency_writer_child(
            path, ready_pipe[1], release_pipe[0]);
    }
    ::close(ready_pipe[1]);
    ::close(release_pipe[0]);
    char ready = 0;
    require(::read(ready_pipe[0], &ready, 1) == 1,
            "cross-domain transaction did not prepare its candidate");

    auto file = lager::persistent_storage_file::open(path);
    lager::shared_store_runtime runtime;
    auto mappings = file.map_existing_extents();
    for (const auto& mapping : mappings)
        runtime.publish_extent(mapping.view());
    auto journal = file.roots();
    const auto roots = journal.recover();
    auto storage = runtime.activate_storage();
    const root_state layout_sample{};
    auto dependency = lager::restore_persistent_substore(
        runtime,
        root_for(roots, dependency_store_id),
        layout_sample,
        journal);
    const auto before_remote_commit = runtime.read(dependency).generation();
    auto transaction = runtime.begin_transaction(dependency);
    (void) lager::update_persistent_root<root_state>(
        transaction,
        layout_sample,
        [](const root_state& old_root) {
            auto updated = old_root;
            updated.values = updated.values.set(0, 8675311);
            return updated;
        },
        journal);
    const auto remotely_updated = runtime.read(dependency);
    require(remotely_updated.generation() > before_remote_commit,
            "persistent read did not acquire the other process root");
    remotely_updated.with_root<root_state>(
        lager::persistent_layout(layout_sample, 1).fingerprint,
        [](const auto& root) {
            require(root.values[0] == 8675311,
                    "persistent read saw a stale process-local root");
        });
    const char release = 'g';
    require(::write(release_pipe[1], &release, 1) == 1,
            "could not release cross-domain transaction");
    int status = 0;
    require(::waitpid(child, &status, 0) == child,
            "waitpid failed for cross-domain transaction");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 2,
            "cross-process exact dependency did not reject stale commit");
    ::close(ready_pipe[0]);
    ::close(release_pipe[1]);

    const auto latest = journal.recover();
    auto first = lager::restore_persistent_substore(
        runtime, root_for(latest, store_id), layout_sample, journal);
    runtime.read(first).with_root<root_state>(
        lager::persistent_layout(layout_sample, 1).fingerprint,
        [](const auto& root) {
            require(root.values[0] == 0,
                    "stale cross-domain writer changed its target root");
        });
    std::cout << "exact cross-domain reads rejected a stale process-local "
                 "candidate through the durable journal\n";
}
#endif

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 2)
            throw std::invalid_argument{"persistent round-trip mode is required"};
        const std::string mode{argv[1]};
        if (mode == "ephemeral" && argc == 2) {
#if !defined(_WIN32)
            shared_ephemeral_process_round_trip();
            return 0;
#else
            throw std::runtime_error{"shared ephemeral test requires POSIX"};
#endif
        }
        if (argc != 3 && argc != 4)
            throw std::invalid_argument{
                "usage: persistent-round-trip write|update|read STORE [initial|updated]"};
        const std::string path{argv[2]};
        if (mode == "write" && argc == 3)
            create_store(path);
        else if (mode == "update" && argc == 3)
            update_store(path);
#if !defined(_WIN32)
        else if (mode == "ephemeral" && argc == 3)
            shared_ephemeral_process_round_trip();
        else if (mode == "crash" && argc == 4)
            crash_case(path, argv[3]);
        else if (mode == "transactions" && argc == 3)
            transaction_semantics(path);
        else if (mode == "race" && argc == 3)
            competing_writers(path);
        else if (mode == "cross-dependency" && argc == 3)
            cross_process_dependency(path);
#endif
        else if (mode == "read" && argc == 4) {
            const std::string stage{argv[3]};
            if (stage != "initial" && stage != "updated")
                throw std::invalid_argument{"reader stage must be initial or updated"};
            read_store(path, stage == "updated");
        } else
            throw std::invalid_argument{"invalid persistent round-trip command"};
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "persistent round trip failed: " << error.what() << '\n';
        return 1;
    }
}
