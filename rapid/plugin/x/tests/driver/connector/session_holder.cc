/*
 * Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2.0,
 * as published by the Free Software Foundation.
 *
 * This program is also distributed with certain software (including
 * but not limited to OpenSSL) that is licensed under separate terms,
 * as designated in a particular file or component or in included license
 * documentation.  The authors of MySQL hereby grant you an additional
 * permission to link the program and your derivative works with the
 * separately licensed software that they have included with MySQL.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License, version 2.0, for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include "plugin/x/tests/driver/connector/session_holder.h"


Session_holder::Session_holder(std::unique_ptr<xcl::XSession> session,
               const Console &console)
: m_session(std::move(session)), m_console(console) {
}

xcl::XSession *Session_holder::get_session() {
  return m_session.get();
}

bool Session_holder::try_get_number_of_received_messages(
    const std::string message_name,
    uint64_t *value) const {
  assert(nullptr != value);
  if (!m_received_msg_counters.count(message_name))
    return false;

  *value = m_received_msg_counters.at(message_name);

  return true;
}

xcl::XError Session_holder::setup_session(const Connection_options &options) {
  xcl::XError error;

  if (options.socket.empty()) {
    error = m_session->connect(
        options.host.c_str(),
        options.port,
        options.user.c_str(),
        options.password.c_str(),
        options.schema.c_str());
  } else {
    error = m_session->connect(
        options.socket.c_str(),
        options.user.c_str(),
        options.password.c_str(),
        options.schema.c_str());
  }

  return error;
}

xcl::XError Session_holder::setup_connection(const Connection_options &options) {
  xcl::XError error;
  auto       &protocol = m_session->get_protocol();
  auto       &connection = protocol.get_connection();

  if (options.socket.empty()) {
    error = connection.connect(
        options.host.c_str(),
        options.port,
        options.ip_mode);
  } else {
    error = connection.connect_to_localhost(
        options.socket.c_str());
  }

  return error;
}

void Session_holder::setup_ssl(const Connection_options &options) {
  auto ssl_mode = options.ssl_mode;
  if (ssl_mode.empty()) {
    if (options.is_ssl_set())
      ssl_mode = "REQUIRED";
    else
      ssl_mode = "DISABLED";
  }

  auto error = m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Ssl_mode,
      ssl_mode);

  if (error)
    throw error;

  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Ssl_ca,
      options.ssl_ca);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Ssl_ca_path,
      options.ssl_ca_path);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Ssl_cert,
      options.ssl_cert);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Ssl_cipher,
      options.ssl_cipher);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Ssl_key,
      options.ssl_key);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Allowed_tls,
      options.allowed_tls);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Read_timeout,
      options.io_timeout);
  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Write_timeout,
      options.io_timeout);
}

void Session_holder::setup_msg_callbacks(const bool force_trace_protocol) {
  auto &protocol = m_session->get_protocol();

  m_session->set_mysql_option(
      xcl::XSession::Mysqlx_option::Consume_all_notices,
      false);

  m_handler_id = protocol.add_notice_handler(
      [this] (const xcl::XProtocol *protocol,
              const bool is_global,
              const Frame_type type,
              const char *data,
              const uint32_t data_length) -> xcl::Handler_result {
        return dump_notices(protocol, is_global, type, data, data_length);
      });

  protocol.add_received_message_handler(
      [this] (xcl::XProtocol *protocol,
          const xcl::XProtocol::Server_message_type_id msg_id,
          const xcl::XProtocol::Message &msg) -> xcl::Handler_result {
    return count_received_messages(protocol, msg_id, msg);
  });

  /** Push message handlers that are responsible for tracing.
   The functionality is enabled by setting "MYSQLX_TRACE_CONNECTION"
   environment variable to any value.
   */
  const auto should_enable_tracing = getenv("MYSQLX_TRACE_CONNECTION");

  if (force_trace_protocol ||
      (should_enable_tracing && strlen(should_enable_tracing))) {
    protocol.add_received_message_handler(
        [this] (xcl::XProtocol *protocol,
            const xcl::XProtocol::Server_message_type_id msg_id,
            const xcl::XProtocol::Message &msg) -> xcl::Handler_result {
      return trace_received_messages(protocol, msg_id, msg);
    });

    protocol.add_send_message_handler(
        [this] (xcl::XProtocol *protocol,
            const xcl::XProtocol::Client_message_type_id msg_id,
            const xcl::XProtocol::Message &msg) -> xcl::Handler_result {
      return trace_send_messages(protocol, msg_id, msg);
    });
  }
}

void Session_holder::remove_notice_handler() {
  if (m_handler_id >= 0) {
    m_session->get_protocol().remove_notice_handler(m_handler_id);
  }
}

xcl::Handler_result Session_holder::trace_send_messages(
    xcl::XProtocol *protocol,
    const xcl::XProtocol::Client_message_type_id msg_id,
    const xcl::XProtocol::Message &msg) {
  print_message(">>>> SEND ", msg);

  /** None of processed messages should be filtered out*/
  return xcl::Handler_result::Continue;
}

xcl::Handler_result Session_holder::trace_received_messages(
    xcl::XProtocol *protocol,
    const xcl::XProtocol::Server_message_type_id msg_id,
    const xcl::XProtocol::Message &msg) {
  print_message("<<<< RECEIVE ", msg);

  /** None of processed messages should be filtered out*/
  return xcl::Handler_result::Continue;
}

xcl::Handler_result Session_holder::count_received_messages(
    xcl::XProtocol *protocol,
    const xcl::XProtocol::Server_message_type_id msg_id,
    const xcl::XProtocol::Message &msg) {
  const std::string &msg_name = msg.GetDescriptor()->full_name();
  ++m_received_msg_counters[msg_name];

  if (msg_name != Mysqlx::Notice::Frame::descriptor()->full_name())
    return xcl::Handler_result::Continue;

  static const std::string *notice_type_id[] = {
      &Mysqlx::Notice::Warning::descriptor()->full_name(),
      &Mysqlx::Notice::SessionVariableChanged::descriptor()->full_name(),
      &Mysqlx::Notice::SessionStateChanged::descriptor()->full_name()};

  const auto notice_type =
      static_cast<const Mysqlx::Notice::Frame *>(&msg)->type() - 1u;
  if (notice_type < array_elements(notice_type_id))
    ++m_received_msg_counters[*notice_type_id[notice_type]];

  /** None of processed messages should be filtered out*/
  return xcl::Handler_result::Continue;
}

xcl::Handler_result Session_holder::dump_notices(
    const xcl::XProtocol *protocol,
    const bool is_global,
    const Frame_type type,
    const char *data,
    const uint32_t data_length) {
  if (type == Frame_type::Frame_Type_SESSION_STATE_CHANGED) {
    Mysqlx::Notice::SessionStateChanged change;

    change.ParseFromArray(data, data_length);

    if (!change.IsInitialized()) {
      m_console.print_error("Invalid notice received from server ",
                            change.InitializationErrorString(), '\n');
    } else {
      if (change.param() ==
          Mysqlx::Notice::SessionStateChanged::ACCOUNT_EXPIRED) {
        m_console.print("NOTICE: Account password expired\n");

        return xcl::Handler_result::Consumed;
      }
    }
  }

  return xcl::Handler_result::Continue;
}

void Session_holder::print_message(
    const std::string &direction,
    const xcl::XProtocol::Message &msg) {
  m_console.print(direction, msg.ByteSize()+1, " ", msg);
}
