#include "ether/ether.h"
#include "codeine/vec.h"

#include <uv.h>

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include <sched.h>

ETH_MODULE("libuv")

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                Promise
 */
typedef enum {
  PROMISE_PENDING,
  PROMISE_RESOLVE,
  PROMISE_REJECT,
} promise_status;

typedef struct promise {
  eth_header header;
  promise_status status;
  eth_t result;
} promise;
#define PROMISE(x) ((struct promise*)(x))
static eth_type *promise_type = NULL;

static void
destroy_promise(eth_type *type, eth_t p)
{
  if (PROMISE(p)->status != PROMISE_PENDING)
    eth_unref(PROMISE(p)->result);
  free(p);
}

static eth_t
create_promise(void)
{
  promise *p = malloc(sizeof(promise));
  eth_init_header(p, promise_type);
  p->status = PROMISE_PENDING;
  return ETH(p);
}

static eth_t
_resolve(void)
{
  promise* promise = PROMISE(*eth_sp++);
  eth_t result = *eth_sp++;
  promise->status = PROMISE_RESOLVE;
  eth_ref(promise->result = result);
  eth_drop_2(ETH(promise), result);
  return eth_nil;
}

static eth_t
_reject(void)
{
  promise* promise = PROMISE(*eth_sp++);
  eth_t result = *eth_sp++;
  promise->status = PROMISE_REJECT;
  eth_ref(promise->result = result);
  eth_drop_2(ETH(promise), result);
  return eth_nil;
}

static eth_t
_promise(void)
{
  eth_t fn = *eth_sp++;
  if (eth_unlikely(not eth_is_fn(fn)))
  {
    eth_drop(fn);
    return eth_exn(eth_type_error());
  }
  eth_t p = create_promise();
  eth_ref(p);
  eth_ref(fn);
  eth_reserve_stack(1);
  eth_sp[0] = p;
  eth_t ret = eth_apply(fn, 1);
  eth_unref(fn);
  eth_dec(p);
  return p;
}

