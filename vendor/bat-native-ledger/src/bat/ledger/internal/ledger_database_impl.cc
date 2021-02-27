/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bat/ledger/internal/ledger_database_impl.h"

#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/logging.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace ledger {

namespace {

void HandleBinding(sql::Statement* statement,
                   const mojom::DBCommandBinding& binding) {
  if (!statement) {
    return;
  }

  switch (binding.value->which()) {
    case mojom::DBValue::Tag::STRING_VALUE: {
      statement->BindString(binding.index, binding.value->get_string_value());
      return;
    }
    case mojom::DBValue::Tag::INT_VALUE: {
      statement->BindInt(binding.index, binding.value->get_int_value());
      return;
    }
    case mojom::DBValue::Tag::INT64_VALUE: {
      statement->BindInt64(binding.index, binding.value->get_int64_value());
      return;
    }
    case mojom::DBValue::Tag::DOUBLE_VALUE: {
      statement->BindDouble(binding.index, binding.value->get_double_value());
      return;
    }
    case mojom::DBValue::Tag::BOOL_VALUE: {
      statement->BindBool(binding.index, binding.value->get_bool_value());
      return;
    }
    case mojom::DBValue::Tag::NULL_VALUE: {
      statement->BindNull(binding.index);
      return;
    }
    default: {
      NOTREACHED();
    }
  }
}

mojom::DBRecordPtr CreateRecord(
    sql::Statement* statement,
    const std::vector<mojom::DBCommand::RecordBindingType>& bindings) {
  auto record = mojom::DBRecord::New();
  if (!statement) {
    return record;
  }

  // NOTE: The |record_bindings| member of DBCommand is deprecated but still
  // supported for existing commands.
  if (bindings.size() > 0) {
    int column = 0;
    for (const auto& binding : bindings) {
      auto value = mojom::DBValue::New();
      switch (binding) {
        case mojom::DBCommand::RecordBindingType::STRING_TYPE: {
          value->set_string_value(statement->ColumnString(column));
          break;
        }
        case mojom::DBCommand::RecordBindingType::INT_TYPE: {
          value->set_int_value(statement->ColumnInt(column));
          break;
        }
        case mojom::DBCommand::RecordBindingType::INT64_TYPE: {
          value->set_int64_value(statement->ColumnInt64(column));
          break;
        }
        case mojom::DBCommand::RecordBindingType::DOUBLE_TYPE: {
          value->set_double_value(statement->ColumnDouble(column));
          break;
        }
        case mojom::DBCommand::RecordBindingType::BOOL_TYPE: {
          value->set_bool_value(statement->ColumnBool(column));
          break;
        }
        default: {
          NOTREACHED();
        }
      }
      record->fields.push_back(std::move(value));
      column++;
    }
    return record;
  }

  for (int column = 0; column < statement->ColumnCount(); ++column) {
    auto value = mojom::DBValue::New();
    switch (statement->GetColumnType(column)) {
      case sql::ColumnType::kInteger:
        value->set_int64_value(statement->ColumnInt64(column));
        break;
      case sql::ColumnType::kFloat:
        value->set_double_value(statement->ColumnDouble(column));
        break;
      case sql::ColumnType::kText:
        value->set_string_value(statement->ColumnString(column));
        break;
      case sql::ColumnType::kBlob: {
        std::string blob_string;
        statement->ColumnBlobAsString(column, &blob_string);
        value->set_string_value(blob_string);
        break;
      }
      case sql::ColumnType::kNull:
        value->set_null_value(0);
        break;
    }
    record->fields.push_back(std::move(value));
  }

  return record;
}

}  // namespace

LedgerDatabaseImpl::LedgerDatabaseImpl(const base::FilePath& path)
    : db_path_(path) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

LedgerDatabaseImpl::~LedgerDatabaseImpl() = default;

