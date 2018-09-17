/* BSD 3-Clause License
 *
 * Copyright (c) 2018, Quansight and Sameer Deshmukh
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "ruby_gumath_internal.h"

/* libxnd.so is not linked without at least one xnd symbol. */
const void *dummy = NULL;

/****************************************************************************/
/*                              Class globals                               */
/****************************************************************************/

/* Function table */
static gm_tbl_t *table = NULL;

/* Xnd type */
static  XndObject *xnd = NULL;

/* Maximum number of threads */
static int64_t max_threads = 1;

static int initialized = 0;

extern VALUE cGumath;
static VALUE cGumath_GufuncObject;

/****************************************************************************/
/*                               Error handling                             */
/****************************************************************************/

static VALUE
seterr(ndt_context_t *ctx)
{
  return rb_ndtypes_set_error(ctx);
}

/****************************************************************************/
/*                               Gufunc Object                              */
/****************************************************************************/

typedef struct {
  const gm_tbl_t *table;          /* kernel table */
  char *name;                     /* function name */
} GufuncObject;

#define GET_GUOBJ(obj, guobj_p) do {                              \
    TypedData_Get_Struct((obj), GufuncObject,                     \
                         &GufuncObject_type, (guobj_p));          \
  } while (0)
#define MAKE_GUOBJ(klass, guobj_p) TypedData_Make_Struct(klass, GufuncObject, \
                                                          &GufuncObject_type, guobj_p)
#define WRAP_GUOBJ(klass, guobj_p) TypedData_Wrap_Struct(klass,           \
                                                          &GufuncObject_type, guobj_p)
static void
GufuncObject_dfree(void *self)
{
  GufuncObject *guobj = (GufuncObject*)self;

  ndt_free(guobj->name);
}

static size_t
GufuncObject_dsize(const void *self)
{
  return sizeof(GufuncObject);
}

