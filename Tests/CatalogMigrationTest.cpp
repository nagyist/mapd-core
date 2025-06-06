/*
 * Copyright 2022 HEAVY.AI, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file CatalogMigrationTest.cpp
 * @brief Test suite for catalog migrations
 *
 */

#include <gtest/gtest.h>
#include <boost/filesystem.hpp>

#include "Catalog/Catalog.h"
#include "DBHandlerTestHelpers.h"
#include "DataMgr/ForeignStorage/AbstractFileStorageDataWrapper.h"
#include "DataMgr/ForeignStorage/ForeignDataWrapperFactory.h"
#include "Shared/StringTransform.h"
#include "Shared/SysDefinitions.h"
#include "SqliteConnector/SqliteConnector.h"
#include "TestHelpers.h"

#ifndef BASE_PATH
#define BASE_PATH "./tmp"
#endif

extern bool g_enable_fsi;
extern bool g_enable_s3_fsi;
extern bool g_enable_system_tables;

namespace BF = boost::filesystem;
using SC = Catalog_Namespace::SysCatalog;

namespace {
bool table_exists(SqliteConnector& conn, const std::string& table_name) {
  conn.query("SELECT name FROM sqlite_master WHERE type='table' AND name='" + table_name +
             "'");
  return conn.getNumRows() > 0;
}

bool has_result(SqliteConnector& conn, const std::string& query) {
  conn.query(query);
  return conn.getNumRows() > 0;
}
}  // namespace

class CatalogTest : public DBHandlerTestFixture {
 protected:
  CatalogTest()
      : cat_conn_(shared::kDefaultDbName,
                  BF::absolute(shared::kCatalogDirectoryName, BASE_PATH).string()) {}

  static void SetUpTestSuite() {
    DBHandlerTestFixture::createDBHandler();
    initSysCatalog();
  }

  static void initSysCatalog() {
    auto db_handler = getDbHandlerAndSessionId().first;
    SC::instance().init(
        BASE_PATH, db_handler->data_mgr_, {}, db_handler->calcite_, false, false, {}, {});
  }

  std::vector<std::string> getTables(SqliteConnector& conn) {
    conn.query("SELECT name FROM sqlite_master WHERE type='table';");
    std::vector<std::string> tables;
    for (size_t i = 0; i < conn.getNumRows(); i++) {
      tables.emplace_back(conn.getData<std::string>(i, 0));
    }
    return tables;
  }

  std::unique_ptr<Catalog_Namespace::Catalog> initCatalog(
      const std::string& db_name = shared::kDefaultDbName) {
    Catalog_Namespace::DBMetadata db_metadata;
    db_metadata.dbName = db_name;
    db_metadata.dbId = 1;
    std::vector<LeafHostInfo> leaves{};
    return std::make_unique<Catalog_Namespace::Catalog>(
        BASE_PATH, db_metadata, nullptr, leaves, nullptr, false);
  }

  SqliteConnector cat_conn_;
};

class SysCatalogTest : public CatalogTest {
 protected:
  SysCatalogTest()
      : syscat_conn_(shared::kSystemCatalogName,
                     BF::absolute(shared::kCatalogDirectoryName, BASE_PATH).string()) {}

  void TearDown() override {
    if (tableExists("mapd_users")) {
      syscat_conn_.query("DELETE FROM mapd_users WHERE name='test_user'");
    }
    if (tableExists("mapd_object_permissions")) {
      syscat_conn_.query(
          "DELETE FROM mapd_object_permissions WHERE roleName='test_user'");
    }
  }

  bool hasResult(const std::string& query) { return has_result(syscat_conn_, query); }

  bool tableExists(const std::string& table_name) {
    return table_exists(syscat_conn_, table_name);
  }

  void createLegacyTestUser() {
    // This creates a test user in mapd_users syscat table, but does not properly add it
    // to mapd_object_permissions so it is incomplete by current standards.
    ASSERT_TRUE(table_exists(syscat_conn_, "mapd_users"));
    syscat_conn_.query("DELETE FROM mapd_users WHERE name='test_user'");
    syscat_conn_.query_with_text_params(
        "INSERT INTO mapd_users (name, passwd_hash, issuper, can_login) VALUES (?, ?, ?, "
        "?)",
        {"test_user", "passwd", "true", "true"});
  }

  static void reinitializeSystemCatalog() {
    SC::destroy();
    initSysCatalog();
  }

  SqliteConnector syscat_conn_;
};

