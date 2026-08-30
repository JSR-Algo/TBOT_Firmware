from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function {signature}")


def test_esp_ml307_is_repo_local_override():
    manifest = read("main/idf_component.yml")
    assert "78/esp-ml307:" in manifest
    dependency = manifest[manifest.index("78/esp-ml307:") :]
    assert "override_path: ../components/esp-ml307" in dependency[:200]
    assert (ROOT / "components/esp-ml307/CMakeLists.txt").is_file()
    gitignore = read(".gitignore")
    assert "!/components/esp-ml307/" in gitignore
    assert "!/components/esp-ml307/**" in gitignore


def test_esp_tcp_shutdown_joins_or_aborts_before_sync_delete():
    source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    header = read("components/esp-ml307/src/esp/esp_tcp.h")
    disconnect = function_body(source, "std::function<void()> EspTcp::DoDisconnect")
    destructor = function_body(source, "EspTcp::~EspTcp")
    connect = function_body(source, "bool EspTcp::Connect")

    assert "EspTcpShutdownState shutdown_state_;" in header
    assert "shutdown(fd, SHUT_RDWR);" in disconnect
    assert disconnect.index("shutdown(fd, SHUT_RDWR);") < disconnect.index("close(fd);")
    assert disconnect.index("shutdown_state_.TaskJoined();") < disconnect.index("close(fd);")
    assert "kReceiveTaskJoinTimeoutMs" in disconnect
    assert "pdMS_TO_TICKS(kReceiveTaskJoinTimeoutMs)" in disconnect
    assert "TryForceStop" not in disconnect
    assert "vTaskDelete(task)" not in disconnect
    assert "abort();" in disconnect
    assert "shutdown_state_.TaskJoined();" in disconnect
    assert "shutdown_state_.NeedsJoin()" in disconnect

    assert "shutdown_state_.CanDeleteSynchronization()" in destructor
    assert destructor.index("shutdown_state_.CanDeleteSynchronization()") < destructor.index(
        "vEventGroupDelete(event_group_)"
    )
    assert "shutdown_state_.TaskStarted();" in connect
    assert connect.index("shutdown_state_.TaskStarted();") < connect.index("xTaskCreateWithCaps")

    wrapper_start = connect.index("BaseType_t created = xTaskCreateWithCaps")
    wrapper_end = connect.index('}, "tcp_receive"', wrapper_start)
    wrapper = connect[wrapper_start:wrapper_end]
    assert "EventGroupHandle_t event_group = tcp->event_group_;" in wrapper
    assert "tcp->shutdown_state_.TaskExited();" in wrapper
    assert "tcp->shutdown_state_.TaskWillExit()" in wrapper
    assert "tcp->receive_task_handle_.store(nullptr);" in wrapper
    assert "xEventGroupSetBits(event_group" in wrapper
    signal_end = wrapper.index("xEventGroupSetBits(event_group")
    assert "tcp->" not in wrapper[signal_end:]
    assert wrapper.index("disconnect_callback();") < wrapper.index(
        "tcp->shutdown_state_.TaskWillExit()"
    )
    assert wrapper.index("tcp->receive_task_handle_.store(nullptr);") < signal_end
    assert wrapper.index("tcp->shutdown_state_.TaskExited();") < signal_end


def test_esp_tcp_connect_task_creation_failure_closes_transport():
    source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    connect = function_body(source, "bool EspTcp::Connect")
    assert "BaseType_t created = xTaskCreateWithCaps" in connect
    assert "if (created != pdPASS)" in connect
    failure = connect[connect.index("if (created != pdPASS)") :]
    assert "shutdown_state_.TaskWillExit()" in failure
    assert "shutdown_state_.TaskExited();" in failure
    assert "shutdown_state_.TaskJoined();" in failure
    assert "shutdown(fd, SHUT_RDWR);" in failure
    assert failure.index("shutdown(fd, SHUT_RDWR);") < failure.index("close(fd);")
    assert "tcp_fd_.store(-1);" in failure
    assert "connected_ = false;" in failure
    assert "receive_task_handle_.store(nullptr);" in failure
    assert "return false;" in failure