static eth_t
_await(void)
{
  eth_t p = *eth_sp++;
  eth_ref(p);
  if (eth_unlikely(p->type != promise_type))
  {
    eth_unref(p);
    return eth_exn(eth_type_error());
  }

  while (true)
  {
    switch (PROMISE(p)->status)
    {
      case PROMISE_PENDING:
        uv_run(uv_default_loop(), UV_RUN_ONCE);
        sched_yield();
        continue;

      case PROMISE_RESOLVE:
      {
        eth_t ret = PROMISE(p)->result;
        eth_ref(ret);
        eth_unref(p);
        eth_dec(ret);
        return ret;
      }

      case PROMISE_REJECT:
      {
        eth_t ret = eth_exn(PROMISE(p)->result);
        eth_unref(p);
        return ret;
      }
    }
  }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                File I/O
 */
typedef struct {
  eth_header header;
  uv_file file;
} uvfile;
#define UVFILE(x) ((uvfile*)(x))
static eth_type *uvfile_type = NULL;

static void
destroy_file(eth_type *type, eth_t file)
{
  close(UVFILE(file)->file);
  free(file);
}

static eth_t
create_file(uv_file file)
{
  uvfile *f = malloc(sizeof(uvfile));
  eth_init_header(f, uvfile_type);
  f->file = file;
  return ETH(f);
}

typedef struct {
  eth_t cb;
  eth_t file;
  eth_t buflist;
  uv_buf_t *bufs;
  int nbufs;
} read_data;

static void
on_read(uv_fs_t *req)
{
  read_data *data = req->data;
  eth_t cb = data->cb;
  eth_t err, nrd;
  if (eth_unlikely(req->result < 0))
  {
    nrd = eth_nil;
    err = eth_str(uv_strerror((int)req->result));
  }
  else
  {
    nrd = eth_num(req->result);
    err = eth_false;
  }
  /*
   * Apply callback:
   */
  eth_reserve_stack(2);
  eth_sp[0] = nrd;
  eth_sp[1] = err;
  eth_t ret = eth_apply(cb, 2);
  if (eth_unlikely(eth_is_exn(ret)))
    eth_error("error in async callback: ~w", ret);
  eth_drop(ret);
  /*
   * Clean up:
   */
  eth_unref(cb);
  eth_unref(data->buflist);
  eth_unref(data->file);
  free(data->bufs);
  free(data);
  uv_fs_req_cleanup(req);
  free(req);
}

static eth_t
__fs_read(void)
{
  eth_args args = eth_start(4);
  eth_t file = eth_arg2(args, uvfile_type);
  eth_t buflist = eth_arg(args);
  int64_t offs = eth_num_val(eth_arg2(args, eth_number_type));
  eth_t cb = eth_arg2(args, eth_function_type);
  /*
   * Prepare buffers:
   */
  size_t nbufs = eth_length(buflist, NULL);
  uv_buf_t *bufs = malloc(sizeof(uv_buf_t) * nbufs);
  int i = 0;
  for (eth_t it = buflist; eth_is_pair(it); it = eth_cdr(it))
  {
    eth_t str = eth_car(it);
    bufs[i].base = eth_str_cstr(str);
    bufs[i].len = eth_str_len(str);
  }
  /*
   * Set up handle:
   */
  uv_fs_t *req = malloc(sizeof(uv_fs_t));
  read_data *data = malloc(sizeof(read_data));
  eth_ref(data->cb = cb);
  eth_ref(data->file = file);
  eth_ref(data->buflist = buflist);
  data->bufs = bufs;
  data->nbufs = nbufs;
  req->data = data;
  /*
   * Run the job:
   */
  int fd = UVFILE(file)->file;
  uv_fs_read(uv_default_loop(), req, fd, bufs, nbufs, offs, on_read);
  eth_return(args, eth_nil);
}

static void
on_open(uv_fs_t *req)
{
  eth_t cb = req->data;
  eth_t err = req->result<0? eth_str(uv_strerror((int)req->result)) : eth_false;
  eth_t file = req->result < 0 ? eth_nil : create_file(req->result);
  // apply callback
  eth_reserve_stack(2);
  eth_sp[0] = file;
  eth_sp[1] = err;
  eth_t ret = eth_apply(cb, 2);
  if (eth_unlikely(eth_is_exn(ret)))
    eth_error("error in async callback: ~w", ret);
  eth_drop(ret);
  // cleanup
  eth_unref(cb);
  uv_fs_req_cleanup(req);
  free(req);
}

static eth_t
_open(void)
{
  eth_args args = eth_start(4);
  const char *path = eth_str_cstr(eth_arg2(args, eth_string_type));
  int flags = eth_num_val(eth_arg2(args, eth_number_type));
  int mode = eth_num_val(eth_arg2(args, eth_number_type));
  eth_t cb = eth_arg2(args, eth_function_type);
  uv_fs_t *open_req = malloc(sizeof(uv_fs_t));
  eth_ref(open_req->data = cb);
  uv_fs_open(uv_default_loop(), open_req, path, flags, mode, on_open);
  eth_return(args, eth_nil);
}

/*******************************************************************************
 *                              Networking
 */
typedef struct {
  eth_t cb, acc;
} stream_reader;

typedef struct {
  eth_header header;
  uv_stream_t *stream;
  cod_vec(stream_reader) readers;
  bool isshutdown;
} uvstream;
#define UVSTREAM(x) ((uvstream*)(x))
static eth_type *uvstream_type = NULL;

static void
add_reader(eth_t stream, eth_t cb, eth_t acc)
{
  uvstream *uvs = UVSTREAM(stream);
  /* will keep a REF until there is at least one reader. */
  if (uvs->readers.len == 0)
    eth_ref(stream);
  cod_vec_emplace(uvs->readers, { .cb = cb, .acc = acc });
  eth_ref(cb);
  eth_ref(acc);
}

/*------------------------------------------------------------------------------
 * Detach readers from the stream and UNREF to enable deletion of the object.
 *
 * Note: Does nothing if there are no readers attached.
 */
static void
remove_readers(eth_t stream)
{
  uvstream *uvs = UVSTREAM(stream);
  if (uvs->readers.len > 0)
  {
    cod_vec_iter(uvs->readers, i, r, eth_unref(r.cb); eth_unref(r.acc));
    uvs->readers.len = 0;
    eth_unref(stream);
  }
}

static void
destroy_stream(eth_type *type, eth_t x)
{
  assert(UVSTREAM(x)->readers.len == 0);
  cod_vec_destroy(UVSTREAM(x)->readers);
  uv_close((uv_handle_t*)UVSTREAM(x)->stream, (uv_close_cb)free);
  free(x);
}

static void
on_read_start(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
  eth_use_symbol(End_of_file);

  uvstream *uvs = UVSTREAM(stream->data);
  eth_t str, err;
  bool isend = false;

  if (nread == UV_EOF)
  {
    str = eth_nil;
    err = End_of_file;
    isend = true;
    if (buf->base)
      free(buf->base);
  }
  else if (nread < 0)
  {
    str = eth_nil;
    err = eth_str(uv_strerror(nread));
    if (buf->base)
      free(buf->base);
  }
  //else if (nread == 0)
  //{
    //str = eth_nil;
    //err = eth_false;
    //if (buf->base)
      //free(buf->base);
    //goto detach_readers;
  //}
  else
  {
    assert(buf->base != NULL);
    char *strbuf = buf->base;
    if (buf->len >= (size_t)nread * 2)
      strbuf = realloc(strbuf, nread + 1);
    strbuf[nread] = '\0';
    str = eth_create_string_from_ptr2(strbuf, nread);
    err = eth_false;
  }

  /*
   * Apply callbacks:
   */
  eth_ref(str);
  eth_ref(err);
  for (size_t i = 0; i < uvs->readers.len; ++i)
  {
    eth_reserve_stack(3);
    eth_t acc = uvs->readers.data[i].acc;
    eth_sp[0] = acc;
    eth_dec(acc);
    eth_sp[1] = str;
    eth_sp[2] = err;
    if (i == uvs->readers.len - 1)
    {
      eth_dec(str);
      eth_dec(err);
    }
    eth_t ret = eth_apply(uvs->readers.data[i].cb, 3);
    if (eth_unlikely(eth_is_exn(ret)))
      eth_error("error in async callback: ~w", ret);
    eth_ref(uvs->readers.data[i].acc = ret);
  }

  if (isend)
    remove_readers(ETH(uvs));
}

static eth_t
__read_start(void)
{
  eth_args args = eth_start(3);
  eth_t stream = eth_arg2(args, uvstream_type);
  eth_t cb = eth_arg2(args, eth_function_type);
  eth_t acc = eth_arg(args);

  void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf)
  {
    buf->base = malloc(size + 1);
    buf->len = size;
  }
  add_reader(stream, cb, acc);
  uv_read_start(UVSTREAM(stream)->stream, alloc_cb, on_read_start);

  eth_return(args, eth_nil);
}