// Check that we migrate correctly from pre 4.0 catalog.
TEST_F(SysCatalogTest, MigrateRoles) {
  // Make sure the post 4.0 tables do not exist to simulate migration.
  syscat_conn_.query("DROP TABLE IF EXISTS mapd_roles");
  syscat_conn_.query("DROP TABLE IF EXISTS mapd_object_permissions");
  createLegacyTestUser();

  // Create the pre 4.0 mapd_privileges table.
  syscat_conn_.query(
      "CREATE TABLE IF NOT EXISTS mapd_privileges (userid integer references mapd_users, "
      "dbid integer references mapd_databases, select_priv boolean, insert_priv boolean, "
      "UNIQUE(userid, dbid))");

  // Copy users who are not the admin (userid 0) into the pre 4.0 mapd_privileges table.
  syscat_conn_.query(
      "INSERT INTO mapd_privileges (userid, dbid) SELECT userid, default_db FROM "
      "mapd_users WHERE userid <> 0");

  // Re-initialization should perform migrations.
  reinitializeSystemCatalog();

  // Users should be inserted into mapd_object_permissions but not mapd_roles on
  // migration.
  ASSERT_TRUE(tableExists("mapd_roles"));
  ASSERT_FALSE(hasResult("SELECT roleName FROM mapd_roles WHERE roleName='test_user'"));

  ASSERT_TRUE(tableExists("mapd_object_permissions"));
  ASSERT_TRUE(hasResult(
      "SELECT roleName FROM mapd_object_permissions WHERE roleName='test_user'"));
}

TEST_F(SysCatalogTest, FixIncorrectRolesMigration) {
  ASSERT_TRUE(tableExists("mapd_roles"));
  createLegacyTestUser();

  // Setup an incorrect migration situation where we have usernames inserted into
  // mapd_roles.  This could occur between versions 4.0 and 5.7 and should now be fixed.
  ASSERT_TRUE(tableExists("mapd_users"));
  syscat_conn_.query("DELETE FROM mapd_roles WHERE roleName='test_user'");
  syscat_conn_.query_with_text_params("INSERT INTO mapd_roles VALUES (?, ?)",
                                      {"test_user", "test_user"});

  ASSERT_TRUE(hasResult("SELECT name FROM mapd_users WHERE name='test_user'"));
  ASSERT_TRUE(hasResult("SELECT roleName FROM mapd_roles WHERE roleName='test_user'"));

  // When we re-initialize the SysCatalog we should fix incorrect past migrations.
  reinitializeSystemCatalog();

  ASSERT_TRUE(hasResult("SELECT name FROM mapd_users WHERE name='test_user'"));
  ASSERT_FALSE(hasResult("SELECT roleName FROM mapd_roles WHERE roleName='test_user'"));
}

class FsiSchemaTest : public CatalogTest {
 protected:
  static void SetUpTestSuite() {
    g_enable_s3_fsi = true;
    g_enable_fsi = true;
    CatalogTest::SetUpTestSuite();
  }

  void SetUp() override {
    g_enable_fsi = false;
    g_enable_s3_fsi = false;
    g_enable_system_tables = false;
    dropFsiTables();
  }

  void TearDown() override { dropFsiTables(); }

  void assertExpectedDefaultServer(Catalog_Namespace::Catalog* catalog,
                                   const std::string& server_name,
                                   const std::string& data_wrapper,
                                   const int32_t user_id) {
    auto foreign_server = catalog->getForeignServerFromStorage(server_name);

    ASSERT_GT(foreign_server->id, 0);
    ASSERT_EQ(server_name, foreign_server->name);
    ASSERT_EQ(data_wrapper, foreign_server->data_wrapper_type);
    ASSERT_EQ(user_id, foreign_server->user_id);

    ASSERT_TRUE(foreign_server->options.find(
                    foreign_storage::AbstractFileStorageDataWrapper::STORAGE_TYPE_KEY) !=
                foreign_server->options.end());
    ASSERT_EQ(foreign_storage::AbstractFileStorageDataWrapper::LOCAL_FILE_STORAGE_TYPE,
              foreign_server->options
                  .find(foreign_storage::AbstractFileStorageDataWrapper::STORAGE_TYPE_KEY)
                  ->second);

    ASSERT_TRUE(foreign_server->options.find(
                    foreign_storage::AbstractFileStorageDataWrapper::BASE_PATH_KEY) ==
                foreign_server->options.end());

    // check that server loaded from storage matches that in memory
    auto foreign_server_in_memory = catalog->getForeignServer(server_name);

    ASSERT_EQ(foreign_server->id, foreign_server_in_memory->id);
    ASSERT_EQ(foreign_server_in_memory->name, foreign_server->name);
    ASSERT_EQ(foreign_server_in_memory->data_wrapper_type,
              foreign_server->data_wrapper_type);
    ASSERT_EQ(foreign_server_in_memory->user_id, foreign_server->user_id);

    ASSERT_TRUE(foreign_server_in_memory->options.find(
                    foreign_storage::AbstractFileStorageDataWrapper::STORAGE_TYPE_KEY) !=
                foreign_server_in_memory->options.end());
    ASSERT_EQ(foreign_storage::AbstractFileStorageDataWrapper::LOCAL_FILE_STORAGE_TYPE,
              foreign_server_in_memory->options
                  .find(foreign_storage::AbstractFileStorageDataWrapper::STORAGE_TYPE_KEY)
                  ->second);

    ASSERT_TRUE(foreign_server_in_memory->options.find(
                    foreign_storage::AbstractFileStorageDataWrapper::BASE_PATH_KEY) ==
                foreign_server_in_memory->options.end());
  }

