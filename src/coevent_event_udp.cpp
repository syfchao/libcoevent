
#include "coevent.h"
#include "coevent_itnl.h"
#include <string.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>

using namespace andrewmc::libcoevent;

typedef Event _super;

// ==========
// necessary definitions
#define __CO_EVENT_UDP_DEFINITIONS
#ifdef __CO_EVENT_UDP_DEFINITIONS

#define _OP_NONE    (0)
#define _OP_SLEEP   (1 << 0)
#define _OP_RECV    (1 << 1)


struct _EventArg {
    UDPEvent            *event;
    int                 fd;
    uint32_t            *libevent_what_ptr;

    struct stCoRoutine_t *coroutine;
    WorkerFunc          worker_func;
    void                *user_arg;

    _EventArg(): event(NULL), fd(0), libevent_what_ptr(NULL), coroutine(NULL)
    {}
};

#endif


// ==========
// libco style routine, this routine is first part of the coroutine adapter
#define __CO_EVENT_UDP_LIBCO_ROUTINE
#ifdef __CO_EVENT_UDP_LIBCO_ROUTINE

static void *_libco_routine(void *libco_arg)
{
    struct _EventArg *arg = (struct _EventArg *)libco_arg;
    (arg->worker_func)(arg->fd, arg->event, arg->user_arg);
    return NULL;
}

#endif


// ==========
// libevent style callback, this callback is second part of the coroutine adapter
#define __CO_EVENT_UDP_CALLBACK
#ifdef __CO_EVENT_UDP_CALLBACK

static void _libevent_callback(evutil_socket_t fd, short what, void *libevent_arg)
{
    struct _EventArg *arg = (struct _EventArg *)libevent_arg;

    // switch into the coroutine
    if (arg->libevent_what_ptr) {
        *(arg->libevent_what_ptr) = (uint32_t)what;
        DEBUG("libevent what: 0x%08x - %s%s", (unsigned)what, event_is_timeout(what) ? "timeout " : "", event_readable(what) ? "read" : "");
    }
    co_resume(arg->coroutine);

    // is coroutine end?
    if (is_coroutine_end(arg->coroutine)) {
        // delete the event if this is under control of the base
        UDPEvent *event = arg->event;
        Base  *base  = event->owner();

        DEBUG("evudp %s ends", event->identifier().c_str());
        base->delete_event_under_control(event);
    }

    // done
    return;
}


#endif


// ==========
#define __INTERNAL_MISC_FUNCTIONS
#ifdef __INTERNAL_MISC_FUNCTIONS

uint32_t UDPEvent::_libevent_what()
{
    uint32_t ret = *_libevent_what_storage;
    *_libevent_what_storage = 0;
    return ret;
}


int UDPEvent::_fd()
{
    if (_fd_ipv4) {
        return _fd_ipv4;
    } else if (_fd_ipv6) {
        return _fd_ipv6;
    } else if (_fd_unix) {
        return _fd_unix;
    } else {
        ERROR("file descriptor not initialized");
        _status.set_app_errno(ERR_NOT_INITIALIZED);
        return -1;
    }
}


struct sockaddr *UDPEvent::_remote_sock_addr()
{
    if (_fd_ipv4) {
        return (struct sockaddr *)(&_remote_addr_ipv4);
    } else if (_fd_ipv6) {
        return (struct sockaddr *)(&_remote_addr_ipv6);
    } else if (_fd_unix) {
        return (struct sockaddr *)(&_remote_addr_unix);
    } else {
        ERROR("file descriptor not initialized");
        _status.set_app_errno(ERR_NOT_INITIALIZED);
        return (struct sockaddr *)(&_remote_addr_unix);
    }
}


socklen_t *UDPEvent::_remote_sock_addr_len()
{
    if (_fd_ipv4) {
        return &_remote_addr_ipv4_len;
    } else if (_fd_ipv6) {
        return &_remote_addr_ipv6_len;
    } else if (_fd_unix) {
        return &_remote_addr_unix_len;
    } else {
        ERROR("file descriptor not initialized");
        _status.set_app_errno(ERR_NOT_INITIALIZED);
        return &_remote_addr_unix_len;
    }
}


#endif  // end of __INTERNAL_MISC_FUNCTIONS


// ==========
#define __PUBLIC_INIT_AND_CLEAR_FUNCTIONS
#ifdef __PUBLIC_INIT_AND_CLEAR_FUNCTIONS

struct Error UDPEvent::init(Base *base, WorkerFunc func, const struct sockaddr *addr, socklen_t addr_len, void *user_arg, BOOL auto_free)
{
    if (!(base && addr)) {
        _status.set_app_errno(ERR_PARA_NULL);
        return _status;
    }

