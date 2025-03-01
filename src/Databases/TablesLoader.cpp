#include <Databases/TablesLoader.h>
#include <Databases/IDatabase.h>
#include <Databases/DDLDependencyVisitor.h>
#include <Databases/DDLLoadingDependencyVisitor.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExternalDictionariesLoader.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/logger_useful.h>
#include <Common/ThreadPool.h>
#include <numeric>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

static constexpr size_t PRINT_MESSAGE_EACH_N_OBJECTS = 256;
static constexpr size_t PRINT_MESSAGE_EACH_N_SECONDS = 5;

void logAboutProgress(Poco::Logger * log, size_t processed, size_t total, AtomicStopwatch & watch)
{
    if (processed % PRINT_MESSAGE_EACH_N_OBJECTS == 0 || watch.compareAndRestart(PRINT_MESSAGE_EACH_N_SECONDS))
    {
        LOG_INFO(log, "{}%", processed * 100.0 / total);
        watch.restart();
    }
}

TablesLoader::TablesLoader(ContextMutablePtr global_context_, Databases databases_, LoadingStrictnessLevel strictness_mode_)
: global_context(global_context_)
, databases(std::move(databases_))
, strictness_mode(strictness_mode_)
, referential_dependencies("ReferentialDeps")
, loading_dependencies("LoadingDeps")
{
    metadata.default_database = global_context->getCurrentDatabase();
    log = &Poco::Logger::get("TablesLoader");
}


void TablesLoader::loadTables()
{
    bool need_resolve_dependencies = !global_context->getConfigRef().has("ignore_table_dependencies_on_metadata_loading");

    /// Load all Lazy, MySQl, PostgreSQL, SQLite, etc databases first.
    for (auto & database : databases)
    {
        if (need_resolve_dependencies && database.second->supportsLoadingInTopologicalOrder())
            databases_to_load.push_back(database.first);
        else
            database.second->loadStoredObjects(global_context, strictness_mode, /* skip_startup_tables */ true);
    }

    if (databases_to_load.empty())
        return;

    /// Read and parse metadata from Ordinary, Atomic, Materialized*, Replicated, etc databases. Build dependency graph.
    for (auto & database_name : databases_to_load)
    {
        databases[database_name]->beforeLoadingMetadata(global_context, strictness_mode);
        bool is_startup = LoadingStrictnessLevel::FORCE_ATTACH <= strictness_mode;
        databases[database_name]->loadTablesMetadata(global_context, metadata, is_startup);
    }

    LOG_INFO(log, "Parsed metadata of {} tables in {} databases in {} sec",
             metadata.parsed_tables.size(), databases_to_load.size(), stopwatch.elapsedSeconds());

    stopwatch.restart();

    buildDependencyGraph();

    /// Update existing info (it's important for ATTACH DATABASE)
    DatabaseCatalog::instance().addDependencies(referential_dependencies);

    /// Remove tables that do not exist
    removeUnresolvableDependencies();

    loadTablesInTopologicalOrder(pool);
}


void TablesLoader::startupTables()
{
    /// Startup tables after all tables are loaded. Background tasks (merges, mutations, etc) may slow down data parts loading.
    for (auto & database : databases)
        database.second->startupTables(pool, strictness_mode);
}


void TablesLoader::buildDependencyGraph()
{
    for (const auto & [table_name, table_metadata] : metadata.parsed_tables)
    {
        auto new_loading_dependencies = getLoadingDependenciesFromCreateQuery(global_context, table_name, table_metadata.ast);

        if (!new_loading_dependencies.empty())
            referential_dependencies.addDependencies(table_name, new_loading_dependencies);

        /// We're adding `new_loading_dependencies` to the graph here even if they're empty because
        /// we need to have all tables from `metadata.parsed_tables` in the graph.
        loading_dependencies.addDependencies(table_name, new_loading_dependencies);
    }

    referential_dependencies.log();
    loading_dependencies.log();
}


void TablesLoader::removeUnresolvableDependencies()
{
    auto need_exclude_dependency = [this](const StorageID & table_id)
    {
        /// Table exists and will be loaded
        if (metadata.parsed_tables.contains(table_id.getQualifiedName()))
            return false;

        if (DatabaseCatalog::instance().isTableExist(table_id, global_context))
        {
            /// Table exists and it's already loaded
        }
        else if (table_id.database_name == metadata.default_database &&
            global_context->getExternalDictionariesLoader().has(table_id.table_name))
        {
            /// Tables depend on a XML dictionary.
            LOG_WARNING(
                log,
                "Tables {} depend on XML dictionary {}, but XML dictionaries are loaded independently."
                "Consider converting it to DDL dictionary.",
                fmt::join(loading_dependencies.getDependents(table_id), ", "),
                table_id);
        }
        else
        {
            /// Some tables depend on table "table_id", but there is no such table in DatabaseCatalog and we don't have its metadata.
            /// We will ignore it and try to load dependent tables without "table_id"
            /// (but most likely dependent tables will fail to load).
            LOG_WARNING(
                log,
                "Tables {} depend on {}, but seems like that does not exist. Will ignore it and try to load existing tables",
                fmt::join(loading_dependencies.getDependents(table_id), ", "),
                table_id);
        }

        size_t num_dependencies, num_dependents;
        loading_dependencies.getNumberOfAdjacents(table_id, num_dependencies, num_dependents);
        if (num_dependencies || !num_dependents)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Table {} does not have dependencies and dependent tables as it expected to."
                                                       "It's a bug", table_id);

        return true; /// Exclude this dependency.
    };

    loading_dependencies.removeTablesIf(need_exclude_dependency);

    if (loading_dependencies.getNumberOfTables() != metadata.parsed_tables.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Number of tables to be loaded is not as expected. It's a bug");

    /// Cannot load tables with cyclic dependencies.
    loading_dependencies.checkNoCyclicDependencies();
}


void TablesLoader::loadTablesInTopologicalOrder(ThreadPool & pool)
{
    /// Compatibility setting which should be enabled by default on attach
    /// Otherwise server will be unable to start for some old-format of IPv6/IPv4 types of columns
    ContextMutablePtr load_context = Context::createCopy(global_context);
    load_context->setSetting("cast_ipv4_ipv6_default_on_conversion_error", 1);

    /// Load tables in parallel.
    auto tables_to_load = loading_dependencies.getTablesSortedByDependencyForParallel();

    for (size_t level = 0; level != tables_to_load.size(); ++level)
    {
        startLoadingTables(pool, load_context, tables_to_load[level], level);
        pool.wait();
    }
}

void TablesLoader::startLoadingTables(ThreadPool & pool, ContextMutablePtr load_context, const std::vector<StorageID> & tables_to_load, size_t level)
{
    size_t total_tables = metadata.parsed_tables.size();

    LOG_INFO(log, "Loading {} tables with dependency level {}", tables_to_load.size(), level);

    for (const auto & table_id : tables_to_load)
    {
        pool.scheduleOrThrowOnError([this, load_context, total_tables, table_name = table_id.getQualifiedName()]()
        {
            const auto & path_and_query = metadata.parsed_tables[table_name];
            databases[table_name.database]->loadTableFromMetadata(load_context, path_and_query.path, table_name, path_and_query.ast, strictness_mode);
            logAboutProgress(log, ++tables_processed, total_tables, stopwatch);
        });
    }
}

}