  void assertFsiTablesExist() {
    auto tables = getTables(cat_conn_);
    ASSERT_FALSE(std::find(tables.begin(), tables.end(), "omnisci_foreign_servers") ==
                 tables.end());
    ASSERT_FALSE(std::find(tables.begin(), tables.end(), "omnisci_foreign_tables") ==
                 tables.end());
  }

  void assertFsiTablesDoNotExist() {
    auto tables = getTables(cat_conn_);
    ASSERT_TRUE(std::find(tables.begin(), tables.end(), "omnisci_foreign_servers") ==
                tables.end());
    ASSERT_TRUE(std::find(tables.begin(), tables.end(), "omnisci_foreign_tables") ==
                tables.end());
  }

 private:
  void dropFsiTables() {
    cat_conn_.query("DROP TABLE IF EXISTS omnisci_foreign_servers;");
    cat_conn_.query("DROP TABLE IF EXISTS omnisci_foreign_tables;");
  }
};

TEST_F(FsiSchemaTest, FsiTablesNotCreatedWhenFsiIsDisabled) {
  assertFsiTablesDoNotExist();

  auto catalog = initCatalog();
  assertFsiTablesDoNotExist();
}

TEST_F(FsiSchemaTest, FsiTablesAreCreatedWhenFsiIsEnabled) {
  assertFsiTablesDoNotExist();

  g_enable_fsi = true;
  auto catalog = initCatalog();
  assertFsiTablesExist();
}

TEST_F(FsiSchemaTest, FsiTablesAreNotDroppedWhenFsiIsDisabled) {
  assertFsiTablesDoNotExist();

  g_enable_fsi = true;
  initCatalog();
  assertFsiTablesExist();

  g_enable_fsi = false;
  initCatalog();
  assertFsiTablesExist();
}

class ForeignTablesTest : public DBHandlerTestFixture {
 protected:
  static void SetUpTestSuite() {
    g_enable_fsi = true;
    DBHandlerTestFixture::SetUpTestSuite();
  }

  static void TearDownTestSuite() {
    DBHandlerTestFixture::TearDownTestSuite();
    g_enable_fsi = false;
  }

  void SetUp() override {
    g_enable_fsi = true;
    DBHandlerTestFixture::SetUp();
    dropTestTables();
  }

  void TearDown() override {
    g_enable_fsi = true;
    g_enable_system_tables = true;
    g_enable_s3_fsi = true;
    dropTestTables();
    DBHandlerTestFixture::TearDown();
  }

 private:
  void dropTestTables() {
    sql("DROP FOREIGN TABLE IF EXISTS test_foreign_table;");
    sql("DROP TABLE IF EXISTS test_table;");
    sql("DROP VIEW IF EXISTS test_view;");
  }
};

