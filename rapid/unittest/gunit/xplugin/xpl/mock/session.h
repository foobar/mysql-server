/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
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

#include "plugin/x/ngs/include/ngs/interface/account_verification_interface.h"
#include "plugin/x/ngs/include/ngs/interface/client_interface.h"
#include "plugin/x/ngs/include/ngs/interface/protocol_encoder_interface.h"
#include "plugin/x/ngs/include/ngs/interface/server_interface.h"
#include "plugin/x/ngs/include/ngs/interface/sql_session_interface.h"
#include "plugin/x/ngs/include/ngs/client.h"
#include "plugin/x/ngs/include/ngs/scheduler.h"
#include "plugin/x/ngs/include/ngs_common/connection_vio.h"
#include "plugin/x/src/account_verification_handler.h"
#include "plugin/x/src/sql_data_context.h"
#include "plugin/x/src/xpl_resultset.h"
#include "plugin/x/src/xpl_session.h"


namespace ngs
{
namespace test
{

class Mock_vio : public Vio_interface
{
public:
  MOCK_METHOD2(read, ssize_t(uchar* buffer, ssize_t bytes_to_send));
  MOCK_METHOD2(write, ssize_t(const uchar* buffer, ssize_t bytes_to_send));
  MOCK_METHOD2(set_timeout, void(const Vio_interface::Direction,
               const uint32_t timeout));
  MOCK_METHOD1(set_state, void(PSI_socket_state state));
  MOCK_METHOD0(set_thread_owner, void());
  MOCK_METHOD0(get_fd, my_socket());
  MOCK_METHOD0(get_type, enum_vio_type());
  MOCK_METHOD2(peer_addr, sockaddr_storage*(std::string &address, uint16 &port));
  MOCK_METHOD0(shutdown, int());
  MOCK_METHOD0(delete_vio, void());
  MOCK_METHOD0(get_vio, Vio*());
};

class Mock_ssl_context : public Ssl_context_interface
{
 public:
  MOCK_METHOD8(setup, bool(const char* tls_version, const char* ssl_key,
      const char* ssl_ca, const char* ssl_capath, const char* ssl_cert,
      const char* ssl_cipher, const char* ssl_crl, const char* ssl_crlpath));
  MOCK_METHOD2(activate_tls, bool(Connection_vio &conn, int handshake_timeout));
  MOCK_METHOD0(options, IOptions_context_ptr());
  MOCK_METHOD0(has_ssl, bool());
};


class Mock_connection : public ngs::Connection_vio
{
public:
  Mock_connection()
  : ngs::Connection_vio(m_context, std::unique_ptr<Mock_vio>(new Mock_vio()))
  {
  }

  Mock_connection(Mock_vio *mock_vio)
  : ngs::Connection_vio(m_context, std::unique_ptr<Mock_vio>(mock_vio)),
    m_mock_vio(mock_vio)
  {
  }

  MOCK_METHOD0(connection_type, Connection_type ());
  MOCK_METHOD0(options, ngs::IOptions_session_ptr());
  MOCK_METHOD0(mark_idle, void ());
  MOCK_METHOD0(mark_active, void ());
  MOCK_METHOD0(set_socket_thread_owner, void ());
  MOCK_METHOD2(read, ssize_t(uchar* buffer, ssize_t bytes_to_send));
  MOCK_METHOD2(write, ssize_t(const uchar* buffer, ssize_t bytes_to_send));

  Mock_vio *m_mock_vio;
private:
  Mock_ssl_context m_context;
};


class Mock_scheduler_dynamic : public Scheduler_dynamic
{
public:
  Mock_scheduler_dynamic()
  : Scheduler_dynamic("")
  {
  }

  MOCK_METHOD0(launch, void());
  MOCK_METHOD0(stop, void());
  MOCK_METHOD0(thread_init, bool ());
  MOCK_METHOD0(thread_end, void ());
  MOCK_METHOD1(set_num_workers, unsigned int(unsigned int n));
};


class Mock_server : public ngs::Server_interface
{
public:


  Authentication_interface_ptr get_auth_handler(const std::string &p1, Session_interface *p2)
  {
    return Authentication_interface_ptr(get_auth_handler2(p1, p2));
  }

  MOCK_METHOD2(get_auth_handler2, Authentication_interface_ptr::element_type *(const std::string &, Session_interface *));
  MOCK_CONST_METHOD0(get_config, ngs::shared_ptr<Protocol_config> ());
  MOCK_METHOD0(is_running, bool ());
  MOCK_CONST_METHOD0(get_worker_scheduler, ngs::shared_ptr<Scheduler_dynamic> ());
  MOCK_CONST_METHOD0(ssl_context, Ssl_context *());
  MOCK_METHOD1(on_client_closed, void (const Client_interface &));
  MOCK_METHOD3(create_session, ngs::shared_ptr<Session_interface> (Client_interface &, Protocol_encoder_interface &, const int));
  MOCK_METHOD0(get_client_exit_mutex, Mutex &());
  MOCK_METHOD0(restart_client_supervision_timer, void ());