typedef struct {
  eth_t cb;
  eth_t buflist;
  uv_buf_t *bufs;
} write_data;

static void
on_write(uv_write_t *req, int status)
{
  write_data *data = req->data;
  eth_reserve_stack(1);
  eth_sp[0] = status < 0 ? eth_str(uv_strerror(status)) : eth_false;
  eth_t ret = eth_apply(data->cb, 1);
  if (eth_unlikely(eth_is_exn(ret)))
    eth_error("error in async callback: ~w", ret);
  eth_drop(ret);
  /*
   * Cleanup:
   */
  free(data->bufs);
  eth_unref(data->cb);
  eth_unref(data->buflist);
  free(data);
  free(req);
}

static eth_t
__write(void)
{
  eth_args args = eth_start(3);
  eth_t stream = eth_arg2(args, uvstream_type);
  eth_t buflist = eth_arg(args);
  eth_t cb = eth_arg2(args, eth_function_type);
  /*
   * Prepare buffers:
   */
  size_t nbufs = eth_length(buflist, NULL);
  uv_buf_t *bufs = malloc(sizeof(uv_buf_t) * nbufs);
  int i = 0;
  for (eth_t it = buflist; eth_is_pair(it); it = eth_cdr(it))
  {
    eth_t str = eth_car(it);
    bufs[i].base = eth_str_cstr(str);
    bufs[i].len = eth_str_len(str);
  }
  uv_write_t *req = malloc(sizeof(uv_write_t));
  write_data *data = malloc(sizeof(write_data));
  eth_ref(data->cb = cb);
  eth_ref(data->buflist = buflist);
  data->bufs = bufs;
  req->data = data;
  /*
   * Run the job:
   */
  uv_write(req, UVSTREAM(stream)->stream, bufs, nbufs, on_write);
  eth_return(args, eth_nil);
}

void on_shutdown(uv_shutdown_t *req, int status)
{
  uv_stream_t *stream = req->handle;
  uvstream *uvs = stream->data;
  eth_reserve_stack(1);
  eth_sp[0] = status < 0 ? eth_str(uv_strerror(status)) : eth_false;
  eth_t ret = eth_apply(req->data, 1);
  if (eth_unlikely(eth_is_exn(ret)))
    eth_error("error in async callback: ~w", ret);
  eth_drop(ret);
  eth_unref(req->data);
  eth_unref(req->handle->data);
  free(req);

  //cod_vec_iter(uvs->readers, i, r, eth_unref(r.cb); eth_unref(r.acc));
  //cod_vec_destroy(uvs->readers);
  //free(uvs);
  //uv_close((uv_handle_t*)stream, (uv_close_cb)free);
}