TEST_F(ForeignTablesTest, ForeignTablesAreNotDroppedWhenFsiIsDisabled) {
  g_enable_fsi = true;
  resetCatalog();
  loginAdmin();

  const auto file_path = BF::canonical("../../Tests/FsiDataFiles/example_1.csv").string();
  sql("CREATE FOREIGN TABLE test_foreign_table (c1 int) SERVER default_local_delimited "
      "WITH (file_path = '" +
      file_path + "');");
  sql("CREATE TABLE test_table (c1 int);");
  sql("CREATE VIEW test_view AS SELECT * FROM test_table;");

  ASSERT_NE(nullptr, getCatalog().getMetadataForTable("test_foreign_table", false));
  ASSERT_NE(nullptr, getCatalog().getMetadataForTable("test_table", false));
  ASSERT_NE(nullptr, getCatalog().getMetadataForTable("test_view", false));

  g_enable_fsi = false;
  // The following flags should be false when FSI is disabled.
  g_enable_system_tables = false;
  g_enable_s3_fsi = false;

  resetCatalog();
  loginAdmin();

  ASSERT_NE(nullptr, getCatalog().getMetadataForTable("test_foreign_table", false));
  ASSERT_NE(nullptr, getCatalog().getMetadataForTable("test_table", false));
  ASSERT_NE(nullptr, getCatalog().getMetadataForTable("test_view", false));
}

class DefaultForeignServersTest : public FsiSchemaTest {};

TEST_F(DefaultForeignServersTest, DefaultServersAreCreatedWhenFsiIsEnabled) {
  g_enable_fsi = true;
  auto catalog = initCatalog();
  g_enable_fsi = false;

  assertExpectedDefaultServer(catalog.get(),
                              "default_local_delimited",
                              foreign_storage::DataWrapperType::CSV,
                              shared::kRootUserId);

  assertExpectedDefaultServer(catalog.get(),
                              "default_local_parquet",
                              foreign_storage::DataWrapperType::PARQUET,
                              shared::kRootUserId);
}

class CommentSchemaTest : public CatalogTest {
 protected:
  void SetUp() override {
    CatalogTest::SetUp();
    sql("DROP DATABASE IF EXISTS test_database;");
    initCatalogPreUpdate();
  }

  void TearDown() override {
    sql("DROP DATABASE IF EXISTS test_database;");
    CatalogTest::TearDown();
  }

  void initCatalogPreUpdate() {
    sql("CREATE DATABASE test_database;");

    auto catalog = SC::instance().getCatalog("test_database");
    auto& connection = catalog->getSqliteConnector();

    connection.query("DROP TABLE mapd_tables");
    connection.query("DROP TABLE mapd_columns");
    connection.query(
        "CREATE TABLE mapd_tables (tableid integer primary key, name text unique, userid "
        "integer, ncolumns integer, "
        "isview boolean, "
        "fragments text, frag_type integer, max_frag_rows integer, max_chunk_size "
        "bigint, "
        "frag_page_size integer, "
        "max_rows bigint, partitions text, shard_column_id integer, shard integer, "
        "sort_column_id integer default 0, storage_type text default '', "
        "max_rollback_epochs integer default -1, "
        "is_system_table boolean default 0, "
        "num_shards integer, key_metainfo TEXT, version_num "
        "BIGINT DEFAULT 1) ");
    connection.query(
        "CREATE TABLE mapd_columns (tableid integer references mapd_tables, columnid "
        "integer, name text, coltype "
        "integer, colsubtype integer, coldim integer, colscale integer, is_notnull "
        "boolean, compression integer, "
        "comp_param integer, size integer, chunks text, is_systemcol boolean, "
        "is_virtualcol boolean, virtual_expr "
        "text, is_deletedcol boolean, version_num BIGINT, default_value text,"
        "primary key(tableid, columnid), unique(tableid, name))");
  }

  void checkCatalogTableHasColumn(Catalog_Namespace::Catalog* catalog,
                                  const std::string& table,
                                  const std::string& column) {
    auto& connection = catalog->getSqliteConnector();
    connection.query("PRAGMA TABLE_INFO(" + table + ")");
    std::vector<std::string> cols;
    for (size_t i = 0; i < connection.getNumRows(); i++) {
      cols.push_back(connection.getData<std::string>(i, 1));
    }
    ASSERT_TRUE(std::find(cols.begin(), cols.end(), std::string(column)) != cols.end())
        << " failed to find column " + column + " in catalog table " + table;
  }
};

TEST_F(CommentSchemaTest, ValidateSchemaUpdateTablesTable) {
  auto catalog = initCatalog("test_database");  // Performs migrations and schema updates
  checkCatalogTableHasColumn(catalog.get(), "mapd_tables", "comment");
}

TEST_F(CommentSchemaTest, ValidateSchemaUpdateColumnsTable) {
  auto catalog = initCatalog("test_database");  // Performs migrations and schema updates
  checkCatalogTableHasColumn(catalog.get(), "mapd_columns", "comment");
}

class SystemTableMigrationTest : public SysCatalogTest {
 protected:
  void SetUp() override {
    g_enable_system_tables = true;
    g_enable_fsi = true;
    dropInformationSchemaDb();
    deleteInformationSchemaMigration();
  }