  // Workaround for GMOCK undefined behaviour with ResultHolder
  MOCK_METHOD2(get_authentication_mechanisms_void, bool (std::vector<std::string> &auth_mech, Client_interface &client));
  void get_authentication_mechanisms(std::vector<std::string> &auth_mech, Client_interface &client)
  {
    get_authentication_mechanisms_void(auth_mech, client);
  }
};

class Mock_authentication_interface : public ngs::Authentication_interface
{
 public:
  MOCK_METHOD3(handle_start, Response(const std::string &, const std::string &,
                                      const std::string &));

  MOCK_METHOD1(handle_continue, Response(const std::string &));

  MOCK_CONST_METHOD3(authenticate_account,
                     ngs::Error_code(const std::string &, const std::string &,
                                     const std::string &));
};

class Mock_account_verification
    : public ngs::Account_verification_interface {
 public:
  MOCK_CONST_METHOD0(get_salt, const std::string&());
  MOCK_CONST_METHOD4(verify_authentication_string,
                     bool(const std::string &, const std::string &,
                          const std::string &, const std::string &));
};

class Mock_sql_data_context : public ngs::Sql_session_interface {
 public:
  MOCK_METHOD1(set_connection_type, Error_code(const Connection_type));
  MOCK_METHOD1(execute_kill_sql_session, Error_code(uint64_t));
  MOCK_CONST_METHOD0(is_killed, bool());
  MOCK_CONST_METHOD0(password_expired, bool());
  MOCK_METHOD0(proto, Protocol_encoder &());
  MOCK_CONST_METHOD0(get_authenticated_user_name, std::string());
  MOCK_CONST_METHOD0(get_authenticated_user_host, std::string());
  MOCK_CONST_METHOD0(has_authenticated_user_a_super_priv, bool());
  MOCK_CONST_METHOD0(mysql_session_id, uint64_t());
  MOCK_METHOD7(authenticate,
               Error_code(const char *, const char *, const char *,
                          const char *, const std::string &,
                          const Authentication_interface &, bool));
  MOCK_METHOD3(execute,
               Error_code(const char *, std::size_t, Resultset_interface *));
  MOCK_METHOD0(attach, Error_code ());
  MOCK_METHOD0(detach, Error_code ());
};

class Mock_protocol_encoder : public ngs::Protocol_encoder_interface {
 public:
  MOCK_METHOD1(send_result, bool(const Error_code &));
  MOCK_METHOD0(send_ok, bool());
  MOCK_METHOD1(send_ok,  bool(const std::string &));
  MOCK_METHOD1(send_init_error, bool(const Error_code &));
  MOCK_METHOD4(send_notice, void(const Frame_type, const Frame_scope,
                                 const std::string &, const bool));
  MOCK_METHOD1(send_rows_affected, void(uint64_t value));
  MOCK_METHOD2(send_local_warning, void(const std::string &, bool));
  MOCK_METHOD1(send_auth_ok, void(const std::string &));
  MOCK_METHOD1(send_auth_continue, void(const std::string &));
  MOCK_METHOD0(send_exec_ok, bool());
  MOCK_METHOD0(send_result_fetch_done, bool());
  MOCK_METHOD0(send_result_fetch_done_more_results, bool());

  MOCK_METHOD1(send_column_metadata ,
               bool (const ::ngs::Encode_column_info *column_info));

  MOCK_METHOD6(send_column_metadata,
               bool(uint64_t, int, int, uint32_t, uint32_t, uint32_t));
  MOCK_METHOD0(row_builder, Row_builder &());
  MOCK_METHOD0(start_row, void());
  MOCK_METHOD0(abort_row, void());
  MOCK_METHOD0(send_row, bool());
  MOCK_METHOD0(get_buffer, Output_buffer *());
  MOCK_METHOD3(send_message, bool(int8_t, const Message &, bool));
  MOCK_METHOD1(on_error, void(int error));
  MOCK_METHOD0(get_protocol_monitor, Protocol_monitor_interface &());
  MOCK_METHOD1(set_write_timeout, void(unsigned int));
};

class Mock_session : public Session_interface
{
public:
  MOCK_CONST_METHOD0(session_id, Session_id ());
  MOCK_METHOD0(init, Error_code ());
  MOCK_METHOD1(on_close, void (const bool));
  MOCK_METHOD0(on_kill, void ());
  MOCK_METHOD1(on_auth_success, void (const Authentication_interface::Response &));
  MOCK_METHOD1(on_auth_failure, void (const Authentication_interface::Response &));
  MOCK_METHOD1(handle_message, bool (Request &));
  MOCK_CONST_METHOD0(state, State ());
  MOCK_CONST_METHOD0(state_before_close, State ());
  MOCK_METHOD0(client, Client_interface &());
  MOCK_METHOD0(mark_as_tls_session, void ());
  MOCK_CONST_METHOD0(get_thd, THD* ());
  MOCK_METHOD0(data_context, Sql_session_interface &());
  MOCK_METHOD0(proto, Protocol_encoder_interface &());
};

class Mock_protocol_monitor : public ngs::Protocol_monitor_interface
{
public:
  MOCK_METHOD0(on_notice_warning_send, void());
  MOCK_METHOD0(on_notice_other_send, void());
  MOCK_METHOD0(on_fatal_error_send, void());
  MOCK_METHOD0(on_init_error_send, void());
  MOCK_METHOD0(on_row_send, void());
  MOCK_METHOD1(on_send, void (long));
  MOCK_METHOD1(on_receive, void (long));
  MOCK_METHOD0(on_error_send, void());
  MOCK_METHOD0(on_error_unknown_msg_type, void());
};

}  // namespace test
}  // namespace ngs


