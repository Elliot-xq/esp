/*
 * Copyright (C) Endpoints Server Proxy Authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#ifndef NGINX_NGX_ESP_MODULE_H_
#define NGINX_NGX_ESP_MODULE_H_

#include <map>
#include <memory>
#include <string>

extern "C" {
#include "third_party/nginx/src/http/ngx_http.h"
}

#include <grpc++/generic/generic_stub.h>
#include <grpc++/grpc++.h>

#include "include/api_manager/api_manager.h"
#include "include/api_manager/utils/status.h"
#include "src/nginx/alloc.h"
#include "src/nginx/grpc.h"
#include "src/nginx/grpc_queue.h"
#include "src/nginx/grpc_server_call.h"
#include "src/nginx/request.h"

namespace google {
namespace api_manager {
namespace nginx {

// ********************************************************
// * Endpoints Server Proxy - Configuration declarations. *
// ********************************************************

//
// ESP Module Configuration - main context.
//
typedef struct {
  // Address of the Google Compute Engine metadata server.
  // Used to override metadata server address for testing.
  // Defaults to "http://169.254.169.254".
  ngx_str_t metadata_server;

  // Array of all endpoints loaded (array of ngx_esp_loc_conf_t*).
  // Endpoints API management is enabled if endpoints.nelts > 0.
  ngx_array_t endpoints;

  // remote_addr variable index. NGX_ERROR if not_found.
  ngx_int_t remote_addr_variable_index;

  // The module-level Esp factory.
  ApiManagerFactory esp_factory;

  // The module-level GRPC library interface.
  std::shared_ptr<NgxEspGrpcQueue> grpc_queue;

  // Shared memory zone for stats per process
  ngx_shm_zone_t *stats_zone;

  // Timer to update process stats
  std::unique_ptr<PeriodicTimer> stats_timer;

  // A timer event to detect worker process existing.
  ngx_event_t exit_timer;
  // the start time to wait for active connections to be closed.
  time_t exit_wait_start_time;
  // If true, ESP::Close has been called.
  bool esp_closed;

  // Absolution path to the trusted CA certificates. If not empty, all outgoing
  // HTTPS requests will require server to provide a valid
  // certificate. Self-signed server certificate will not be accepted.
  ngx_str_t cert_path;

  // HTTP module configuration context pointers used for the HTTP implementation
  // based on NGINX upstream module. Only used in the HTTP subrequest path.
  ngx_http_conf_ctx_t http_module_conf_ctx;

#if NGX_HTTP_SSL
  // SSL for subrequests.
  ngx_ssl_t *ssl;
#endif

} ngx_esp_main_conf_t;

//
// ESP Module Configuration - server context.
//
typedef struct ngx_esp_srv_conf_s ngx_esp_srv_conf_t;
struct ngx_esp_srv_conf_s {
  ngx_esp_srv_conf_s() : esp_main_conf(nullptr) {}

  // Pointer to the main context configuration.
  ngx_esp_main_conf_t *esp_main_conf;
};

typedef std::map<std::string, std::shared_ptr<::grpc::GenericStub>>
    ngx_esp_grpc_stub_map_t;

//
// ESP Module Configuration - location context.
//
typedef struct {
  // Core module configuration - used to access logs.
  ngx_http_core_loc_conf_t *http_core_loc_conf;

  // Parent context pointer.
  ngx_esp_srv_conf_t *esp_srv_conf;

  ngx_str_t endpoints_config;  // API Configuration file name.
  ngx_flag_t endpoints_api;    // Does this location host an Endpoints API?

  // Endpoints Server Proxy library interface.
  std::shared_ptr<ApiManager> esp;

  unsigned endpoints_block : 1;  // location has `endpoints` block
  unsigned grpc_pass_block : 1;  // location has `grpc_pass` block

  // TODO: Move the following into the Endpoints config protocol buffer.
  ngx_str_t endpoints_servicecontrol_secret;

  // Server config
  ngx_str_t endpoints_server_config;

  // The map of backends to GRPC stubs.  These are constructed
  // on-demand.
  ngx_esp_grpc_stub_map_t grpc_stubs;

  // The GRPC proxy passthrough prefix, used for sending non-API
  // traffic to nginx's HTTP request handler chain.
  ngx_str_t grpc_passthrough_prefix;

  // The GRPC backend address override.  If this is a non-zero-length
  // string, this is where all GRPC API traffic will be sent,
  // regardless of the contents of the service config.
  ngx_str_t grpc_backend_address_override;

  // The GRPC backend address fallback.  If this is a non-zero-length
  // string, this is where GRPC API traffic will be sent if
  // grpc_backend_address_override is not specified and there is no
  // configured backend address for the API method in the API service
  // configuration.
  ngx_str_t grpc_backend_address_fallback;
} ngx_esp_loc_conf_t;

// **************************************************
// * Endpoints Server Proxy - Runtime declarations. *
// **************************************************

typedef struct ngx_esp_request_ctx_s ngx_esp_request_ctx_t;
typedef utils::Status (*ngx_http_esp_access_handler_pt)(
    ngx_http_request_t *r, ngx_esp_request_ctx_t *ctx);

typedef struct wakeup_context_s wakeup_context_t;

//
// Runtime state of the ESP module - per-request module context.
//
struct ngx_esp_request_ctx_s {
  // Constructor, destructor.
  ngx_esp_request_ctx_s(ngx_http_request_t *r, ngx_esp_loc_conf_t *lc);
  ~ngx_esp_request_ctx_s();

  // Function pointer to the current handler in the access state machine. The
  // state machine transitions are:
  //  - initialize module data
  //  - call service control
  //  - act on the result of the service control call
  ngx_http_esp_access_handler_pt current_access_handler;

  // An event pre-allocated for the wakeup of the client request after
  // the service control continuation completes. Because the api manager
  // module doesn't use NGINX subrequests, the parent request wakeup
  // is not automatic and we do it explicitly.
  ngx_event_t wakeup_event;

  // The wakeup context pointer is shared between the request context (here)
  // and the continuation which is going to wake this request up after
  // Check call completes. If, however, the parent request goes out of scope
  // before Check call calls the continuation, it would end up waking up
  // deallocated request.
  // Therefore, ir the destructor of ngx_esp_request_ctx_s we mark the
  // wakeup context as "wake up cancelled" and the continuation will
  // not attempt the wakeup.
  std::shared_ptr<wakeup_context_t> wakeup_context;

  // Endpoints variables.
  ngx_str_t endpoints_api_userinfo;  // base64 encoded Json of user info.

  // Result of the check call.
  ::google::api_manager::utils::Status check_status;

  // Auth token from incoming request.
  ngx_str_t auth_token;

  // GRPC Proxying support.
  NgxEspGrpcServerCall *grpc_server_call;

  // RequestHandlerInterface object
  std::unique_ptr<RequestHandlerInterface> request_handler;

  // The backend request time in milliseconds. -1 if not available.
  int64_t backend_time;
};

static_assert(std::is_standard_layout<ngx_esp_request_ctx_t>::value,
              "ngx_esp_request_ctx_t must be a standard layout type");

// Get or create the ESP per-request context.
ngx_esp_request_ctx_t *ngx_http_esp_ensure_module_ctx(ngx_http_request_t *r);

// The currently active request context.
// TODO: Make this thread-local, just for paranoia.
extern ngx_esp_request_ctx_t *ngx_esp_current_request_context;

}  // namespace nginx
}  // namespace api_manager
}  // namespace google

extern ngx_module_t ngx_esp_module;

#endif  // NGINX_NGX_ESP_MODULE_H_