  void TearDown() override {
    dropInformationSchemaDb();
    deleteInformationSchemaMigration();
    g_enable_system_tables = false;
    g_enable_fsi = false;
  }

  void dropInformationSchemaDb() {
    auto& system_catalog = SC::instance();
    Catalog_Namespace::DBMetadata db_metadata;
    if (system_catalog.getMetadataForDB(shared::kInfoSchemaDbName, db_metadata)) {
      system_catalog.dropDatabase(db_metadata);
    }
  }

  void deleteInformationSchemaMigration() {
    if (tableExists("mapd_version_history")) {
      syscat_conn_.query_with_text_param(
          "DELETE FROM mapd_version_history WHERE migration_history = ?",
          shared::kInfoSchemaMigrationName);
    }
  }

  bool isInformationSchemaMigrationRecorded() {
    return hasResult("SELECT * FROM mapd_version_history WHERE migration_history = '" +
                     shared::kInfoSchemaMigrationName + "';");
  }
};

TEST_F(SystemTableMigrationTest, SystemTablesEnabled) {
  g_enable_system_tables = true;
  g_enable_fsi = true;
  reinitializeSystemCatalog();
  ASSERT_TRUE(isInformationSchemaMigrationRecorded());
}

TEST_F(SystemTableMigrationTest, PreExistingInformationSchemaDatabase) {
  g_enable_system_tables = false;
  SC::instance().createDatabase("information_schema", shared::kRootUserId);

  g_enable_system_tables = true;
  g_enable_fsi = true;
  reinitializeSystemCatalog();
  ASSERT_FALSE(isInformationSchemaMigrationRecorded());
}

class LegacyDataWrapperMigrationTest : public FsiSchemaTest {
 protected:
  struct LegacyDataWrapperMapping {
    std::string test_server_name;
    std::string old_data_wrapper_name;
    std::string new_data_wrapper_name;
  };

  void insertForeignServer(const std::string& server_name,
                           const std::string& data_wrapper_type) {
    cat_conn_.query_with_text_params(
        "INSERT INTO omnisci_foreign_servers (name, data_wrapper_type, owner_user_id, "
        "creation_time, options) VALUES (?, ?, ?, ?, ?)",
        std::vector<std::string>{server_name,
                                 data_wrapper_type,
                                 shared::kRootUserIdStr,
                                 std::to_string(std::time(nullptr)),
                                 "{\"STORAGE_TYPE\":\"LOCAL_FILE\"}"});
  }

  void assertForeignServerCount(const std::string& server_name,
                                const std::string& data_wrapper_type,
                                size_t expected_count) {
    cat_conn_.query_with_text_params(
        "SELECT COUNT(*) FROM omnisci_foreign_servers WHERE name = ? AND "
        "data_wrapper_type = ?",
        std::vector<std::string>{server_name, data_wrapper_type});
    ASSERT_EQ(cat_conn_.getNumRows(), static_cast<size_t>(1));
    ASSERT_EQ(cat_conn_.getData<size_t>(0, 0), expected_count);
  }

  void clearMigration(const std::string& migration_name) {
    cat_conn_.query_with_text_params(
        "DELETE FROM mapd_version_history WHERE migration_history = ?",
        std::vector<std::string>{migration_name});
  }
};

TEST_F(LegacyDataWrapperMigrationTest, LegacyDataWrappersAreRenamed) {
  g_enable_fsi = true;
  initCatalog();
  assertFsiTablesExist();

  using foreign_storage::DataWrapperType;
  // clang-format off
  std::vector<LegacyDataWrapperMapping> legacy_data_wrapper_mappings{
    LegacyDataWrapperMapping{"test_csv_server",
                             "OMNISCI_CSV",
                             DataWrapperType::CSV},
    LegacyDataWrapperMapping{"test_parquet_server",
                             "OMNISCI_PARQUET",
                             DataWrapperType::PARQUET},
    LegacyDataWrapperMapping{"test_regex_server",
                             "OMNISCI_REGEX_PARSER",
                             DataWrapperType::REGEX_PARSER},
    LegacyDataWrapperMapping{"test_catalog_server",
                             "OMNISCI_INTERNAL_CATALOG",
                             DataWrapperType::INTERNAL_CATALOG},
    LegacyDataWrapperMapping{"test_memory_stats_server",
                             "INTERNAL_OMNISCI_MEMORY_STATS",
                             DataWrapperType::INTERNAL_MEMORY_STATS},
    LegacyDataWrapperMapping{"test_storage_stats_server",
                             "INTERNAL_OMNISCI_STORAGE_STATS",
                             DataWrapperType::INTERNAL_STORAGE_STATS}
  };
  // clang-format on

  for (const auto& mapping : legacy_data_wrapper_mappings) {
    // Insert foreign servers that use legacy data wrapper names
    insertForeignServer(mapping.test_server_name, mapping.old_data_wrapper_name);

    assertForeignServerCount(mapping.test_server_name, mapping.old_data_wrapper_name, 1);
    assertForeignServerCount(mapping.test_server_name, mapping.new_data_wrapper_name, 0);
  }

  clearMigration("rename_legacy_data_wrappers");
  initCatalog();
  for (const auto& mapping : legacy_data_wrapper_mappings) {
    // Assert that foreign servers now use the new data wrapper names
    assertForeignServerCount(mapping.test_server_name, mapping.old_data_wrapper_name, 0);
    assertForeignServerCount(mapping.test_server_name, mapping.new_data_wrapper_name, 1);
  }
}