def test_esp_tcp_disconnect_is_bounded_and_callback_is_exactly_once():
    source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    header = read("components/esp-ml307/src/esp/esp_tcp.h")
    disconnect = function_body(source, "std::function<void()> EspTcp::DoDisconnect")
    connect = function_body(source, "bool EspTcp::Connect")
    receive = function_body(source, "std::function<void()> EspTcp::ReceiveTask")

    timeout_match = re.search(r"kReceiveTaskJoinTimeoutMs\s*=\s*(\d+)", header)
    assert timeout_match is not None
    assert int(timeout_match.group(1)) <= 1000
    assert "portMAX_DELAY" not in disconnect
    assert "abort();" in disconnect
    assert "if (!connected_)" not in function_body(source, "void EspTcp::Disconnect")
    assert connect.index("DoDisconnect(true)") < connect.index("socket(")
    assert "disconnect_notified_.exchange(true)" in disconnect
    assert "disconnect_notified_.exchange(true)" in receive
    assert "stop_requested_.load()" in receive
    assert "tcp_fd_.exchange(-1)" in disconnect


def test_esp_tcp_retains_fd_until_join_and_serializes_send():
    source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    header = read("components/esp-ml307/src/esp/esp_tcp.h")
    disconnect = function_body(source, "std::function<void()> EspTcp::DoDisconnect")
    send = function_body(source, "int EspTcp::Send")
    receive = function_body(source, "std::function<void()> EspTcp::ReceiveTask")

    assert "std::mutex send_mutex_;" in header
    assert "std::lock_guard<std::mutex> send_lock(send_mutex_);" in send
    assert disconnect.index("shutdown_state_.TaskJoined();") < disconnect.index(
        "tcp_fd_.exchange(-1)"
    )
    assert "close(owned_fd)" not in receive
    assert "tcp_fd_.exchange(-1)" not in receive


def test_esp_tcp_serializes_connect_and_disconnect_task_publication():
    source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    header = read("components/esp-ml307/src/esp/esp_tcp.h")
    connect = function_body(source, "bool EspTcp::Connect")
    disconnect = function_body(source, "void EspTcp::Disconnect")

    assert "std::mutex lifecycle_mutex_;" in header
    assert "std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);" in connect
    assert "std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);" in disconnect
    assert "lifecycle_lock.unlock();" in disconnect
    callback_call = disconnect.index("disconnect_callback();")
    assert disconnect.index("lifecycle_lock.unlock();") < callback_call


def test_websocket_installs_callbacks_before_transport_can_receive():
    source = read("components/esp-ml307/src/web_socket.cc")
    connect = function_body(source, "bool WebSocket::Connect")
    destructor = function_body(source, "WebSocket::~WebSocket")

    transport_connect = connect.index("tcp_->Connect")
    assert connect.index("xEventGroupClearBits") < transport_connect
    assert connect.index("tcp_->OnStream") < transport_connect
    assert connect.index("tcp_->OnDisconnected") < transport_connect
    assert "if (tcp_)" in destructor
    assert destructor.index("tcp_->Disconnect();") < destructor.index(
        "vEventGroupDelete(handshake_event_group_)"
    )