namespace xpl
{
namespace test
{

class Mock_ngs_client : public ngs::Client
{
public:
  using ngs::Client::Client;
  using ngs::Client::read_one_message;
  using ngs::Client::set_encoder;
  MOCK_METHOD0(resolve_hostname, std::string ());
  MOCK_CONST_METHOD0(is_interactive, bool ());
  MOCK_METHOD1(set_is_interactive, void (const bool ));
};

class Mock_client : public ngs::Client_interface
{
public:
  MOCK_METHOD0(get_session_exit_mutex, ngs::Mutex &());

  MOCK_CONST_METHOD0(client_id, const char *());

  MOCK_CONST_METHOD0(client_address, const char *());
  MOCK_CONST_METHOD0(client_hostname, const char *());
  MOCK_METHOD0(connection, ngs::Connection_vio  &());
  MOCK_CONST_METHOD0(server, ngs::Server_interface &());
  MOCK_CONST_METHOD0(protocol, ngs::Protocol_encoder_interface &());

  MOCK_CONST_METHOD0(client_id_num, Client_id ());
  MOCK_CONST_METHOD0(client_port, int ());

  MOCK_CONST_METHOD0(get_accept_time, ngs::chrono::time_point ());
  MOCK_CONST_METHOD0(get_state, Client_state ());

  MOCK_METHOD0(session, ngs::shared_ptr<ngs::Session_interface> ());
  MOCK_CONST_METHOD0(supports_expired_passwords, bool ());

  MOCK_CONST_METHOD0(is_interactive, bool ());
  MOCK_METHOD1(set_is_interactive, void (bool));

  MOCK_METHOD1(set_wait_timeout, void (const unsigned int));
  MOCK_METHOD1(set_interactive_timeout, void (const unsigned int));
  MOCK_METHOD1(set_read_timeout, void (const unsigned int));
  MOCK_METHOD1(set_write_timeout, void (const unsigned int));

public:
  MOCK_METHOD1(on_session_reset_void, bool (ngs::Session_interface &));
  MOCK_METHOD1(on_session_close_void, bool (ngs::Session_interface &));
  MOCK_METHOD1(on_session_auth_success_void, bool (ngs::Session_interface &));

  MOCK_METHOD0(disconnect_and_trigger_close_void, bool ());
  MOCK_METHOD0(activate_tls_void, bool ());
  MOCK_METHOD0(on_auth_timeout_void, bool ());
  MOCK_METHOD0(on_server_shutdown_void, bool ());
  MOCK_METHOD1(run_void, bool (bool));
  MOCK_METHOD0(reset_accept_time_void, bool ());

  void on_session_reset(ngs::Session_interface &arg)
  {
    on_session_reset_void(arg);
  }

  void on_session_close(ngs::Session_interface &arg)
  {
    on_session_close_void(arg);
  }

  void on_session_auth_success(ngs::Session_interface &arg)
  {
    on_session_auth_success_void(arg);
  }

  void disconnect_and_trigger_close()
  {
    disconnect_and_trigger_close_void();
  }

  void activate_tls()
  {
    activate_tls_void();
  }

  void on_auth_timeout()
  {
    on_auth_timeout_void();
  }

  void on_server_shutdown()
  {
    on_server_shutdown_void();
  }

  void run(bool arg)
  {
    run_void(arg);
  }

  void reset_accept_time()
  {
    reset_accept_time_void();
  }
};

class Mock_account_verification_handler
    : public xpl::Account_verification_handler {
 public:
  Mock_account_verification_handler(xpl::Session *session)
      : xpl::Account_verification_handler(session) {}

  MOCK_CONST_METHOD2(authenticate,
                     ngs::Error_code(const ngs::Authentication_interface &,
                                     const std::string &));
  MOCK_CONST_METHOD1(
      get_account_verificator,
      const ngs::Account_verification_interface *(
          const ngs::Account_verification_interface::Account_type));
};

} // namespace test
}  // namespace xpl