class SystemCatalogMigrationTest : public DBHandlerTestFixture {
 protected:
  void SetUp() override {
    dirname_ = std::filesystem::path(BASE_PATH) / std::filesystem::path("catalogs");
    dbname_ = "system_catalog";
    dbname_old_ = "system_catalog_backup";

    // NOTE: This test possibly puts the (global test) system catalog in an
    // undefined state; in order to not affect other test cases, we restore the
    // original system catalog after this test completes.
    removeSysCatalogIfExists(dbname_old_);
    snapshotSysCatalog();

    destroyDBHandler();

    sys_catalog_sqlite_connector_ = std::make_unique<SqliteConnector>(dbname_, dirname_);
  }

  void TearDown() override {
    DBHandlerTestFixture::TearDown();
    removeSysCatalogIfExists(dbname_);
    restoreSysCatalog();
  }

  void removeSysCatalogIfExists(const std::string& dbname) {
    std::filesystem::path catalog_file = std::filesystem::path(dirname_) / dbname;
    if (std::filesystem::exists(catalog_file)) {
      std::filesystem::remove_all(catalog_file);
    }
  }

  void snapshotSysCatalog() {
    std::filesystem::copy(std::filesystem::path(dirname_) / dbname_,
                          std::filesystem::path(dirname_) / dbname_old_);
  }

  void restoreSysCatalog() {
    std::filesystem::copy(std::filesystem::path(dirname_) / dbname_old_,
                          std::filesystem::path(dirname_) / dbname_);
  }

  void assertExpectedQueryResult(
      const std::string query,
      const std::vector<std::vector<std::string>>& expected_values,
      const std::optional<std::vector<bool>>& case_insensitive_col_opt = std::nullopt) {
    sys_catalog_sqlite_connector_->query(query);

    size_t max_col_size = 0;
    for (const auto& row : expected_values) {
      max_col_size = std::max(row.size(), max_col_size);
    }
    ASSERT_EQ(sys_catalog_sqlite_connector_->getNumRows(), expected_values.size());
    ASSERT_GE(sys_catalog_sqlite_connector_->getNumCols(), max_col_size);

    std::vector<bool> case_insensitive_col;
    if (case_insensitive_col_opt.has_value()) {
      case_insensitive_col = case_insensitive_col_opt.value();
    } else {
      case_insensitive_col.assign(expected_values[0].size(), false);
    }
    for (size_t i = 0; i < expected_values.size(); ++i) {
      CHECK_EQ(case_insensitive_col.size(), expected_values[i].size());
    }

    for (size_t irow = 0; irow < expected_values.size(); ++irow) {
      auto& row = expected_values[irow];
      for (size_t icol = 0; icol < row.size(); ++icol) {
        auto value = sys_catalog_sqlite_connector_->getData<std::string>(irow, icol);
        if (!case_insensitive_col[icol]) {
          ASSERT_EQ(row[icol], value)
              << " failed case sensitive equality at irow = " << irow
              << " icol = " << icol;
        } else {
          ASSERT_EQ(to_upper(row[icol]), to_upper(value))
              << " failed case insensitive equality at irow = " << irow
              << " icol = " << icol;
        }
      }
    }
  }

  std::unique_ptr<SqliteConnector> sys_catalog_sqlite_connector_;
  std::string dirname_;
  std::string dbname_;
  std::string dbname_old_;
};

