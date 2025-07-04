/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "proxy/http2/Http2SessionAccept.h"
#include "proxy/http2/Http2ClientSession.h"
#include "iocore/utils/Machine.h"
#include "proxy/IPAllow.h"

namespace
{
DbgCtl dbg_ctl_http2_seq{"http2_seq"};

} // end anonymous namespace

Http2SessionAccept::Http2SessionAccept(const HttpSessionAccept::Options &_o) : SessionAccept(nullptr), options(_o)
{
  SET_HANDLER(&Http2SessionAccept::mainEvent);
}

Http2SessionAccept::~Http2SessionAccept() = default;

bool
Http2SessionAccept::accept(NetVConnection *netvc, MIOBuffer *iobuf, IOBufferReader *reader)
{
  sockaddr const *client_ip = nullptr;

  for (int i = 0; i < IpAllow::Subject::MAX_SUBJECTS; ++i) {
    if (IpAllow::Subject::PEER == IpAllow::subjects[i]) {
      client_ip = netvc->get_remote_addr();
      break;
    } else if (IpAllow::Subject::PROXY == IpAllow::subjects[i] &&
               netvc->get_proxy_protocol_version() != ProxyProtocolVersion::UNDEFINED) {
      client_ip = netvc->get_proxy_protocol_src_addr();
      break;
    }
  }

  IpAllow::ACL session_acl = IpAllow::match(client_ip, IpAllow::match_key_t::SRC_ADDR);
  if (!session_acl.isValid()) {
    ip_port_text_buffer ipb;
    Warning("HTTP/2 client '%s' prohibited by ip-allow policy", ats_ip_ntop(client_ip, ipb, sizeof(ipb)));
    return false;
  }

  netvc->attributes = this->options.transport_type;

  if (dbg_ctl_http2_seq.on()) {
    ip_port_text_buffer ipb;

    DbgPrint(dbg_ctl_http2_seq, "[HttpSessionAccept2:mainEvent %p] accepted connection from %s transport type = %d", netvc,
             ats_ip_nptop(client_ip, ipb, sizeof(ipb)), netvc->attributes);
  }

  Http2ClientSession *new_session = THREAD_ALLOC_INIT(http2ClientSessionAllocator, this_ethread());
  new_session->acl                = std::move(session_acl);
  new_session->accept_options     = &options;

  // Pin session to current ET_NET thread
  new_session->setThreadAffinity(this_ethread());
  new_session->new_connection(netvc, iobuf, reader);

  return true;
}

int
Http2SessionAccept::mainEvent(int event, void *data)
{
  ink_release_assert(event == NET_EVENT_ACCEPT || event == EVENT_ERROR);
  ink_release_assert((event == NET_EVENT_ACCEPT) ? (data != nullptr) : (1));

  if (event == NET_EVENT_ACCEPT) {
    NetVConnection *netvc = static_cast<NetVConnection *>(data);
    if (!this->accept(netvc, nullptr, nullptr)) {
      netvc->do_io_close();
    }
    return EVENT_CONT;
  }

  // XXX We should hoist the error handling so that all the protocols generate the statistics
  // without code duplication.
  if (((long)data) == -ECONNABORTED) {
    Metrics::Counter::increment(http_rsb.ua_counts_errors_pre_accept_hangups);
    // Metrics::Counter::increment(http_rsb.ua_msecs_errors_pre_accept_hangups, 0); // ToDo: This is odd, but we added 0 before as
    // well
  }

  ink_abort("HTTP/2 accept received fatal error: errno = %d", -(static_cast<int>((intptr_t)data)));
  return EVENT_CONT;
}