static const rb_data_type_t GufuncObject_type = {
  .wrap_struct_name = "GufuncObject",
  .function = {
    .dmark = NULL,
    .dfree = GufuncObject_dfree,
    .dsize = GufuncObject_dsize,
    .reserved = {0,0},
  },
  .parent = 0,
  .flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE
GufuncObject_alloc(const gm_tbl_t *table, const char *name)
{
  NDT_STATIC_CONTEXT(ctx);
  GufuncObject *guobj_p;
  VALUE guobj;

  guobj = MAKE_GUOBJ(cGumath_GufuncObject, guobj_p);
  guobj_p->table = table;
  guobj_p->name = ndt_strdup(name, &ctx);
  if (guobj_p->name == NULL) {
    seterr(&ctx);
  }

  return guobj;
}

/****************************************************************************/
/*                               Instance methods                           */
/****************************************************************************/

static VALUE
Gumath_GufuncObject_call(int argc, VALUE *argv, VALUE self)
{
  NDT_STATIC_CONTEXT(ctx);
  xnd_t stack[NDT_MAX_ARGS];
  const ndt_t *in_types[NDT_MAX_ARGS];
  gm_kernel_t kernel;
  ndt_apply_spec_t spec = ndt_apply_spec_empty;
  GufuncObject *self_p;
  VALUE x, result[NDT_MAX_ARGS];
  int i, k;
  size_t nin = argc;

  if (argc > NDT_MAX_ARGS) {
    rb_raise(rb_eArgError, "too many arguments.");
  }

  for (i = 0; i < argc; i++) {
    if (!rb_xnd_check_type(argv[i])) {
      rb_raise(rb_eArgError, "Args must be XND.");
    }

    stack[i] = *rb_xnd_const_xnd(argv[i]);
    in_types[i] = stack[i].type;
  }

  GET_GUOBJ(self, self_p);

  kernel = gm_select(&spec, self_p->table, self_p->name, in_types, argc, stack, &ctx);
  if (kernel.set == NULL) {
    seterr(&ctx);
    raise_error();
  }

  if (spec.nbroadcast > 0) {
    for (i = 0; i < argc; i++) {
      stack[i].type = spec.broadcast[i];
    }
  }

  for (i = 0; i < spec.nout; i++) {
    if (ndt_is_concrete(spec.out[i])) {
      x = rb_xnd_empty_from_type(spec.out[i]);
      if (x == NULL) {
        ndt_apply_spec_clear(&spec);
        rb_raise(rb_eNoMemError, "could not allocate empty XND object.");
      }
      result[i] = x;
      stack[nin+i] = *rb_xnd_const_xnd(x);
    }
    else {
      result[i] = NULL;
      stack[nin+i] = xnd_error;
    }
  }

#ifdef HAVE_PTHREAD_H
  if (gm_apply_thread(&kernel, stack, spec.outer_dims, spec.flags,
                      max_threads, &ctx) < 0) {
    seterr(&ctx);
    raise_error();
  }
#else
  if (gm_apply(&kernel, stack, spec.outer_dims, &ctx) < 0) {
    seterr(&ctx);
    raise_error();
  }
#endif

  for (i = 0; i < spec.nout; i++) {
    if (ndt_is_abstract(spec.out[i])) {
      ndt_del(spec.out[i]);
      VALUE x = rb_xnd_from_xnd(&stack[nin+i]);
      stack[nin+i] = xnd_error;
      if (x == NULL) {
        for (k = i+i; k < spec.nout; k++) {
          if (ndt_is_abstract(spec.out[k])) {
            xnd_del_buffer(&stack[nin+k], XND_OWN_ALL);
          }
        }
      }
      result[i] = x;
    }
  }

  if (spec.nbroadcast > 0) {
    for (i = 0; i < nin; ++i) {
      ndt_del(spec.broadcast[i]);
    }
  }

  switch(spec.nout) {
  case 0: return Qnil;
  case 1: return result[0];
  default: {
    VALUE tuple = array_new(spec.nout);
    for (i = 0; i < spec.nout; ++i) {
      rb_ary_store(tuple, i, result[i]);
    }
    return tuple;
  }
  }
}

/****************************************************************************/
/*                               Singleton methods                          */
/****************************************************************************/

static VALUE
Gumath_s_unsafe_add_kernel(int argc, VALUE *argv, VALUE klass)
{
  
}

static VALUE
Gumath_s_get_max_threads(VALUE klass)
{
  return INT2NUM(max_threads);
}

static VALUE
Gumath_s_set_max_threads(VALUE klass, VALUE threads)
{
  
}

/****************************************************************************/
/*                                   Other functions                        */
/****************************************************************************/

static void
init_max_threads(void)
{
  max_threads = NUM2INT(rb_funcall(rb_const_get(rb_cObject, rb_intern("Etc")),
                               rb_intern("nprocessors"), NULL, 0));
}

/****************************************************************************/
/*                                   C-API                                  */
/****************************************************************************/

struct map_args {
  VALUE module;
  const gm_tbl_t *table;
};

/* Function called by libgumath that will load function kernels from function
   table of type gm_tbl_t into a Ruby module. Don't call this directly use
   rb_gumath_add_functions.
 */
int
add_function(const gm_func_t *f, void *args)
{
  struct map_args *a = (struct map_args *)args;
  VALUE func, func_hash;

  func = GufuncObject_alloc(a->table, f->name);
  if (func == NULL) {
    return -1;
  }

  func_hash = rb_ivar_get(a->module, GUMATH_FUNCTION_HASH);
  rb_hash_aset(func_hash, ID2SYM(rb_intern(f->name)), func);

  return 0;
}

/* C API call for adding functions from a gumath kernel table to  */
int
rb_gumath_add_functions(VALUE module, const gm_tbl_t *tbl)
{
  struct map_args args = {module, tbl};

  if (gm_tbl_map(tbl, add_function, &args) < 0) {
    return -1;
  }
}

void Init_ruby_gumath(void)
{
  NDT_STATIC_CONTEXT(ctx);

  if (!initialized) {
    dummy = &xnd_error;

    gm_init();

    if (!xnd_exists()) {
      rb_raise(rb_eLoadError, "Need XND for gumath.");
    }

    if (!ndt_exists()) {
      rb_raise(rb_eLoadError, "Need NDT for gumath.");
    }

    table = gm_tbl_new(&ctx);
    if (table == NULL) {
      seterr(&ctx);
      raise_error();
    }

    init_max_threads();

    initialized = 1;
  }

  //  xnd = rb_xnd_get_type();
  if (xnd == NULL) {
    /* TODO: raise error here. */
  }
  
  cGumath = rb_define_class("Gumath", rb_cObject);
  cGumath_GufuncObject = rb_define_class_under(cGumath, "GufuncObject", rb_cObject);
    
  /* Class: Gumath */
  
  /* Singleton methods */
  rb_define_singleton_method(cGumath, "unsafe_add_kernel", Gumath_s_unsafe_add_kernel, -1);
  rb_define_singleton_method(cGumath, "get_max_threads", Gumath_s_get_max_threads, 0);
  rb_define_singleton_method(cGumath, "set_max_threads", Gumath_s_set_max_threads, 1);

  /* Class: Gumath::GufuncObject */

  /* Instance methods */
  rb_define_method(cGumath_GufuncObject, "call", Gumath_GufuncObject_call,-1);
  
  Init_gumath_functions();
}