void LedgerDatabaseImpl::RunTransaction(
    mojom::DBTransactionPtr transaction,
    mojom::DBCommandResponse* command_response) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!command_response) {
    return;
  }

  if (!db_.is_open() && !db_.Open(db_path_)) {
    command_response->status =
        mojom::DBCommandResponse::Status::INITIALIZATION_ERROR;
    return;
  }

  // Close command must always be sent as single command in transaction
  if (transaction->commands.size() == 1 &&
      transaction->commands[0]->type == mojom::DBCommand::Type::CLOSE) {
    db_.Close();
    meta_table_.Reset();
    initialized_ = false;
    command_response->status = mojom::DBCommandResponse::Status::RESPONSE_OK;
    return;
  }

  sql::Transaction committer(&db_);
  if (!committer.Begin()) {
    command_response->status =
        mojom::DBCommandResponse::Status::TRANSACTION_ERROR;
    return;
  }

  bool vacuum_requested = false;

  for (auto const& command : transaction->commands) {
    mojom::DBCommandResponse::Status status;

    switch (command->type) {
      case mojom::DBCommand::Type::INITIALIZE: {
        status = Initialize(transaction->version,
                            transaction->compatible_version, command_response);
        break;
      }
      case mojom::DBCommand::Type::READ: {
        status = Read(command.get(), command_response);
        break;
      }
      case mojom::DBCommand::Type::EXECUTE: {
        status = Execute(command.get(), command_response);
        break;
      }
      case mojom::DBCommand::Type::RUN: {
        status = Run(command.get(), command_response);
        break;
      }
      case mojom::DBCommand::Type::MIGRATE: {
        status = Migrate(transaction->version, transaction->compatible_version);
        break;
      }
      case mojom::DBCommand::Type::VACUUM: {
        vacuum_requested = true;
        status = mojom::DBCommandResponse::Status::RESPONSE_OK;
        break;
      }
      case mojom::DBCommand::Type::CLOSE: {
        NOTREACHED();
        status = mojom::DBCommandResponse::Status::COMMAND_ERROR;
        break;
      }
    }

    if (status != mojom::DBCommandResponse::Status::RESPONSE_OK) {
      committer.Rollback();
      command_response->status = status;
      return;
    }
  }

  if (!committer.Commit()) {
    command_response->status =
        mojom::DBCommandResponse::Status::TRANSACTION_ERROR;
    return;
  }

  if (vacuum_requested) {
    if (!db_.Execute("VACUUM")) {
      // If vacuum was not successful, log an error but do not
      // prevent forward progress.
      LOG(ERROR) << "Error executing VACUUM: " << db_.GetErrorMessage();
    }
  }
}

void LedgerDatabaseImpl::OpenInMemoryForTesting() {
  if (!db_.is_open()) {
    CHECK(db_.OpenInMemory());
  }
}

mojom::DBCommandResponse::Status LedgerDatabaseImpl::Initialize(
    int32_t version,
    int32_t compatible_version,
    mojom::DBCommandResponse* command_response) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!command_response) {
    return mojom::DBCommandResponse::Status::RESPONSE_ERROR;
  }

  int table_version = 0;
  if (!initialized_) {
    bool table_exists = false;
    if (meta_table_.DoesTableExist(&db_)) {
      table_exists = true;
    }

    // NOTE: For a new database, the meta table will be initialized with the
    // current DB version. The current version will be immediately overwritten
    // by the first migration, but it is not atomic: there will be a time when a
    // new, empty database has the current version in its meta table.
    if (!meta_table_.Init(&db_, version, compatible_version)) {
      return mojom::DBCommandResponse::Status::INITIALIZATION_ERROR;
    }

    if (table_exists) {
      table_version = meta_table_.GetVersionNumber();
    }

    initialized_ = true;
    memory_pressure_listener_.reset(new base::MemoryPressureListener(
        FROM_HERE, base::BindRepeating(&LedgerDatabaseImpl::OnMemoryPressure,
                                       base::Unretained(this))));
  } else {
    table_version = meta_table_.GetVersionNumber();
  }

  auto value = mojom::DBValue::New();
  value->set_int_value(table_version);
  auto result = mojom::DBCommandResult::New();
  result->set_value(std::move(value));
  command_response->result = std::move(result);

  return mojom::DBCommandResponse::Status::RESPONSE_OK;
}