    if (addr->sa_family != AF_INET
        && addr->sa_family != AF_INET6
        && addr->sa_family != AF_UNIX)
    {
        _status.set_app_errno(ERR_NETWORK_TYPE_ILLEGAL);
        return _status;
    }

    _clear();

    // create arguments
    struct _EventArg *arg = new _EventArg;
    _event_arg = arg;
    arg->event = this;
    arg->user_arg = user_arg;
    arg->worker_func = func;
    arg->libevent_what_ptr = _libevent_what_storage;
    DEBUG("arg->libevent_what_ptr = %p", arg->libevent_what_ptr);

    // create arg for libco
    int call_ret = co_create(&(arg->coroutine), NULL, _libco_routine, arg);
    if (call_ret != 0) {
        _clear();
        _status.set_app_errno(ERR_LIBCO_CREATE);
        return _status;
    }
    else {
        DEBUG("Init coroutine %p", arg->coroutine);
    }

    // determine network type
    if (AF_INET == addr->sa_family)
    {
        memcpy(&_sockaddr_ipv4, addr, sizeof(_sockaddr_ipv4));
        _fd_ipv4 = socket(AF_INET, SOCK_DGRAM, 0);
        if (_fd_ipv4 < 0) {
            _clear();
            _status.set_sys_errno(errno);
            return _status;
        }
    }
    else if (AF_INET6 == addr->sa_family)
    {
        memcpy(&_sockaddr_ipv6, addr, sizeof(_sockaddr_ipv6));
        _fd_ipv6 = socket(AF_INET6, SOCK_DGRAM, 0);
        if (_fd_ipv6 < 0) {
            _clear();
            _status.set_sys_errno(errno);
            return _status;
        }
    }
    else
    {
        memcpy(&_sockaddr_unix, addr, sizeof(_sockaddr_unix));
        _fd_unix = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (_fd_unix < 0) {
            _clear();
            _status.set_sys_errno(errno);
            return _status;
        }
    }

    // try binding
    int fd = _fd_ipv4 ? _fd_ipv4 : (_fd_ipv6 ? _fd_ipv4 : _fd_unix);
    int status = bind(_fd_ipv4, addr, addr_len);
    if (status < 0) {
        _clear();
        _status.set_sys_errno(errno);
        return _status;
    }

    // non-block
    set_fd_nonblock(fd);

    // create event
    _owner_base = base;
    _event = event_new(base->event_base(), fd, EV_TIMEOUT | EV_READ, _libevent_callback, arg);     // should NOT use EV_ET or EV_PERSIST
    if (NULL == _event) {
        ERROR("Failed to new a UDP event");
        _clear();
        _status.set_app_errno(ERR_EVENT_EVENT_NEW);
        return _status;
    }
    else {
        struct timeval sleep_time = {0, 0};
        DEBUG("Add UDP event %s", _identifier.c_str());
        event_add(_event, &sleep_time);
    }

    // automatic free
    if (auto_free) {
        base->put_event_under_control(this);
    }

    _status.clear_err();
    return _status;
}


struct Error UDPEvent::init(Base *base, WorkerFunc func, const struct sockaddr &addr, socklen_t addr_len, void *user_arg, BOOL auto_free)
{
    return init(base, func, &addr, addr_len, user_arg, auto_free);
}


struct Error UDPEvent::init(Base *base, WorkerFunc func, NetType_t network_type, int bind_port, void *user_arg, BOOL auto_free)
{
    if (NetIPv4 == network_type)
    {
        DEBUG("Init a IPv4 UDP event");
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)bind_port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return init(base, func, (const struct sockaddr *)(&addr), sizeof(addr), user_arg, auto_free);
    }
    else if (NetIPv6 == network_type)
    {
        DEBUG("Init a IPv6 UDP event");
        struct sockaddr_in6 addr6;
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((unsigned short)bind_port);
        addr6.sin6_addr = in6addr_any;
        return init(base, func, (const struct sockaddr *)(&addr6), sizeof(addr6), user_arg, auto_free);
    }
    else {
        ERROR("Invalid network type %d", (int)network_type);
        _status.set_app_errno(ERR_NETWORK_TYPE_ILLEGAL);
        return _status;
    }
}


struct Error UDPEvent::init(Base *base, WorkerFunc func, const char *bind_path, void *user_arg, BOOL auto_free)
{
    struct sockaddr_un addr;
    size_t path_len = 0;
    
    if (NULL == bind_path) {
        _status.set_app_errno(ERR_PARA_NULL);
        return _status;
    }

    path_len = strlen(bind_path);
    if (path_len + 1 > sizeof(addr.sun_path)) {
        _status.set_app_errno(ERR_BIND_PATH_ILLEGAL);
        return _status;
    }

    DEBUG("Init a local UDP event");
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, bind_path, path_len + 1);
    return init(base, func, (const struct sockaddr *)(&addr), sizeof(addr), user_arg, auto_free);
}


