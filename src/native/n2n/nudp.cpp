#include "nudp.h"
#include "../third_party/libutp/utp.h"
#include "../third_party/crc32/crc32.h"
#include "nat.h"
#include "../util/buf.h"

namespace noobaa {

DBG_INIT(0);

Nan::Persistent<v8::Function> Nudp::_ctor;

static const int UTP_TARGET_DELAY_MICROS = 1000;
static const int UTP_SNDBUF_SIZE = 1 * 1024 * 1024;
static const int UTP_RCVBUF_SIZE = 1 * 1024 * 1024;

static std::string addrinfo2str(const struct addrinfo* ai);
static std::string sockaddr2str(const struct sockaddr* sa);

NAN_MODULE_INIT(Nudp::setup)
{
    DBG2("Nudp::setup");
    auto name = "Nudp";
    auto tpl = Nan::New<v8::FunctionTemplate>(Nudp::new_instance);
    tpl->SetClassName(NAN_STR(name));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    Nan::SetPrototypeMethod(tpl, "close", Nudp::close);
    Nan::SetPrototypeMethod(tpl, "bind", Nudp::bind);
    Nan::SetPrototypeMethod(tpl, "connect", Nudp::connect);
    Nan::SetPrototypeMethod(tpl, "send", Nudp::send);
    Nan::SetPrototypeMethod(tpl, "send_outbound", Nudp::send_outbound);
    Nan::SetPrototypeMethod(tpl, "stats", Nudp::stats);
    auto func = Nan::GetFunction(tpl).ToLocalChecked();
    _ctor.Reset(func);
    NAN_SET(target, name, func);
}

NAN_METHOD(Nudp::new_instance)
{
    NAN_MAKE_CTOR_CALL(_ctor);
    Nudp* obj = new Nudp();
    obj->Wrap(info.This());
    info.GetReturnValue().Set(info.This());
}

Nudp::Nudp()
    : _utp_socket(NULL)
    , _recv_payload(NULL)
    , _recv_hdr_pos(0)
    , _recv_payload_pos(0)
    , _send_msg_seq(1)
    , _recv_msg_seq(1)
    , _closed(false)
    , _receiving(false)
    , _local_port(0)
{
    DBG2("Nudp::Nudp");
    _utp_ctx = utp_init(2); // version=2
    utp_context_set_userdata(_utp_ctx, this);
    utp_context_set_option(_utp_ctx, UTP_SNDBUF, UTP_SNDBUF_SIZE);
    utp_context_set_option(_utp_ctx, UTP_RCVBUF, UTP_RCVBUF_SIZE);
    utp_set_callback(_utp_ctx, UTP_SENDTO,           &Nudp::utp_callback_sendto);
    utp_set_callback(_utp_ctx, UTP_ON_READ,          &Nudp::utp_callback_on_read);
    utp_set_callback(_utp_ctx, UTP_ON_STATE_CHANGE,  &Nudp::utp_callback_on_state_change);
    utp_set_callback(_utp_ctx, UTP_ON_FIREWALL,      &Nudp::utp_callback_on_firewall);
    utp_set_callback(_utp_ctx, UTP_ON_ACCEPT,        &Nudp::utp_callback_on_accept);
    utp_set_callback(_utp_ctx, UTP_ON_ERROR,         &Nudp::utp_callback_on_error);
    utp_set_callback(_utp_ctx, UTP_LOG,              &Nudp::utp_callback_log);
    if (DBG_VISIBLE(0)) {
        utp_context_set_option(_utp_ctx, UTP_LOG_NORMAL, 1);
    }
    if (DBG_VISIBLE(0)) {
        utp_context_set_option(_utp_ctx, UTP_LOG_MTU,    1);
    }
    if (DBG_VISIBLE(2)) {
        utp_context_set_option(_utp_ctx, UTP_LOG_DEBUG,  1);
    }

    NAUV_CALL(uv_udp_init(uv_default_loop(), &_uv_udp_handle));
    NAUV_CALL(uv_timer_init(uv_default_loop(), &_uv_timer_handle));
    NAUV_CALL(uv_timer_start(&_uv_timer_handle, &Nudp::uv_callback_timer, 0, 100));
    NAUV_CALL(uv_prepare_init(uv_default_loop(), &_uv_prepare_handle));
    NAUV_CALL(uv_prepare_start(&_uv_prepare_handle, &Nudp::uv_callback_prepare));
    _uv_udp_handle.data = this;
    _uv_timer_handle.data = this;
    _uv_prepare_handle.data = this;
}

Nudp::~Nudp()
{
    DBG2("Nudp::~Nudp");
    _close();
}

NAN_METHOD(Nudp::close)
{
    /*
    struct CloseWorker : public ThreadLoop::Worker
    {
        Nudp& self;
        std::list<Msg*> canceled_message;
        CloseWorker(Nudp& nudp) : self(nudp)
        {
            self._tloop->submit(this);
        }
        virtual void work()
        {
            //canceled_message =
            self._close();
        }
        virtual void after_work()
        {
            while (!canceled_message.empty()) {
                Msg* m =canceled_message.front();
                v8::Local<v8::Value> argv[] = { NAN_ERR("NUDP CLOSED") };
                m->callback->Call(1, argv);
                canceled_message.pop_front();
                delete m;
            }
        }
    };
    */
    Nudp& self = *NAN_UNWRAP_THIS(Nudp);
    self._close();
    NAN_RETURN(Nan::Undefined());
}

void
Nudp::_close()
{
    if (_closed) {
        return;
    }
    _closed = true;
    LOG("Nudp::close: local_port " << _local_port);
    Nan::HandleScope scope;
    uv_close(reinterpret_cast<uv_handle_t*>(&_uv_udp_handle), NULL);
    uv_close(reinterpret_cast<uv_handle_t*>(&_uv_timer_handle), NULL);
    uv_close(reinterpret_cast<uv_handle_t*>(&_uv_prepare_handle), NULL);
    if (_utp_socket) {
        utp_close(_utp_socket);
        _utp_socket = NULL;
    }
    if (_recv_payload) {
        delete[] _recv_payload;
    }
    while (!_messages.empty()) {
        Msg* m =_messages.front();
        v8::Local<v8::Value> argv[] = { NAN_ERR("NUDP CLOSED") };
        m->callback->Call(1, argv);
        _messages.pop_front();
        delete m;
    }
    v8::Local<v8::Value> argv[] = { NAN_STR("close") };
    Nan::MakeCallback(handle(), "emit", 1, argv);
}

NAN_METHOD(Nudp::bind)
{
    /*
    struct BindWorker : public ThreadLoop::Worker
    {
        Nudp& self;
        BindWorker(Nudp& nudp) : self(nudp)
        {
            self._tloop->submit(this);
        }
        virtual void work()
        {
            self._bind(*address, port);
        }
        virtual void after_work()
        {
            v8::Local<v8::Value> args[] = { Nan::Undefined(), NAN_INT(self._local_port) };
            Nan::MakeCallback(info.This(), info[2].As<v8::Function>(), 2, args);
        }
    };
    */
    Nudp& self = *NAN_UNWRAP_THIS(Nudp);
    int port = info[0]->Int32Value();
    Nan::Utf8String address(info[1]);
    self._bind(*address, port);
    v8::Local<v8::Value> args[] = { Nan::Undefined(), NAN_INT(self._local_port) };
    Nan::MakeCallback(info.This(), info[2].As<v8::Function>(), 2, args);
    NAN_RETURN(Nan::Undefined());
}

NAN_METHOD(Nudp::connect)
{
    Nudp& self = *NAN_UNWRAP_THIS(Nudp);
    int port = info[0]->Int32Value();
    Nan::Utf8String address(info[1]);
    struct sockaddr_in sin;
    NAUV_IP4_ADDR(*address, port, &sin);
    self._bind("0.0.0.0", 0);
    DBG2("Nudp::connect:"
         << " local_port " << self._local_port
         << " to " << *address << ":" << port);
    self._setup_socket(NULL); // will create utp socket
    utp_connect(self._utp_socket, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
    v8::Local<v8::Value> args[] = { Nan::Undefined(), NAN_INT(self._local_port) };
    Nan::MakeCallback(info.This(), info[2].As<v8::Function>(), 2, args);
    NAN_RETURN(Nan::Undefined());
}

NAN_METHOD(Nudp::send)
{
    Nudp& self = *NAN_UNWRAP_THIS(Nudp);
    Msg* m = new Msg();
    // TODO handle leak of m on exception
    v8::Local<v8::Object> buffer_or_buffers = Nan::To<v8::Object>(info[0]).ToLocalChecked();
    m->callback.reset(new Nan::Callback(info[1].As<v8::Function>()));
    m->persistent.Reset(buffer_or_buffers); // keep persistent ref to the buffer
    if (node::Buffer::HasInstance(buffer_or_buffers)) {
        m->iovecs.resize(2);
        m->iovecs[0].iov_base = &m->hdr;
        m->iovecs[0].iov_len = MSG_HDR_SIZE;
        m->iovecs[1].iov_base = node::Buffer::Data(buffer_or_buffers);
        m->iovecs[1].iov_len = node::Buffer::Length(buffer_or_buffers);
        m->hdr.len = m->iovecs[1].iov_len;
        #if NUDP_USE_CRC
        m->hdr.crc = crc32_fast(m->iovecs[1].iov_base, m->iovecs[1].iov_len);
        #endif
    } else if (buffer_or_buffers->IsArray()) {
        int num_buffers = buffer_or_buffers.As<v8::Array>()->Length();
        m->iovecs.resize(num_buffers + 1);
        m->iovecs[0].iov_base = &m->hdr;
        m->iovecs[0].iov_len = MSG_HDR_SIZE;
        for (int i=0; i<num_buffers; ++i) {
            auto buf = NAN_GET_OBJ(buffer_or_buffers, i);
            char* data = node::Buffer::Data(buf);
            int len = node::Buffer::Length(buf);
            m->iovecs[i+1].iov_base = data;
            m->iovecs[i+1].iov_len = len;
            m->hdr.len += len;
            #if NUDP_USE_CRC
            m->hdr.crc = crc32_fast(data, len, m->hdr.crc);
            #endif
        }
    } else {
        return Nan::ThrowError("Nudp::send: expected buffer or array of buffers");
    }
    m->hdr.seq = self._send_msg_seq++;
    DBG2("Nudp::send:"
         << " seq " << m->hdr.seq
         << " len " << m->hdr.len
         << " local_port " << self._local_port);
    m->hdr.encode();
    self._messages.push_back(m);
    self._write_data();
    NAN_RETURN(Nan::Undefined());
}

void
Nudp::_write_data()
{
    Nan::HandleScope scope;
    while (!_messages.empty()) {
        Msg* m =_messages.front();
        const int num_iovecs = m->iovecs.size();
        const int remain_iovecs = num_iovecs - m->iov_index;
        if (remain_iovecs <= 0) {
            DBG3("Nudp::_write_data: write message done"
                 << " seq " << ntohll(m->hdr.seq)
                 << " len " << ntohl(m->hdr.len)
                 << " messages " << _messages.size()
                 << " local_port " << _local_port);
            v8::Local<v8::Value> argv[] = { Nan::Undefined() };
            m->callback->Call(1, argv);
            _messages.pop_front();
            delete m;
            continue;
        }
        utp_iovec* iop = &m->iovecs[m->iov_index];
        DBG3("Nudp::_write_data:"
             << " seq " << ntohll(m->hdr.seq)
             << " len " << ntohl(m->hdr.len)
             << " remain iovecs " << remain_iovecs
             << " next iovec len " << iop->iov_len
             << " messages " << _messages.size()
             << " local_port " << _local_port);
        size_t sent = utp_writev(_utp_socket, iop, remain_iovecs);
        if (sent <= 0) {
            DBG4("Nudp::_write_data: utp not writable. local_port " << _local_port);
            return;
        }
        if (DBG_VISIBLE(9) && m->iov_index == 0) {
            Buf::hexdump(iop->iov_base, iop->iov_len, "Nudp::_write_data (header)");
        }
        while (m->iov_index < num_iovecs && iop->iov_len >= 0 && sent >= iop->iov_len) {
            DBG4("Nudp::_write_data: iovec consumed completely "
                 << " iov_len " << iop->iov_len
                 << " sent " << sent
                 << " local_port " << _local_port);
            // iovec consumed completely
            sent -= iop->iov_len;
            iop->iov_len = 0;
            iop->iov_base = 0;
            iop++;
            m->iov_index++;
        }
        if (sent > 0) {
            DBG4("Nudp::_write_data: iovec partially consumed "
                 << " iov_len " << iop->iov_len
                 << " sent " << sent
                 << " local_port " << _local_port);
            // iovec partially consumed
            assert(m->iov_index < num_iovecs);
            assert(iop->iov_len > sent);
            iop->iov_len -= sent;
            iop->iov_base = reinterpret_cast<byte*>(iop->iov_base) + sent;
            sent = 0;
        } else {
            assert(sent == 0);
            assert(m->iov_index <= num_iovecs);
        }
    }
}

void
Nudp::_read_data(const uint8_t *buf, int len)
{
    DBG2("Nudp::_read_data: put buffer of length "
         << len << " local_port " << _local_port);
    while (len > 0) {
        if (!_recv_payload) {

            // copy bytes to header
            int copy_len = MSG_HDR_SIZE - _recv_hdr_pos;
            if (copy_len > len) {
                copy_len = len;
            }
            char* hdr_ptr = reinterpret_cast<char*>(&_recv_hdr) + _recv_hdr_pos;
            memcpy(hdr_ptr, buf, copy_len);
            buf += copy_len;
            len -= copy_len;
            _recv_hdr_pos += copy_len;

            // process the header when full
            if (_recv_hdr_pos >= MSG_HDR_SIZE) {
                if (DBG_VISIBLE(9)) {
                    Buf::hexdump(&_recv_hdr, MSG_HDR_SIZE, "Nudp::_read_data (header)");
                }
                _recv_hdr.decode();
                DBG3("Nudp::_read_data: incoming message"
                     << " seq " << _recv_hdr.seq
                     << " len " << _recv_hdr.len
                     << " local_port " << _local_port);
                if (_recv_hdr.len > MAX_MSG_LEN) {
                    // TODO close connection instead of panic
                    LOG("message too big:"
                        << " magic " << _recv_hdr.magic
                        << " seq " << _recv_hdr.seq
                        << " seq " << _recv_msg_seq
                        << " len " << _recv_hdr.len);
                    _recv_hdr.len = 0;
                }
                _recv_payload = new char[_recv_hdr.len];
                _recv_hdr_pos = 0;
                _recv_payload_pos = 0;
            }
        } else {

            // copy bytes to payload
            int copy_len = _recv_hdr.len - _recv_payload_pos;
            if (copy_len > len) {
                copy_len = len;
            }
            memcpy(_recv_payload + _recv_payload_pos, buf, copy_len);
            buf += copy_len;
            len -= copy_len;
            _recv_payload_pos += copy_len;

            // process the payload when full
            if (_recv_payload_pos >= _recv_hdr.len) {
                #if NUDP_USE_CRC
                uint32_t crc = crc32_fast(_recv_payload, _recv_hdr.len);
                #endif
                if (!_recv_hdr.is_valid() ||
                    #if NUDP_USE_CRC
                    crc != _recv_hdr.crc ||
                    #endif
                    _recv_hdr.seq != _recv_msg_seq) {
                    Buf::hexdump(&_recv_hdr, MSG_HDR_SIZE, "Nudp::_read_data (header decoded)");
                    Buf::hexdump(_recv_payload,
                                 _recv_hdr.len > 128 ? 128 : _recv_hdr.len,
                                 "Nudp::_read_data (payload)");
                    // TODO close connection instead of panic
                    PANIC("bad message:"
                          << " magic " << _recv_hdr.magic
                          << " seq " << _recv_hdr.seq
                          << " expected " << _recv_msg_seq
                          #if NUDP_USE_CRC
                          << " crc 0x" << std::hex << crc
                          << " expected 0x" << _recv_hdr.crc << std::dec
                          #endif
                          << " len " << _recv_hdr.len);
                }
                _recv_msg_seq += 1;
                // ownership on memory passed to the node buffer
                Nan::HandleScope scope;
                v8::Local<v8::Object> node_buf =
                    Nan::NewBuffer(_recv_payload, _recv_hdr.len).ToLocalChecked();
                _recv_payload = NULL;
                _recv_hdr_pos = 0;
                _recv_payload_pos = 0;
                // emit the message buffer
                DBG3("Nudp::_read_data: incoming message completed"
                     << " seq " << _recv_hdr.seq
                     << " len " << _recv_hdr.len
                     << " local_port " << _local_port);
                v8::Local<v8::Value> argv[] = { NAN_STR("message"), node_buf };
                Nan::MakeCallback(handle(), "emit", 2, argv);
            }
        }
    }
}

void
Nudp::_bind(const char* address, int port)
{
    if (_local_port) {
        return;
    }
    struct sockaddr_in sin;
    int sin_len = sizeof(sin);
    NAUV_IP4_ADDR(address, port, &sin);
    NAUV_CALL(uv_udp_bind(&_uv_udp_handle, NAUV_UDP_ADDR(&sin), 0));
    NAUV_CALL(uv_udp_getsockname(&_uv_udp_handle, NAUV_UDP_ADDR(&sin), &sin_len));
    _local_port = ntohs(sin.sin_port);
    _start_receiving();
}

void
Nudp::_setup_socket(utp_socket* socket)
{
    if (_utp_socket && !socket) {
        return;
    }
    if (_utp_socket) {
        PANIC("Nudp::_setup_socket: already has socket. local_port " << _local_port);
    }
    if (socket) {
        _utp_socket = socket;
    } else {
        _utp_socket = utp_create_socket(_utp_ctx);
    }
    // DBG5("original UTP_TARGET_DELAY " << utp_getsockopt(_utp_socket, UTP_TARGET_DELAY));
    // DBG5("original UTP_SNDBUF " << utp_getsockopt(_utp_socket, UTP_SNDBUF));
    // DBG5("original UTP_RCVBUF " << utp_getsockopt(_utp_socket, UTP_RCVBUF));
    utp_setsockopt(_utp_socket, UTP_TARGET_DELAY, UTP_TARGET_DELAY_MICROS); // in microseconds
    utp_setsockopt(_utp_socket, UTP_SNDBUF, UTP_SNDBUF_SIZE);
    utp_setsockopt(_utp_socket, UTP_RCVBUF, UTP_RCVBUF_SIZE);
    // start receiving if not already
    _start_receiving();
}

void
Nudp::_start_receiving()
{
    if (_receiving) {
        return;
    }
    _receiving = true;
    NAUV_CALL(uv_udp_recv_start(
                  &_uv_udp_handle,
                  &Nudp::uv_callback_alloc_wrap,
                  &Nudp::uv_callback_receive_wrap));
}

// check for utp events in the uv loop
NAUV_CALLBACK(Nudp::uv_callback_timer, uv_timer_t* handle)
{
    // DBG9("Nudp::uv_callback_timer");
    Nudp& self = *reinterpret_cast<Nudp*>(handle->data);
    utp_issue_deferred_acks(self._utp_ctx);
    utp_check_timeouts(self._utp_ctx);
}

NAUV_CALLBACK(Nudp::uv_callback_prepare, uv_prepare_t* handle)
{
    // DBG9("Nudp::uv_callback_prepare");
    Nudp& self = *reinterpret_cast<Nudp*>(handle->data);
    utp_issue_deferred_acks(self._utp_ctx);
}

void
Nudp::uv_callback_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    buf->len = suggested_size;
    buf->base = new char[buf->len];
    DBG9("Nudp::uv_callback_alloc: allocating " << buf->len << " suggested " << suggested_size);
}

void
Nudp::uv_callback_receive(
    uv_udp_t* handle,
    ssize_t nread,
    const uv_buf_t* buf,
    const struct sockaddr* addr,
    unsigned flags)
{
    Nudp& self = *reinterpret_cast<Nudp*>(handle->data);
    DBG3("Nudp::uv_callback_receive:"
         << " local_port " << self._local_port
         << " nread " << nread
         << " addr " << sockaddr2str(addr)
         << " flags " << flags);
    if (DBG_VISIBLE(9)) {
        Buf::hexdump(buf->base, nread > 128 ? 128 : nread, "Nudp::uv_callback_receive");
    }
    if (flags & UV_UDP_PARTIAL) {
        PANIC("Nudp::uv_callback_receive: truncated packet"
              << " local_port " << self._local_port
              << " nread " << nread
              << " buf len " << buf->len);
    }
    if (nread <= 0) {
        delete[] buf->base;
        return;
    }
    assert(addr);
    if (NAT::is_stun_packet(buf->base, nread)) {
        DBG2("Nudp::uv_callback_receive: got STUN packet local_port " << self._local_port);
        Nan::HandleScope scope;
        const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(addr);
        // the node buffer takes ownership on the memory, so not deleting the allocation in this path
        v8::Local<v8::Value> argv[] = {
            NAN_STR("stun"),
            Nan::NewBuffer(buf->base, nread).ToLocalChecked(),
            NAN_INT(ntohs(sin->sin_port)),
            NAN_STR(inet_ntoa(sin->sin_addr))
        };
        Nan::MakeCallback(self.handle(), "emit", 4, argv);
    } else {
        const byte* data = reinterpret_cast<const byte*>(buf->base);
        if (!utp_process_udp(self._utp_ctx, data, nread, addr, sizeof(struct sockaddr))) {
            DBG3("Nudp::uv_callback_receive: UDP packet not handled by UTP. Ignoring.");
        }
        delete[] buf->base;
    }
}

struct SendUtpPacketReq
{
    char* buf;
    struct sockaddr_in sin;
};

uint64_t
Nudp::utp_callback_sendto(utp_callback_arguments *a)
{
    Nudp& self = *reinterpret_cast<Nudp*>(utp_context_get_userdata(a->context));
    uv_udp_send_t* req = new uv_udp_send_t;
    SendUtpPacketReq* data = new SendUtpPacketReq;
    req->data = data;
    data->buf = new char[a->len];
    memcpy(data->buf, a->buf, a->len);
    uv_buf_t buf = uv_buf_init(data->buf, a->len);
    data->sin = *reinterpret_cast<const struct sockaddr_in*>(a->address);
    DBG3("Nudp::utp_callback_sendto:"
         << " local_port " << self._local_port
         << " packet length " << a->len
         << " addr " << sockaddr2str(a->address));
    if (DBG_VISIBLE(9)) {
        Buf::hexdump(a->buf, a->len > 128 ? 128 : a->len, "Nudp::utp_callback_sendto");
    }
    NAUV_CALL(uv_udp_send(
                  req, &self._uv_udp_handle,
                  &buf, 1,
                  NAUV_UDP_ADDR(&data->sin),
                  &Nudp::uv_callback_send_utp));
    return 0;
}

void
Nudp::uv_callback_send_utp(uv_udp_send_t* req, int status)
{
    DBG3("Nudp::uv_callback_send_utp: status " << status);
    // no real need to do anything if the status indicates an error
    // since UTP will handle timeouts anyhow.
    SendUtpPacketReq* data = reinterpret_cast<SendUtpPacketReq*>(req->data);
    delete[] data->buf;
    delete data;
    delete req;
}


uint64_t
Nudp::utp_callback_on_read(utp_callback_arguments *a)
{
    DBG3("Nudp::utp_callback_on_read: buffer length " << a->len);
    Nudp& self = *reinterpret_cast<Nudp*>(utp_context_get_userdata(a->context));
    self._read_data(a->buf, a->len);
    utp_read_drained(a->socket);
    return 0;
}

uint64_t
Nudp::utp_callback_on_state_change(utp_callback_arguments *a)
{
    DBG1("Nudp::utp_callback_on_state_change: state " <<
         utp_state_names[a->state] << " (" << a->state << ")");
    Nudp& self = *reinterpret_cast<Nudp*>(utp_context_get_userdata(a->context));
    utp_socket_stats *stats;

    switch (a->state) {
    case UTP_STATE_CONNECT:
    case UTP_STATE_WRITABLE:
        self._write_data();
        break;

    case UTP_STATE_EOF:
        LOG("Nudp::utp_callback_on_state_change: EOF");
        self._close();
        break;

    case UTP_STATE_DESTROYING:
        LOG("Nudp::utp_callback_on_state_change: destroying");
        stats = utp_get_stats(a->socket);
        if (stats) {
            LOG("Nudp::utp_callback_on_state_change: stats:");
            LOG("    Bytes sent:          " << stats->nbytes_xmit);
            LOG("    Bytes received:      " << stats->nbytes_recv);
            LOG("    Packets sent:        " << stats->nxmit);
            LOG("    Packets received:    " << stats->nrecv);
            LOG("    Duplicate receives:  " << stats->nduprecv);
            LOG("    Retransmits:         " << stats->rexmit);
            LOG("    Fast Retransmits:    " << stats->fastrexmit);
            LOG("    Best guess at MTU:   " << stats->mtu_guess);
        } else {
            LOG("Nudp::utp_callback_on_state_change: stats not available");
        }
        break;
    }

    return 0;
}

NAN_METHOD(Nudp::send_outbound)
{
    Nudp& self = *NAN_UNWRAP_THIS(Nudp);
    Msg* m = new Msg();
    v8::Local<v8::Object> buffer = Nan::To<v8::Object>(info[0]).ToLocalChecked();
    int port = info[1]->Int32Value();
    Nan::Utf8String address(info[2]);
    struct sockaddr_in sin;
    NAUV_IP4_ADDR(*address, port, &sin);

    uv_buf_t buf = uv_buf_init(node::Buffer::Data(buffer), node::Buffer::Length(buffer));
    m->persistent.Reset(buffer); // keep persistent ref to the buffer
    m->callback.reset(new Nan::Callback(info[3].As<v8::Function>()));

    DBG3("Nudp::send_outbound: packet length " << buf.len << " addr " << *address << ":" << port);
    uv_udp_send_t* req = new uv_udp_send_t;
    req->data = m;
    NAUV_CALL(uv_udp_send(
                  req, &self._uv_udp_handle,
                  &buf, 1,
                  NAUV_UDP_ADDR(&sin),
                  &Nudp::uv_callback_send_outbound));
    NAN_RETURN(Nan::Undefined());
}

void
Nudp::uv_callback_send_outbound(uv_udp_send_t* req, int status)
{
    Nan::HandleScope scope;
    DBG3("Nudp::uv_callback_send_outbound: status " << status);
    Msg* m = reinterpret_cast<Msg*>(req->data);
    if (status) {
        v8::Local<v8::Value> argv[] = { NAN_ERR("NUDP STUN SEND ERROR") };
        m->callback->Call(1, argv);
    } else {
        v8::Local<v8::Value> argv[] = { Nan::Undefined() };
        m->callback->Call(1, argv);
    }
    delete m;
    delete req;
}

NAN_METHOD(Nudp::stats)
{
    Nudp& self = *NAN_UNWRAP_THIS(Nudp);
    if (!self._utp_socket) {
        NAN_RETURN(Nan::Undefined());
        return;
    }
    utp_socket_stats* stats = utp_get_stats(self._utp_socket);
    if (!stats) {
        NAN_RETURN(Nan::Undefined());
        return;
    }
    auto obj = NAN_NEW_OBJ();
    NAN_SET_NUM(obj, "bytes_sent", stats->nbytes_xmit);
    NAN_SET_NUM(obj, "bytes_received", stats->nbytes_recv);
    NAN_SET_INT(obj, "packets_sent", stats->nxmit);
    NAN_SET_INT(obj, "packets_received", stats->nrecv);
    NAN_SET_INT(obj, "packets_received_dup", stats->nduprecv);
    NAN_SET_INT(obj, "retransmits", stats->rexmit);
    NAN_SET_INT(obj, "retransmits_fast", stats->fastrexmit);
    NAN_SET_INT(obj, "mtu_guess", stats->mtu_guess);
    NAN_RETURN(obj);
}

uint64_t
Nudp::utp_callback_on_firewall(utp_callback_arguments *a)
{
    DBG1("Nudp::utp_callback_on_firewall");
    Nudp& self = *reinterpret_cast<Nudp*>(utp_context_get_userdata(a->context));
    return 0;
}

uint64_t
Nudp::utp_callback_on_accept(utp_callback_arguments *a)
{
    LOG("Nudp::utp_callback_on_accept");
    Nudp& self = *reinterpret_cast<Nudp*>(utp_context_get_userdata(a->context));
    self._setup_socket(a->socket);
    return 0;
}

uint64_t
Nudp::utp_callback_on_error(utp_callback_arguments *a)
{
    LOG("Nudp::utp_callback_on_error: " << utp_error_code_names[a->error_code]);
    Nudp& self = *reinterpret_cast<Nudp*>(utp_context_get_userdata(a->context));
    self._close();
    return 0;
}

uint64_t
Nudp::utp_callback_log(utp_callback_arguments *a)
{
    LOG("Nudp::utp_callback_log: " << a->buf);
    return 0;
}


static
std::string
addrinfo2str(const struct addrinfo* ai)
{
    if (!ai) return std::string("?:?");
    const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
    return std::string(inet_ntoa(sin->sin_addr)) + ":" + std::to_string(ntohs(sin->sin_port));
}

static
std::string
sockaddr2str(const struct sockaddr* sa)
{
    if (!sa) return std::string("?:?");
    const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(sa);
    return std::string(inet_ntoa(sin->sin_addr)) + ":" + std::to_string(ntohs(sin->sin_port));
}

Nudp::Msg::Msg()
    : iov_index(0)
{
}

Nudp::Msg::~Msg()
{
    persistent.Reset();
    callback.reset();
}

void
Nudp::MsgHdr::encode()
{
    len = htonl(len);
    seq = htonll(seq);
    #if NUDP_USE_CRC
    crc = htonl(crc);
    #endif
}

void
Nudp::MsgHdr::decode()
{
    len = ntohl(len);
    seq = ntohll(seq);
    #if NUDP_USE_CRC
    crc = ntohl(crc);
    #endif
}

const char Nudp::MSG_HDR_MAGIC[Nudp::MSG_MAGIC_LEN] = { 'N', 'u', 'd', 'p' };

bool
Nudp::MsgHdr::is_valid()
{
    if (memcmp(magic, MSG_HDR_MAGIC, MSG_MAGIC_LEN)) {
        return false;
    }
    return true;
}

} // namespace noobaa