def test_production_wss_uses_esp_ssl_with_joined_shutdown():
    network = read("components/esp-ml307/src/esp/esp_network.cc")
    source = read("components/esp-ml307/src/esp/esp_ssl.cc")
    header = read("components/esp-ml307/src/esp/esp_ssl.h")
    connect = function_body(source, "bool EspSsl::Connect")
    disconnect = function_body(source, "std::function<void()> EspSsl::DoDisconnect")
    destructor = function_body(source, "EspSsl::~EspSsl")

    assert "return std::make_unique<EspSsl>();" in function_body(
        network, "std::unique_ptr<Tcp> EspNetwork::CreateSsl"
    )
    assert "EspSslShutdownState shutdown_state_;" in header
    assert "std::mutex lifecycle_mutex_;" in header
    assert "std::atomic<bool> stop_requested_" in header
    assert "std::atomic<bool> disconnect_notified_" in header
    assert "BaseType_t created = xTaskCreateWithCaps" in connect
    assert connect.index("shutdown_state_.TaskStarted();") < connect.index("xTaskCreateWithCaps")
    assert "if (created != pdPASS)" in connect
    assert "shutdown(sockfd, SHUT_RDWR);" in disconnect
    assert "close(sockfd);" not in disconnect
    assert "pdMS_TO_TICKS(kReceiveTaskJoinTimeoutMs)" in disconnect
    assert "vTaskDelete(task)" not in disconnect
    assert "abort();" in disconnect
    assert disconnect.index("shutdown_state_.TaskJoined();") < disconnect.index(
        "esp_tls_conn_destroy(tls_client_)"
    )
    assert "shutdown_state_.CanDeleteSynchronization()" in destructor
    assert destructor.index("shutdown_state_.CanDeleteSynchronization()") < destructor.index(
        "vEventGroupDelete(event_group_)"
    )


def test_http_timeout_is_propagated_to_each_new_tcp_transport_before_connect():
    tcp = read("components/esp-ml307/include/tcp.h")
    http = read("components/esp-ml307/src/http_client.cc")
    open_body = function_body(http, "bool HttpClient::Open")

    assert "virtual void SetTimeout(int timeout_ms)" in tcp
    assert "(void)timeout_ms;" in tcp
    timeout_idx = open_body.index("tcp_->SetTimeout(remaining_ms);")
    connect_idx = open_body.index("tcp_->Connect(host_, port_)")
    assert timeout_idx < connect_idx
    assert "open_deadline" in open_body
    assert open_body.count("RemainingTransportTimeoutMs") >= 2
    assert open_body.index("tcp_->SetTimeout", connect_idx) > connect_idx


def test_esp_tcp_connect_uses_nonblocking_deadline_and_restores_socket_flags():
    source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    header = read("components/esp-ml307/src/esp/esp_tcp.h")
    connect = function_body(source, "bool EspTcp::Connect")

    assert "void SetTimeout(int timeout_ms) override;" in header
    assert "int timeout_ms_" in header
    assert "fcntl(fd, F_GETFL" in connect
    assert "fcntl(fd, F_SETFL, original_flags | O_NONBLOCK)" in connect
    assert "select(fd + 1" in connect
    assert "getsockopt(fd, SOL_SOCKET, SO_ERROR" in connect
    assert "last_error_ = ETIMEDOUT" in connect
    assert "fcntl(fd, F_SETFL, original_flags)" in connect
    assert "ResolveHostIpv4WithDeadline" in connect
    assert "gethostbyname(" not in connect
    assert "RemainingTransportTimeoutMs" in connect


def test_esp_tcp_and_ssl_send_paths_have_absolute_write_deadlines():
    tcp_source = read("components/esp-ml307/src/esp/esp_tcp.cc")
    ssl_source = read("components/esp-ml307/src/esp/esp_ssl.cc")
    tcp_send = function_body(tcp_source, "int EspTcp::Send")
    ssl_send = function_body(ssl_source, "int EspSsl::Send")

    for send_body in (tcp_send, ssl_send):
        assert "TransportDeadlineUs" in send_body
        assert "RemainingTransportTimeoutMs" in send_body
        assert "SO_SNDTIMEO" in send_body
        assert "last_error_ = ETIMEDOUT" in send_body

    want_write = ssl_send.index("ESP_TLS_ERR_SSL_WANT_WRITE")
    assert "continue;" in ssl_send[want_write:]
    assert "vTaskDelay(1);" in ssl_send[want_write:]
    assert ssl_send.index("RemainingTransportTimeoutMs") < want_write