mojom::DBCommandResponse::Status LedgerDatabaseImpl::Execute(
    mojom::DBCommand* command,
    mojom::DBCommandResponse* command_response) {
  DCHECK(command);
  DCHECK(command_response);

  if (!initialized_) {
    return mojom::DBCommandResponse::Status::INITIALIZATION_ERROR;
  }

  bool result = db_.Execute(command->command.c_str());

  if (!result) {
    // Ideally database errors should be logged to the Rewards log file, but
    // since this class runs in the browser process it cannot use the BLOG
    // logging macro.
    LOG(ERROR) << "DB Execute error: " << db_.GetErrorMessage();
    return mojom::DBCommandResponse::Status::COMMAND_ERROR;
  }

  auto db_value = mojom::DBValue::New();
  db_value->set_int_value(db_.GetLastChangeCount());
  auto db_result = mojom::DBCommandResult::New();
  db_result->set_value(std::move(db_value));
  command_response->result = std::move(db_result);

  return mojom::DBCommandResponse::Status::RESPONSE_OK;
}

mojom::DBCommandResponse::Status LedgerDatabaseImpl::Run(
    mojom::DBCommand* command,
    mojom::DBCommandResponse* command_response) {
  DCHECK(command);
  DCHECK(command_response);

  if (!initialized_) {
    return mojom::DBCommandResponse::Status::INITIALIZATION_ERROR;
  }

  sql::Statement statement(db_.GetUniqueStatement(command->command.c_str()));

  for (auto const& binding : command->bindings) {
    HandleBinding(&statement, *binding.get());
  }

  if (!statement.Run()) {
    LOG(ERROR) << "DB Run error: " << db_.GetErrorMessage() << " ("
               << db_.GetErrorCode() << ")";
    return mojom::DBCommandResponse::Status::COMMAND_ERROR;
  }

  auto db_value = mojom::DBValue::New();
  db_value->set_int_value(db_.GetLastChangeCount());
  auto db_result = mojom::DBCommandResult::New();
  db_result->set_value(std::move(db_value));
  command_response->result = std::move(db_result);

  return mojom::DBCommandResponse::Status::RESPONSE_OK;
}

mojom::DBCommandResponse::Status LedgerDatabaseImpl::Read(
    mojom::DBCommand* command,
    mojom::DBCommandResponse* command_response) {
  if (!initialized_) {
    return mojom::DBCommandResponse::Status::INITIALIZATION_ERROR;
  }

  if (!command || !command_response) {
    return mojom::DBCommandResponse::Status::RESPONSE_ERROR;
  }

  sql::Statement statement(db_.GetUniqueStatement(command->command.c_str()));

  for (auto const& binding : command->bindings) {
    HandleBinding(&statement, *binding.get());
  }

  auto result = mojom::DBCommandResult::New();
  result->set_records(std::vector<mojom::DBRecordPtr>());
  command_response->result = std::move(result);
  while (statement.Step()) {
    command_response->result->get_records().push_back(
        CreateRecord(&statement, command->record_bindings));
  }

  return mojom::DBCommandResponse::Status::RESPONSE_OK;
}

mojom::DBCommandResponse::Status LedgerDatabaseImpl::Migrate(
    const int32_t version,
    const int32_t compatible_version) {
  if (!initialized_) {
    return mojom::DBCommandResponse::Status::INITIALIZATION_ERROR;
  }

  meta_table_.SetVersionNumber(version);
  meta_table_.SetCompatibleVersionNumber(compatible_version);

  return mojom::DBCommandResponse::Status::RESPONSE_OK;
}

void LedgerDatabaseImpl::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  db_.TrimMemory();
}

}  // namespace ledger