class ImmerseMetadataMigrationTest : public SystemCatalogMigrationTest {
 protected:
  void SetUp() override {
    SystemCatalogMigrationTest::SetUp();

    // Setup a pre-migrated state of mapd_users
    sys_catalog_sqlite_connector_->query("DROP TABLE IF EXISTS mapd_users;");
    sys_catalog_sqlite_connector_->query(
        "CREATE TABLE mapd_users (userid integer primary key, name text unique, "
        "passwd_hash text, issuper boolean, default_db integer references "
        "mapd_databases, can_login boolean);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_users "
        "VALUES(0,'admin','$2a$12$WaJQNlOE1q.D7Ity5mRqkehvui3ePmUI/"
        "HorBDeYAS74KVFuBb2au',1,NULL,1);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_users "
        "VALUES(1,'test_user1','$2a$12$GWEpeJWheabNCra9LOV85uIwfV8."
        "8yDwLoDxUMe3POwY4qr59q2pu',0,NULL,1);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_users "
        "VALUES(2,'test_user2','$2a$12$R9o4uYIibqcEPGtUvsLCpOorUF8i7zng/j8KlJ/"
        "O689ntfrNICLQu',0,NULL,1);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_users "
        "VALUES(3,'test_user3','$2a$12$hw4XAkNZAg.KaI9vQAhqcu6zKsI.Bh8DKos5oa/"
        ".yt7nT.3zn2/Te',0,NULL,1);");

    // Setup a pre-migrated state of mapd_databases
    sys_catalog_sqlite_connector_->query("DROP TABLE IF EXISTS mapd_databases;");
    sys_catalog_sqlite_connector_->query(
        "CREATE TABLE mapd_databases (dbid integer primary key, name text unique, owner "
        "integer references mapd_users)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_databases "
        "VALUES(1,'heavyai',0);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_databases "
        "VALUES(2,'information_schema',0);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_databases "
        "VALUES(3,'test_db1',0);");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_databases "
        "VALUES(4,'test_db2',0);");
  }

  void TearDown() override { SystemCatalogMigrationTest::TearDown(); }
};

TEST_F(ImmerseMetadataMigrationTest, MockMigration) {
  // The migration happens as part of creating the db handler below, see method
  // `SetUp` for details of system catalog configuration prior to migration
  createDBHandler();

  // Validate mapd_users migration
  assertExpectedQueryResult("PRAGMA TABLE_INFO(mapd_users)",
                            {{"0", "userid", "integer"},
                             {"1", "name", "text"},
                             {"2", "passwd_hash", "text"},
                             {"3", "issuper", "boolean"},
                             {"4", "default_db", "integer"},
                             {"5", "can_login", "boolean"},
                             {"6", "immerse_metadata_json", "text"}},
                            std::vector<bool>{false, false, true});

  // clang-format off
  assertExpectedQueryResult("SELECT * FROM mapd_users ORDER BY userid",
  {
    { "0", "admin",
        "$2a$12$WaJQNlOE1q.D7Ity5mRqkehvui3ePmUI/HorBDeYAS74KVFuBb2au", "1",
        "", "1", "" },
    { "1", "test_user1",
        "$2a$12$GWEpeJWheabNCra9LOV85uIwfV8.8yDwLoDxUMe3POwY4qr59q2pu", "0",
        "", "1", "" },
    { "2", "test_user2",
        "$2a$12$R9o4uYIibqcEPGtUvsLCpOorUF8i7zng/j8KlJ/O689ntfrNICLQu", "0",
        "", "1", "" },
    { "3", "test_user3",
        "$2a$12$hw4XAkNZAg.KaI9vQAhqcu6zKsI.Bh8DKos5oa/.yt7nT.3zn2/Te", "0",
        "", "1", "" }
  }
  );
  // clang-format off

  // Validate mapd_databases migration
  assertExpectedQueryResult("PRAGMA TABLE_INFO(mapd_databases)",
                            {{"0", "dbid", "integer"},
                             {"1", "name", "text"},
                             {"2", "owner", "integer"},
                             {"3", "immerse_metadata_json", "text"}},
                            std::vector<bool>{false, false, true});
  // clang-format off
  assertExpectedQueryResult("SELECT * FROM mapd_databases ORDER BY dbid",
  {
    { "1", "heavyai", "0", ""},
    { "2", "information_schema", "0", ""},
    { "3", "test_db1", "0", ""},
    { "4", "test_db2", "0", ""},
  }
  );
  // clang-format off
}