def test_esp_dns_deadline_callback_owns_state_after_caller_timeout():
    resolver = read("components/esp-ml307/src/esp/esp_dns_resolver.h")

    assert "tcpip_try_callback" in resolver
    assert "dns_gethostbyname_addrtype" in resolver
    assert "AsyncLookupLifecycle" in resolver
    assert "kMaxConcurrentDnsLookups = 8" in resolver
    assert "EncodeDnsCallbackToken" in resolver
    assert "generation" in resolver
    assert "ETIMEDOUT" in resolver
    assert "gethostbyname(" not in resolver
    assert "getaddrinfo" not in resolver
    assert "xTaskCreate" not in resolver
    assert "g_dns_lookup_in_flight" not in resolver
    assert "inet_aton(" in resolver
    assert "EAGAIN" in resolver


def test_esp_ssl_connect_uses_http_timeout_for_tls_handshake():
    source = read("components/esp-ml307/src/esp/esp_ssl.cc")
    header = read("components/esp-ml307/src/esp/esp_ssl.h")
    connect = function_body(source, "bool EspSsl::Connect")

    assert "void SetTimeout(int timeout_ms) override;" in header
    assert "int timeout_ms_" in header
    assert "cfg.timeout_ms" in connect
    assert "RemainingTransportTimeoutMs" in connect
    assert "ResolveHostIpv4WithDeadline" in connect
    assert "inet_ntop(AF_INET" in connect
    assert "cfg.common_name = host.c_str();" in connect
    assert "cfg.skip_common_name = false;" in connect
    assert "esp_tls_conn_new_sync(resolved_host" in connect
    assert "esp_tls_conn_new_sync(host.c_str()" not in connect


def test_esp_ssl_callback_completes_before_exit_publication():
    source = read("components/esp-ml307/src/esp/esp_ssl.cc")
    connect = function_body(source, "bool EspSsl::Connect")
    wrapper_start = connect.index("BaseType_t created = xTaskCreateWithCaps")
    wrapper_end = connect.index('}, "ssl_receive"', wrapper_start)
    wrapper = connect[wrapper_start:wrapper_end]

    assert wrapper.index("disconnect_callback();") < wrapper.index(
        "ssl->shutdown_state_.TaskWillExit()"
    )
    signal = wrapper.index("xEventGroupSetBits(event_group")
    assert wrapper.index("ssl->shutdown_state_.TaskExited();") < signal
    assert "ssl->" not in wrapper[signal:]


def test_esp_ssl_send_does_not_block_receive_exit_on_lifecycle_lock():
    source = read("components/esp-ml307/src/esp/esp_ssl.cc")
    header = read("components/esp-ml307/src/esp/esp_ssl.h")
    send = function_body(source, "int EspSsl::Send")

    assert "std::mutex send_mutex_;" in header
    assert "std::lock_guard<std::mutex> send_lock(send_mutex_);" in send
    assert "lifecycle_mutex_" not in send


def test_protocol_releases_gate_before_websocket_join_and_keeps_callbacks_stable():
    protocol = read("main/protocols/websocket_protocol.cc")
    close = function_body(protocol, "void WebsocketProtocol::CompleteCloseAndNotify")
    deferred = function_body(protocol, "void WebsocketProtocol::CompleteDeferredClose")
    detach = function_body(protocol, "void WebsocketProtocol::DetachAndResetWebsocket")
    destructor = function_body(protocol, "WebsocketProtocol::~WebsocketProtocol")

    assert "}\n    DetachAndResetWebsocket();" in close
    assert "}\n    DetachAndResetWebsocket();" in deferred
    assert "}\n    DetachAndResetWebsocket();" in destructor
    assert "OnData(nullptr)" not in detach
    assert "OnDisconnected(nullptr)" not in detach


def test_websocket_handshake_disconnect_is_atomic_and_fails_fast():
    source = read("components/esp-ml307/src/web_socket.cc")
    header = read("components/esp-ml307/include/web_socket.h")
    connect = function_body(source, "bool WebSocket::Connect")
    disconnected = function_body(source, "void WebSocket::HandleTransportDisconnected")

    assert "std::atomic<ConnectionState> connection_state_" in header
    assert "std::atomic<bool> handshake_completed_" in header
    assert "connection_state_.exchange(ConnectionState::kDisconnected)" in disconnected
    assert "HANDSHAKE_FAILED_BIT" in disconnected
    assert "compare_exchange_strong" in connect
    assert "bool connected_" not in header