static eth_t
__shutdown(void)
{
  eth_use_symbol(Shutdown_error);
  eth_args args = eth_start(2);
  eth_t stream = eth_arg2(args, uvstream_type);
  eth_t cb = eth_arg2(args, eth_function_type);

  if (not UVSTREAM(stream)->isshutdown)
  {
    uv_shutdown_t *req = malloc(sizeof(uv_shutdown_t));
    eth_ref(req->data = cb);
    eth_ref(stream);
    uv_shutdown(req, UVSTREAM(stream)->stream, on_shutdown);
    UVSTREAM(stream)->isshutdown = true;
    eth_return(args, eth_nil);
  }
  else
    eth_throw(args, Shutdown_error);
}

static void
on_connect(uv_connect_t *req, int status)
{
  eth_debug("connected to TCP host");
  eth_t cb = req->data;

  eth_t stream, err;
  if (status < 0)
  {
    stream = eth_nil;
    err = eth_str(uv_strerror(status));
  }
  else
  {
    uvstream *sock = malloc(sizeof(uvstream));
    eth_init_header(sock, uvstream_type);
    cod_vec_init(sock->readers);
    sock->stream = req->handle;
    sock->stream->data = sock;
    sock->isshutdown = false;
    stream = ETH(sock);
    err = eth_false;
  }

  eth_reserve_stack(2);
  eth_sp[0] = stream;
  eth_sp[1] = err;
  eth_t ret = eth_apply(cb, 2);
  if (eth_unlikely(eth_is_exn(ret)))
    eth_error("error in async callback: ~w", ret);
  eth_drop(ret);
  eth_unref(cb);
  free(req);
}