class ColumnLevelSecurityMigrationTest : public SystemCatalogMigrationTest {
 protected:
  void SetUp() override {
    SystemCatalogMigrationTest::SetUp();
    sys_catalog_sqlite_connector_->query("DROP TABLE IF EXISTS mapd_object_permissions;");
    sys_catalog_sqlite_connector_->query(
        "CREATE TABLE mapd_object_permissions (roleName TEXT, roleType bool, dbId "
        "integer references mapd_databases, objectName text, objectId integer, "
        "objectPermissionsType integer, objectPermissions integer, objectOwnerId "
        "integer, UNIQUE(roleName, objectPermissionsType, dbId, objectId))");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('admin',1,0,'heavyai',-1,1,0,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user1',1,0,'heavyai',-1,1,0,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user2',1,0,'heavyai',-1,1,0,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user1',1,1,'heavyai',-1,1,8,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user2',1,1,'heavyai',-1,1,8,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user1',1,1,'test_table1',4,2,4,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user2',1,1,'test_table2',5,2,8,0)");
    sys_catalog_sqlite_connector_->query(
        "INSERT INTO mapd_object_permissions VALUES('user2',1,1,'heavyai',-1,2,4,0)");
  }

  void TearDown() override { SystemCatalogMigrationTest::TearDown(); }

  void checkTableDoesNotExist(const std::string& table_name) {
    sys_catalog_sqlite_connector_->query("PRAGMA TABLE_INFO(" + table_name + ")");
    ASSERT_EQ(sys_catalog_sqlite_connector_->getNumRows(), 0UL);
  }

  void verifyUniqueConstraint() {
    sys_catalog_sqlite_connector_->query("PRAGMA INDEX_LIST(mapd_object_permissions)");
    ASSERT_EQ(sys_catalog_sqlite_connector_->getNumRows(), 1UL);
    ASSERT_EQ(sys_catalog_sqlite_connector_->getNumCols(), 5UL);
    ASSERT_EQ(sys_catalog_sqlite_connector_->getData<int>(0, 2),
              1);  // This asserts the columns are sorted
    auto index_name = sys_catalog_sqlite_connector_->getData<std::string>(0, 1);
    assertExpectedQueryResult("PRAGMA INDEX_INFO(\"" + index_name + "\")",
                              {{"0", "0", "roleName"},
                               {"1", "5", "objectPermissionsType"},
                               {"2", "2", "dbId"},
                               {"3", "4", "objectId"},
                               {"4", "8", "subObjectId"}});
  }
};

TEST_F(ColumnLevelSecurityMigrationTest, MockMigration) {
  // The migration happens as part of creating the db handler below, see method
  // `SetUp` for details of system catalog configuration prior to migration
  createDBHandler();

  assertExpectedQueryResult("PRAGMA TABLE_INFO(mapd_object_permissions)",
                            {{"0", "roleName", "text"},
                             {"1", "roleType", "bool"},
                             {"2", "dbId", "integer"},
                             {"3", "objectName", "text"},
                             {"4", "objectId", "integer"},
                             {"5", "objectPermissionsType", "integer"},
                             {"6", "objectPermissions", "integer"},
                             {"7", "objectOwnerId", "integer"},
                             {"8", "subObjectId", "integer"}},
                            std::vector<bool>{false, false, true});

  assertExpectedQueryResult(
      "SELECT * FROM mapd_object_permissions",
      {
          {"admin", "1", "0", "heavyai", "-1", "1", "0", "0", "-1"},
          {"user1", "1", "0", "heavyai", "-1", "1", "0", "0", "-1"},
          {"user2", "1", "0", "heavyai", "-1", "1", "0", "0", "-1"},
          {"user1", "1", "1", "heavyai", "-1", "1", "8", "0", "-1"},
          {"user2", "1", "1", "heavyai", "-1", "1", "8", "0", "-1"},
          {"user1", "1", "1", "test_table1", "4", "2", "4", "0", "-1"},
          {"user2", "1", "1", "test_table2", "5", "2", "8", "0", "-1"},
          {"user2", "1", "1", "heavyai", "-1", "2", "4", "0", "-1"},
      });

  verifyUniqueConstraint();

  checkTableDoesNotExist("mapd_object_permissions_original");
}

int main(int argc, char** argv) {
  TestHelpers::init_logger_stderr_only(argc, argv);
  testing::InitGoogleTest(&argc, argv);

  int err{0};
  try {
    testing::AddGlobalTestEnvironment(new DBHandlerTestEnvironment);
    err = RUN_ALL_TESTS();
  } catch (const std::exception& e) {
    LOG(ERROR) << e.what();
  }

  return err;
}
