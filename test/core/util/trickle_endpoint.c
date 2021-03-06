/*
 *
 * Copyright 2016 gRPC authors.
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
 *
 */

#include "src/core/lib/iomgr/sockaddr.h"

#include "test/core/util/passthru_endpoint.h"

#include <inttypes.h>
#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/useful.h>
#include "src/core/lib/slice/slice_internal.h"

#define WRITE_BUFFER_SIZE (2 * 1024 * 1024)

typedef struct {
  grpc_endpoint base;
  double bytes_per_second;
  grpc_endpoint *wrapped;
  gpr_timespec last_write;

  gpr_mu mu;
  grpc_slice_buffer write_buffer;
  grpc_slice_buffer writing_buffer;
  grpc_error *error;
  bool writing;
  grpc_closure *write_cb;
} trickle_endpoint;

static void te_read(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                    grpc_slice_buffer *slices, grpc_closure *cb) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  grpc_endpoint_read(exec_ctx, te->wrapped, slices, cb);
}

static void maybe_call_write_cb_locked(grpc_exec_ctx *exec_ctx,
                                       trickle_endpoint *te) {
  if (te->write_cb != NULL && (te->error != GRPC_ERROR_NONE ||
                               te->write_buffer.length <= WRITE_BUFFER_SIZE)) {
    grpc_closure_sched(exec_ctx, te->write_cb, GRPC_ERROR_REF(te->error));
    te->write_cb = NULL;
  }
}

static void te_write(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                     grpc_slice_buffer *slices, grpc_closure *cb) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  gpr_mu_lock(&te->mu);
  GPR_ASSERT(te->write_cb == NULL);
  if (te->write_buffer.length == 0) {
    te->last_write = gpr_now(GPR_CLOCK_MONOTONIC);
  }
  for (size_t i = 0; i < slices->count; i++) {
    grpc_slice_buffer_add(&te->write_buffer,
                          grpc_slice_copy(slices->slices[i]));
  }
  te->write_cb = cb;
  maybe_call_write_cb_locked(exec_ctx, te);
  gpr_mu_unlock(&te->mu);
}

static void te_add_to_pollset(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                              grpc_pollset *pollset) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  grpc_endpoint_add_to_pollset(exec_ctx, te->wrapped, pollset);
}

static void te_add_to_pollset_set(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                                  grpc_pollset_set *pollset_set) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  grpc_endpoint_add_to_pollset_set(exec_ctx, te->wrapped, pollset_set);
}

static void te_shutdown(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep,
                        grpc_error *why) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  gpr_mu_lock(&te->mu);
  if (te->error == GRPC_ERROR_NONE) {
    te->error = GRPC_ERROR_REF(why);
  }
  maybe_call_write_cb_locked(exec_ctx, te);
  gpr_mu_unlock(&te->mu);
  grpc_endpoint_shutdown(exec_ctx, te->wrapped, why);
}

static void te_destroy(grpc_exec_ctx *exec_ctx, grpc_endpoint *ep) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  grpc_endpoint_destroy(exec_ctx, te->wrapped);
  gpr_mu_destroy(&te->mu);
  grpc_slice_buffer_destroy_internal(exec_ctx, &te->write_buffer);
  grpc_slice_buffer_destroy_internal(exec_ctx, &te->writing_buffer);
  GRPC_ERROR_UNREF(te->error);
  gpr_free(te);
}

static grpc_resource_user *te_get_resource_user(grpc_endpoint *ep) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  return grpc_endpoint_get_resource_user(te->wrapped);
}

static char *te_get_peer(grpc_endpoint *ep) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  return grpc_endpoint_get_peer(te->wrapped);
}

static int te_get_fd(grpc_endpoint *ep) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  return grpc_endpoint_get_fd(te->wrapped);
}

static void te_finish_write(grpc_exec_ctx *exec_ctx, void *arg,
                            grpc_error *error) {
  trickle_endpoint *te = arg;
  gpr_mu_lock(&te->mu);
  te->writing = false;
  grpc_slice_buffer_reset_and_unref(&te->writing_buffer);
  gpr_mu_unlock(&te->mu);
}

static const grpc_endpoint_vtable vtable = {
    te_read,     te_write,   te_add_to_pollset,    te_add_to_pollset_set,
    te_shutdown, te_destroy, te_get_resource_user, te_get_peer,
    te_get_fd};

grpc_endpoint *grpc_trickle_endpoint_create(grpc_endpoint *wrap,
                                            double bytes_per_second) {
  trickle_endpoint *te = gpr_malloc(sizeof(*te));
  te->base.vtable = &vtable;
  te->wrapped = wrap;
  te->bytes_per_second = bytes_per_second;
  te->write_cb = NULL;
  gpr_mu_init(&te->mu);
  grpc_slice_buffer_init(&te->write_buffer);
  grpc_slice_buffer_init(&te->writing_buffer);
  te->error = GRPC_ERROR_NONE;
  te->writing = false;
  return &te->base;
}

static double ts2dbl(gpr_timespec s) {
  return (double)s.tv_sec + 1e-9 * (double)s.tv_nsec;
}

size_t grpc_trickle_endpoint_trickle(grpc_exec_ctx *exec_ctx,
                                     grpc_endpoint *ep) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  gpr_mu_lock(&te->mu);
  if (!te->writing && te->write_buffer.length > 0) {
    gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
    double elapsed = ts2dbl(gpr_time_sub(now, te->last_write));
    size_t bytes = (size_t)(te->bytes_per_second * elapsed);
    // gpr_log(GPR_DEBUG, "%lf elapsed --> %" PRIdPTR " bytes", elapsed, bytes);
    if (bytes > 0) {
      grpc_slice_buffer_move_first(&te->write_buffer,
                                   GPR_MIN(bytes, te->write_buffer.length),
                                   &te->writing_buffer);
      te->writing = true;
      te->last_write = now;
      grpc_endpoint_write(
          exec_ctx, te->wrapped, &te->writing_buffer,
          grpc_closure_create(te_finish_write, te, grpc_schedule_on_exec_ctx));
      maybe_call_write_cb_locked(exec_ctx, te);
    }
  }
  size_t backlog = te->write_buffer.length;
  gpr_mu_unlock(&te->mu);
  return backlog;
}

size_t grpc_trickle_get_backlog(grpc_endpoint *ep) {
  trickle_endpoint *te = (trickle_endpoint *)ep;
  gpr_mu_lock(&te->mu);
  size_t backlog = te->write_buffer.length;
  gpr_mu_unlock(&te->mu);
  return backlog;
}