struct Error UDPEvent::init(Base *base, WorkerFunc func, std::string &bind_path, void *user_arg, BOOL auto_free)
{
    return init(base, func, bind_path.c_str(), user_arg, auto_free);
}


void UDPEvent::_init()
{
    char identifier[64];
    sprintf(identifier, "UDP event %p", this);
    _identifier = identifier;

    _action_mask = _OP_NONE;
    _remote_addr_ipv4_len = sizeof(_remote_addr_ipv4);
    _remote_addr_ipv6_len = sizeof(_remote_addr_ipv6);
    _remote_addr_unix_len = sizeof(_remote_addr_unix);

    if (NULL == _libevent_what_storage) {
        _libevent_what_storage = (uint32_t *)malloc(sizeof(*_libevent_what_storage));
        if (NULL == _libevent_what_storage) {
            ERROR("Failed to init storage");
            throw std::bad_alloc();
        }
        else {
            *_libevent_what_storage = 0;
            DEBUG("Init _libevent_what_storage OK: %p", _libevent_what_storage);
        }
    }
    return;
}


void UDPEvent::_clear()
{
    if (_event) {
        DEBUG("Delete io event");
        evtimer_del(_event);
        _event = NULL;
    }

    if (_owner_base) {
        DEBUG("clear owner base");
        _owner_base = NULL;
    }

    if (_event_arg) {
        struct _EventArg *arg = (struct _EventArg *)_event_arg;
        _event_arg = NULL;

        if (arg->coroutine) {
            DEBUG("remove coroutine");
            co_release(arg->coroutine);
            arg->coroutine = NULL;
        }

        DEBUG("Delete _event_arg");
        delete arg;
    }

    if (_fd_ipv4) {
        close(_fd_ipv4);
    }
    if (_fd_ipv6) {
        close(_fd_ipv6);
    }
    if (_fd_unix) {
        close(_fd_unix);
    }

    _event_arg = NULL;
    _fd_ipv4 = 0;
    _fd_ipv6 = 0;
    _fd_unix = 0;
    return;
}


UDPEvent::UDPEvent()
{
    _init();
    return;
}


UDPEvent::~UDPEvent()
{
    _clear();

    if (_libevent_what_storage) {
        free(_libevent_what_storage);
        _libevent_what_storage = NULL;
    }

    return;
}

#endif  // end of __PUBLIC_INIT_AND_CLEAR_FUNCTIONS


// ==========
#define __PUBLIC_MISC_FUNCTIONS
#ifdef __PUBLIC_MISC_FUNCTIONS

NetType_t UDPEvent::network_type()
{
    if (_fd_ipv4) {
        return NetIPv4;
    } else if (_fd_ipv6) {
        return NetIPv6;
    } else if (_fd_unix) {
        return NetLocal;
    } else {
        return NetUnknown;
    }
}


const char *UDPEvent::c_socket_path()
{
    if (_fd_unix) {
        return _sockaddr_unix.sun_path;
    } else {
        return "";
    }
}


int UDPEvent::port()
{
    if (_fd_ipv4)
    {
        unsigned short port = ntohs(_sockaddr_ipv4.sin_port);
        if (0 == port) {
            DEBUG("read sock info by getsockname()");
            struct sockaddr_in addr = {0};
            socklen_t socklen = sizeof(addr);
            getsockname(_fd_ipv4, (struct sockaddr *)(&addr), &socklen);
            port = ntohs(addr.sin_port);
        }
        return (int)port;
    }
    if (_fd_ipv6)
    {
        unsigned short port = ntohs(_sockaddr_ipv6.sin6_port);
        if (0 == port) {
            DEBUG("read sock info by getsockname()");
            struct sockaddr_in6 addr = {0};
            socklen_t socklen = sizeof(addr);
            getsockname(_fd_ipv6, (struct sockaddr *)(&addr), &socklen);
            port = ntohs(addr.sin6_port);
        }
        return (int)port;
    }
    // else
    return -1;
}


#endif  // end of __PUBLIC_MISC_FUNCTIONS


// ==========
#define __PUBLIC_FUNCTIONAL_FUNCTIONS
#ifdef __PUBLIC_FUNCTIONAL_FUNCTIONS


struct Error UDPEvent::sleep(struct timeval &sleep_time)
{
    struct _EventArg *arg = (struct _EventArg *)_event_arg;

    _status.clear_err();
    if ((0 == sleep_time.tv_sec) && (0 == sleep_time.tv_usec)) {
        return _status;
    }

    event_add(_event, &sleep_time);
    co_yield(arg->coroutine);