static eth_t
__tcp__connect(void)
{
  eth_args args = eth_start(3);
  eth_t host = eth_arg2(args, eth_string_type);
  eth_t port = eth_arg2(args, eth_number_type);
  eth_t cb   = eth_arg2(args, eth_function_type);

  uv_tcp_t *sock = malloc(sizeof(uv_tcp_t));
  uv_tcp_init(uv_default_loop(), sock);

  uv_connect_t *conn = malloc(sizeof(uv_connect_t));
  struct sockaddr_in dest;
  uv_ip4_addr(eth_str_cstr(host), eth_num_val(port), &dest);

  eth_ref(conn->data = cb);
  eth_debug("connect to %s at port %d", eth_str_cstr(host),
      (int)eth_num_val(port));
  uv_tcp_connect(conn, sock, (struct sockaddr*)&dest, on_connect);

  eth_return(args, eth_nil);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                Timer
 */
typedef struct {
  eth_t fun;
  eth_t acc;
} timer_data;

static void
timer_cb(uv_timer_t *handle)
{
  eth_t fun = ((timer_data*)handle->data)->fun;
  eth_t acc = ((timer_data*)handle->data)->acc;

  eth_reserve_stack(1);
  eth_dec(eth_sp[0] = acc);
  eth_t ret = eth_apply(fun, 1);

  if (eth_unlikely(eth_is_exn(ret)))
  {
    eth_error("exception in async callback: ~w", ret);
    eth_drop(ret);
    goto stop;
  }
  else if (ret == eth_false)
  {
    goto stop;
  }
  else
  {
    eth_ref(((timer_data*)handle->data)->acc = ret);
  }

  return;

stop:;
  uv_timer_stop(handle);
  void close_cb(uv_handle_t *handle)
  {
    eth_unref(((timer_data*)handle->data)->fun);
    free(handle->data);
    free(handle);
  }
  uv_close((uv_handle_t*)handle, close_cb);
}

static eth_t
_create_timer(void)
{
  eth_args args = eth_start(4);
  uint64_t timeout = eth_num_val(eth_arg2(args, eth_number_type));
  uint64_t repeat = eth_num_val(eth_arg2(args, eth_number_type));
  eth_t cb = eth_arg2(args, eth_function_type);
  eth_t acc = eth_arg(args);

  uv_timer_t *handle = malloc(sizeof(uv_timer_t));
  timer_data *data = malloc(sizeof(timer_data));
  eth_ref(data->fun = cb);
  eth_ref(data->acc = acc);
  handle->data = data;
  uv_timer_init(uv_default_loop(), handle);
  uv_timer_start(handle, timer_cb, timeout, repeat);

  eth_return(args, eth_nil);
}


static void
at_exit(void* _)
{
  eth_debug("starting libuv loop");
  while (uv_run(uv_default_loop(), UV_RUN_DEFAULT));
  int err = uv_loop_close(uv_default_loop());
  if (err != 0)
    eth_error("failed to close LibUV-loop (%s)", uv_strerror(err));
}

int
ether_module(eth_module *mod, eth_env *topenv)
{
  if (not uvfile_type)
  {
    uvfile_type = eth_create_type("uvfile");
    uvfile_type->destroy = destroy_file;
    eth_add_destructor(mod, uvfile_type, (void*)eth_destroy_type);
  }

  if (not promise_type)
  {
    promise_type = eth_create_type("promise");
    promise_type->destroy = destroy_promise;
    eth_add_destructor(mod, promise_type, (void*)eth_destroy_type);
  }

  if (not uvstream_type)
  {
    uvstream_type = eth_create_type("uvstream");
    uvstream_type->destroy = destroy_stream;
    eth_add_destructor(mod, uvstream_type, (void*)eth_destroy_type);
  }


  eth_define(mod, "__create_timer", eth_create_proc(_create_timer, 4, NULL, NULL));
  eth_define(mod, "__open", eth_create_proc(_open, 4, NULL, NULL));
  eth_define(mod, "__fs_read", eth_create_proc(__fs_read, 4, NULL, NULL));
  eth_define(mod, "__tcp__connect", eth_create_proc(__tcp__connect, 3, NULL, NULL));
  eth_define(mod, "__read_start", eth_create_proc(__read_start, 3, NULL, NULL));
  eth_define(mod, "__shutdown", eth_create_proc(__shutdown, 2, NULL, NULL));
  eth_define(mod, "__write", eth_create_proc(__write, 3, NULL, NULL));

  eth_define(mod, "__resolve", eth_create_proc(_resolve, 2, NULL, NULL));
  eth_define(mod, "__reject", eth_create_proc(_reject, 2, NULL, NULL));
  eth_define(mod, "__promise", eth_create_proc(_promise, 1, NULL, NULL));
  eth_define(mod, "__await", eth_create_proc(_await, 1, NULL, NULL));

  eth_define(mod, "o_rdonly", eth_num(O_RDONLY));
  eth_define(mod, "o_wronly", eth_num(O_WRONLY));
  eth_define(mod, "o_rdwr", eth_num(O_RDWR));
  eth_define(mod, "o_append", eth_num(O_APPEND));
  eth_define(mod, "o_async", eth_num(O_ASYNC));
  eth_define(mod, "o_cloexec", eth_num(O_CLOEXEC));
  eth_define(mod, "o_creat", eth_num(O_CREAT));
  eth_define(mod, "o_directory", eth_num(O_DIRECTORY));
  eth_define(mod, "o_dsync", eth_num(O_DSYNC));
  eth_define(mod, "o_excl", eth_num(O_EXCL));
  eth_define(mod, "o_noctty", eth_num(O_NOCTTY));
  eth_define(mod, "o_nofollow", eth_num(O_NOFOLLOW));
  eth_define(mod, "o_sync", eth_num(O_SYNC));
  eth_define(mod, "o_trunc", eth_num(O_TRUNC));

  eth_define(mod, "s_irwxu", eth_num(S_IRWXU));
  eth_define(mod, "s_irusr", eth_num(S_IRUSR));
  eth_define(mod, "s_iwusr", eth_num(S_IWUSR));
  eth_define(mod, "s_ixusr", eth_num(S_IXUSR));
  eth_define(mod, "s_irwxg", eth_num(S_IRWXG));
  eth_define(mod, "s_irgrp", eth_num(S_IRGRP));
  eth_define(mod, "s_iwgrp", eth_num(S_IWGRP));
  eth_define(mod, "s_ixgrp", eth_num(S_IXGRP));
  eth_define(mod, "s_irwxo", eth_num(S_IRWXO));
  eth_define(mod, "s_iroth", eth_num(S_IROTH));
  eth_define(mod, "s_iwoth", eth_num(S_IWOTH));
  eth_define(mod, "s_ixoth", eth_num(S_IXOTH));
  eth_define(mod, "s_isuid", eth_num(S_ISUID));
  eth_define(mod, "s_isgid", eth_num(S_ISGID));
  eth_define(mod, "s_isvtx", eth_num(S_ISVTX));

  eth_add_exit_handle(topenv, at_exit, NULL);

  if (not eth_add_module_script(mod, "./lib.eth", topenv))
    return 1;
  return 0;
}