    // determine libevent event masks
    uint32_t libevent_what = _libevent_what();
    if (event_is_timeout(libevent_what))
    {
        // normally timeout
        _status.clear_err();
        return _status;
    }
    else if (event_readable(libevent_what))
    {
        // read event occurred
        _status.set_app_errno(ERR_INTERRUPTED_SLEEP);
        return _status;
    }
    else {
        // unexpected error
        ERROR("%s - unexpected libevent masks: 0x%04x", identifier().c_str(), (unsigned)libevent_what);
        _status.set_app_errno(ERR_UNKNOWN);
        return _status;
    }
}


struct Error UDPEvent::sleep_milisecs(unsigned mili_secs)
{
    struct timeval timeout = to_timeval_from_milisecs(mili_secs);
    return sleep(timeout);
}


struct Error UDPEvent::sleep(double seconds)
{
    if (seconds <= 0) {
        _status.clear_err();
        return _status;
    }
    else {
        struct timeval timeout = to_timeval(seconds);
        return sleep(timeout);
    }
}


struct Error UDPEvent::recv_in_timeval(void *data_out, const size_t len_limit, size_t *len_out, const struct timeval &timeout)
{
    ssize_t recv_len = 0;
    uint32_t libevent_what = 0;
    struct _EventArg *arg = (struct _EventArg *)_event_arg;

    // param check
    if (NULL == data_out) {
        ERROR("no recv data buffer spectied");
        _status.set_app_errno(ERR_PARA_NULL);
        return _status;
    }
    _status.clear_err();

    // recvfrom()
    libevent_what = _libevent_what();
    if (event_readable(libevent_what))
    {
        // data readable
        recv_len = recv_from(_fd(), data_out, len_limit, 0, _remote_sock_addr(), _remote_sock_addr_len());
        if (recv_len < 0) {
            _status.set_sys_errno();
        }
    }
    else {
        // no data avaliable
        struct timeval timeout_copy;
        timeout_copy.tv_sec = timeout.tv_sec;
        timeout_copy.tv_usec = timeout.tv_usec;

        DEBUG("UDP libevent what flag: 0x%04x, now wait", (unsigned)libevent_what);
        if ((0 == timeout_copy.tv_sec) && (0 == timeout_copy.tv_usec)) {
            timeout_copy.tv_sec = FOREVER_SECONDS;
        }
        event_add(_event, &timeout_copy);
        co_yield(arg->coroutine);

        // check if data read
        libevent_what = _libevent_what();
        if (event_readable(libevent_what))
        {
            recv_len = recv_from(_fd(), data_out, len_limit, 0, _remote_sock_addr(), _remote_sock_addr_len());
            if (recv_len < 0) {
                _status.set_sys_errno();
            }
        }
        else if (event_is_timeout(libevent_what))
        {
            recv_len = 0;
            _status.set_app_errno(ERR_TIMEOUT);
        }
        else {
            ERROR("unrecognized event flag: 0x%04u", libevent_what);
            _status.set_app_errno(ERR_UNKNOWN);
        }
    }

    // write read data len and return
    if (len_out) {
        *len_out = (recv_len > 0) ? recv_len : 0;
    }
    return _status;
}


struct Error UDPEvent::recv_in_mimlisecs(void *data_out, const size_t len_limit, size_t *len_out, unsigned mili_secs)
{
    struct timeval timeout = to_timeval_from_milisecs(mili_secs);
    return recv_in_timeval(data_out, len_limit, len_out, timeout);
}


struct Error UDPEvent::recv(void *data_out, const size_t len_limit, size_t *len_out, double timeout_seconds)
{
    struct timeval timeout = {0, 0};
    if (timeout_seconds > 0) {
        timeout = to_timeval(timeout_seconds);
    }

    return recv_in_timeval(data_out, len_limit, len_out, timeout);
}


void UDPEvent::copy_client_addr(std::string &addr_str)
{
    if (_fd_ipv4)
    {
        char c_addr_str[INET_ADDRSTRLEN + 7];
        char c_port_str[7];

        inet_ntop(AF_INET, &(_remote_addr_ipv4.sin_addr), c_addr_str, sizeof(c_addr_str));
        sprintf(c_port_str, ":%u", (unsigned)ntohs(_remote_addr_ipv4.sin_port));
        strcat(c_addr_str, c_port_str);

        addr_str = c_addr_str;
    }
    else if (_fd_ipv6) {
        char c_addr_str[INET6_ADDRSTRLEN + 7];
        char c_port_str[7];

        inet_ntop(AF_INET6, &(_remote_addr_ipv6.sin6_addr), c_addr_str, sizeof(c_addr_str));
        sprintf(c_port_str, ":%u", (unsigned)ntohs(_remote_addr_ipv6.sin6_port));
        strcat(c_addr_str, c_port_str);

        addr_str = c_addr_str;
    }
    else if (_fd_unix) {
        addr_str = _remote_addr_unix.sun_path;
    }
    else {
        addr_str.clear();
    }
    return;
}

#endif
